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
 * @file button.h
 * @defgroup button button
 * @{
 *
 * ESP-IDF driver for simple GPIO buttons.
 *
 * Supports anti-jitter, auto repeat, long press.
 *
 * Copyright (c) 2021 Ruslan V. Uss <unclerus@gmail.com>
 *
 * MIT Licensed as described in the file LICENSE
 */
#ifndef __COMPONENTS_BUTTON_H__
#define __COMPONENTS_BUTTON_H__

#include <stdint.h>
#include <stdbool.h>
#include <driver/gpio.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque button handle
 */
typedef struct button *button_handle_t;

/**
 * Button detection mode
 */
typedef enum
{
    BUTTON_MODE_POLL = 0,    //!< Periodic timer polling (default)
    BUTTON_MODE_INTERRUPT,   //!< GPIO interrupt with debounce timer
} button_mode_t;

/**
 * Button states/events
 *
 * When autorepeat is disabled, the event sequence for a short press is:
 *   PRESSED -> RELEASED -> CLICKED
 * and for a long press:
 *   PRESSED -> PRESSED_LONG -> RELEASED
 *
 * When autorepeat is enabled, long press detection is disabled. Instead,
 * holding the button generates repeated CLICKED events:
 *   PRESSED -> CLICKED -> CLICKED -> ... -> RELEASED
 */
typedef enum
{
    BUTTON_PRESSED = 0,
    BUTTON_RELEASED,
    BUTTON_CLICKED,
    BUTTON_PRESSED_LONG,
} button_state_t;

/**
 * Button event
 */
typedef struct
{
    button_state_t type;       //!< Event type
    button_handle_t sender;    //!< Button handle
} button_event_t;

/**
 * Callback prototype
 *
 * The callback is always invoked from the ESP timer task context.
 * It must not block or call back into the library.
 *
 * @param event  Button event
 * @param ctx    User context
 */
typedef void (*button_event_cb_t)(const button_event_t *event, void *ctx);

/**
 * Button configuration
 */
typedef struct
{
    gpio_num_t gpio;                   //!< GPIO pin number
    button_mode_t mode;                //!< Detection mode (poll or interrupt)
    uint8_t pressed_level;             //!< Logic level when button is pressed (0 or 1)
    bool enable_internal_pull;         //!< Enable internal pull-up/pull-down resistor
    bool autorepeat;                   //!< Enable autorepeat (mutually exclusive with long press)
    uint32_t dead_time_us;             //!< Dead time after press / debounce delay (microseconds)
    uint32_t long_press_time_us;       //!< Long press threshold in microseconds (ignored when autorepeat is enabled)
    uint32_t autorepeat_timeout_us;    //!< Time before autorepeat starts, in microseconds
    uint32_t autorepeat_interval_us;   //!< Autorepeat interval in microseconds
    uint32_t poll_interval_us;         //!< Polling / monitoring interval (microseconds)
    button_event_cb_t callback;        //!< Event callback (required)
    void *callback_ctx;                //!< User context passed to callback
} button_config_t;

#define BUTTON_DEFAULT_CONFIG() { \
    .gpio = GPIO_NUM_NC, \
    .mode = BUTTON_MODE_POLL, \
    .pressed_level = 0, \
    .enable_internal_pull = true, \
    .autorepeat = false, \
    .dead_time_us = CONFIG_BUTTON_DEAD_TIME * 1000, \
    .long_press_time_us = CONFIG_BUTTON_LONG_PRESS_TIMEOUT * 1000, \
    .autorepeat_timeout_us = CONFIG_BUTTON_AUTOREPEAT_TIMEOUT * 1000, \
    .autorepeat_interval_us = CONFIG_BUTTON_AUTOREPEAT_INTERVAL * 1000, \
    .poll_interval_us = CONFIG_BUTTON_POLL_TIMEOUT * 1000, \
    .callback = NULL, \
    .callback_ctx = NULL, \
}

/**
 * @brief Create a new button
 *
 * When using ::BUTTON_MODE_INTERRUPT, the GPIO ISR service must be installed
 * by the caller before creating any interrupt-mode buttons (via
 * `gpio_install_isr_service()`).
 *
 * @param config Button configuration
 * @param[out] handle Button handle, populated on success
 * @return `ESP_OK` on success
 */
esp_err_t button_create(const button_config_t *config, button_handle_t *handle);

/**
 * @brief Delete a button
 *
 * @param handle Button handle
 * @return `ESP_OK` on success
 */
esp_err_t button_delete(button_handle_t handle);

#ifdef __cplusplus
}
#endif

/**@}*/

#endif /* __COMPONENTS_BUTTON_H__ */
