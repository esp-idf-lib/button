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
#include <esp_log.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char *TAG = "button";

// One button per pin: the GPIO driver keeps a single interrupt handler per pin,
// so a second button on the same pin would silently take the handler over and
// the first one would stop seeing presses
static bool pins_in_use[GPIO_NUM_MAX];
static portMUX_TYPE pins_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t take_pin(gpio_num_t pin)
{
    portENTER_CRITICAL(&pins_mux);
    bool busy = pins_in_use[pin];
    if (!busy)
        pins_in_use[pin] = true;
    portEXIT_CRITICAL(&pins_mux);

    if (busy)
    {
        ESP_LOGE(TAG, "GPIO %d is already used by another button", pin);
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static void give_pin(gpio_num_t pin)
{
    portENTER_CRITICAL(&pins_mux);
    pins_in_use[pin] = false;
    portEXIT_CRITICAL(&pins_mux);
}

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
        esp_err_t err;

        if (btn->state == BUTTON_RELEASED)
        {
            // Button released (or false trigger): stop monitoring and re-enable interrupt
            if (btn->monitoring)
            {
                err = esp_timer_stop(btn->timer);
                if (err != ESP_OK)
                    ESP_LOGE(TAG, "Could not stop the timer: %s", esp_err_to_name(err));
                btn->monitoring = false;
            }
            gpio_intr_enable(btn->config.gpio);
        }
        else if (!btn->monitoring)
        {
            // Debounce confirmed press: switch to periodic monitoring
            err = esp_timer_start_periodic(btn->timer, btn->config.poll_interval_us);
            if (err != ESP_OK)
                ESP_LOGE(TAG, "Could not start the timer: %s", esp_err_to_name(err));
            btn->monitoring = err == ESP_OK;
        }
    }

    xSemaphoreGive(btn->lock);
}

////////////////////////////////////////////////////////////////////////////////
// Interrupt mode helpers

static void button_isr_handler(void *arg)
{
    button_handle_t btn = (button_handle_t)arg;
    gpio_intr_disable(btn->config.gpio);
    esp_timer_start_once(btn->timer, 0);
}

////////////////////////////////////////////////////////////////////////////////

static esp_err_t check_config(const button_config_t *config)
{
    if (!GPIO_IS_VALID_GPIO(config->gpio))
        return ESP_ERR_INVALID_ARG;
    if (config->mode != BUTTON_MODE_POLL && config->mode != BUTTON_MODE_INTERRUPT)
        return ESP_ERR_INVALID_ARG;
    if (config->pressed_level > 1)
        return ESP_ERR_INVALID_ARG;
    if (!config->poll_interval_us)
        return ESP_ERR_INVALID_ARG;
    if (config->autorepeat && !config->autorepeat_interval_us)
        return ESP_ERR_INVALID_ARG;
    if (!config->callback)
        return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static void release_pin(const button_config_t *config)
{
    gpio_intr_disable(config->gpio);
    gpio_set_intr_type(config->gpio, GPIO_INTR_DISABLE);
    gpio_set_pull_mode(config->gpio, GPIO_FLOATING);
}

esp_err_t button_create(const button_config_t *config, button_handle_t *handle)
{
    CHECK_ARG(handle);
    *handle = NULL;

    if (!config)
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = check_config(config);
    if (err != ESP_OK)
        return err;

    err = take_pin(config->gpio);
    if (err != ESP_OK)
        return err;

    button_handle_t btn = calloc(1, sizeof(struct button));
    if (!btn)
    {
        give_pin(config->gpio);
        return ESP_ERR_NO_MEM;
    }

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
    release_pin(&btn->config);
    vSemaphoreDelete(btn->lock);
fail:
    give_pin(btn->config.gpio);
    free(btn);
    return err;
}

esp_err_t button_delete(button_handle_t handle)
{
    CHECK_ARG(handle);

    // Remove the handler first: after that the ISR cannot arm the timer again
    if (handle->config.mode == BUTTON_MODE_INTERRUPT)
    {
        gpio_intr_disable(handle->config.gpio);
        gpio_isr_handler_remove(handle->config.gpio);
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
    // esp_timer_stop() only unarms the timer while the callback may be running
    // or about to run, so wait for it before the handle is freed
    esp_timer_stop_blocking(handle->timer, portMAX_DELAY);
#else
    esp_timer_stop(handle->timer);
#endif

    // An interrupt that was already in service may have armed the timer after
    // the stop above; esp_timer_delete() refuses to delete an armed timer
    esp_err_t err;
    do
    {
        esp_timer_stop(handle->timer);
        err = esp_timer_delete(handle->timer);
    }
    while (err == ESP_ERR_INVALID_STATE);

    // Drain a callback that has already taken the lock
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->monitoring = false;
    xSemaphoreGive(handle->lock);

    release_pin(&handle->config);
    give_pin(handle->config.gpio);

    vSemaphoreDelete(handle->lock);

    free(handle);
    return ESP_OK;
}
