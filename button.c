/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Ruslan V. Uss <unclerus@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file button.c
 *
 * ESP-IDF driver for simple GPIO buttons.
 *
 * Supports anti-jitter, autorepeat, long press.
 *
 * Copyright (c) 2021 Ruslan V. Uss <unclerus@gmail.com>
 *
 * MIT Licensed as described in the file LICENSE
 */
#include "button.h"
#include <stdlib.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>


#define CHECK_ARG(VAL) do { if (!(VAL)) return ESP_ERR_INVALID_ARG; } while (0)

struct button
{
    button_config_t config;
    esp_timer_handle_t timer;
    SemaphoreHandle_t lock;
    button_state_t state;
    uint32_t pressed_time;
    uint32_t repeating_time;
    bool monitoring;              // interrupt mode: true when esp_timer periodic is running
};

static inline void emit_event(button_handle_t btn, button_state_t type)
{
    button_event_t ev = { .type = type, .sender = btn };
    btn->config.callback(&ev, btn->config.callback_ctx);
}

static void poll_button(button_handle_t btn)
{
    if (btn->state == BUTTON_PRESSED && btn->pressed_time < btn->config.dead_time_us)
    {
        // Dead time, ignore all
        btn->pressed_time += btn->config.poll_interval_us;
        return;
    }

    if (gpio_get_level(btn->config.gpio) == btn->config.pressed_level)
    {
        // button is pressed
        if (btn->state == BUTTON_RELEASED)
        {
            // pressing just started, reset pressing/repeating time and run callback
            btn->state = BUTTON_PRESSED;
            btn->pressed_time = 0;
            btn->repeating_time = 0;
            emit_event(btn, BUTTON_PRESSED);
            return;
        }
        // increment pressing time
        btn->pressed_time += btn->config.poll_interval_us;

        // check autorepeat
        if (btn->config.autorepeat)
        {
            // check autorepeat timeout
            if (btn->pressed_time < btn->config.autorepeat_timeout_us)
                return;
            // increment repeating time
            btn->repeating_time += btn->config.poll_interval_us;

            if (btn->repeating_time >= btn->config.autorepeat_interval_us)
            {
                // reset repeating time and run callback
                btn->repeating_time = 0;
                emit_event(btn, BUTTON_CLICKED);
            }
            return;
        }

        if (btn->state == BUTTON_PRESSED && btn->pressed_time >= btn->config.long_press_time_us)
        {
            // button pressed long time, change state and run callback
            btn->state = BUTTON_PRESSED_LONG;
            emit_event(btn, BUTTON_PRESSED_LONG);
        }
    }
    else if (btn->state != BUTTON_RELEASED)
    {
        // button released
        bool clicked = btn->state == BUTTON_PRESSED
                       && !(btn->config.autorepeat && btn->pressed_time >= btn->config.autorepeat_timeout_us);
        btn->state = BUTTON_RELEASED;
        emit_event(btn, BUTTON_RELEASED);
        if (clicked)
        {
            emit_event(btn, BUTTON_CLICKED);
        }
    }
}

static void button_timer_handler(void *arg)
{
    button_handle_t btn = (button_handle_t)arg;
    xSemaphoreTake(btn->lock, portMAX_DELAY);

    poll_button(btn);

    if (btn->config.mode == BUTTON_MODE_INTERRUPT)
    {
        if (btn->state == BUTTON_RELEASED)
        {
            // Button released (or false trigger): stop monitoring and re-enable interrupt
            if (btn->monitoring)
            {
                esp_timer_stop(btn->timer);
                btn->monitoring = false;
            }
            gpio_intr_enable(btn->config.gpio);
        }
        else if (!btn->monitoring)
        {
            // Debounce confirmed press: switch to periodic monitoring
            esp_timer_start_periodic(btn->timer, btn->config.poll_interval_us);
            btn->monitoring = true;
        }
    }

    xSemaphoreGive(btn->lock);
}

////////////////////////////////////////////////////////////////////////////////
// Interrupt mode helpers

static void IRAM_ATTR button_isr_handler(void *arg)
{
    button_handle_t btn = (button_handle_t)arg;
    gpio_intr_disable(btn->config.gpio);
    esp_timer_start_once(btn->timer, 0);
}

////////////////////////////////////////////////////////////////////////////////

esp_err_t button_create(const button_config_t *config, button_handle_t *handle)
{
    CHECK_ARG(handle && config && config->callback);

    esp_err_t err;

    button_handle_t btn = calloc(1, sizeof(struct button));
    if (!btn)
        return ESP_ERR_NO_MEM;

    btn->config = *config;
    btn->state = BUTTON_RELEASED;

    btn->lock = xSemaphoreCreateMutex();
    if (!btn->lock)
    {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    err = gpio_set_direction(btn->config.gpio, GPIO_MODE_INPUT);
    if (err != ESP_OK)
        goto fail_lock;

    if (btn->config.enable_internal_pull)
    {
        err = gpio_set_pull_mode(btn->config.gpio, btn->config.pressed_level ? GPIO_PULLDOWN_ONLY : GPIO_PULLUP_ONLY);
        if (err != ESP_OK)
            goto fail_lock;
    }

    const esp_timer_create_args_t timer_args =
    {
        .name = "__button__",
        .arg = btn,
        .callback = button_timer_handler,
        .dispatch_method = ESP_TIMER_TASK
    };

    err = esp_timer_create(&timer_args, &btn->timer);
    if (err != ESP_OK)
        goto fail_lock;

    if (btn->config.mode == BUTTON_MODE_INTERRUPT)
    {
        err = gpio_set_intr_type(btn->config.gpio, GPIO_INTR_ANYEDGE);
        if (err != ESP_OK)
            goto fail_timer;

        err = gpio_isr_handler_add(btn->config.gpio, button_isr_handler, btn);
        if (err != ESP_OK)
            goto fail_timer;
    }
    else
    {
        // Poll mode: start periodic timer immediately
        err = esp_timer_start_periodic(btn->timer, btn->config.poll_interval_us);
        if (err != ESP_OK)
            goto fail_timer;
    }

    *handle = btn;

    return ESP_OK;

fail_timer:
    esp_timer_delete(btn->timer);
fail_lock:
    vSemaphoreDelete(btn->lock);
fail:
    free(btn);
    return err;
}

esp_err_t button_delete(button_handle_t handle)
{
    CHECK_ARG(handle);

    if (handle->config.mode == BUTTON_MODE_INTERRUPT)
    {
        gpio_intr_disable(handle->config.gpio);
        gpio_isr_handler_remove(handle->config.gpio);
    }

    esp_timer_stop(handle->timer);

    // Drain any in-flight callback
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->monitoring = false;
    xSemaphoreGive(handle->lock);

    esp_timer_delete(handle->timer);
    vSemaphoreDelete(handle->lock);

    free(handle);
    return ESP_OK;
}
