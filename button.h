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
 * The callback is invoked from the ESP timer task context, while the internal
 * mutex of the button is held.
 *
 * It must not block, and it must not call ::button_delete() or ::button_create():
 * the mutex is not recursive, so such a call deadlocks. Any delay in the callback
 * also delays every other ESP timer in the system, because they share one task.
 *
 * @param event  Button event
 * @param ctx    User context
 */
typedef void (*button_event_cb_t)(const button_event_t *event, void *ctx);

/**
 * Button configuration
 *
 * ::button_create() validates the fields: the GPIO must exist, the mode must be
 * one of ::BUTTON_MODE_POLL or ::BUTTON_MODE_INTERRUPT, ::pressed_level must be
 * 0 or 1, ::poll_interval_us must be non-zero and, when ::autorepeat is enabled,
 * ::autorepeat_interval_us must be non-zero as well.
 */
typedef struct
{
    gpio_num_t gpio;                   //!< GPIO pin number
    button_mode_t mode;                //!< Detection mode (poll or interrupt)
    uint8_t pressed_level;             //!< Logic level when button is pressed (0 or 1)
    bool enable_internal_pull;         //!< Enable internal pull-up/pull-down resistor
    bool autorepeat;                   //!< Enable autorepeat (mutually exclusive with long press)
    uint32_t dead_time_us;             //!< Debounce time: samples taken right after a press are ignored (microseconds)
    uint32_t long_press_time_us;       //!< Long press threshold in microseconds (ignored when autorepeat is enabled)
    uint32_t autorepeat_timeout_us;    //!< Time before autorepeat starts, in microseconds
    uint32_t autorepeat_interval_us;   //!< Autorepeat interval in microseconds
    uint32_t poll_interval_us;         //!< Period of the internal timer and the unit of the timings, in microseconds
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
 * The configuration is validated before the GPIO is touched. On failure @p handle
 * is set to NULL and the pin is left as it was found.
 *
 * One pin can serve one button at a time: the GPIO driver stores a single
 * interrupt handler per pin, so a second button on the same pin would take the
 * handler over and the first one would stop seeing presses. A second attempt on
 * a pin that is already taken fails with `ESP_ERR_INVALID_STATE`; the pin becomes
 * free again after ::button_delete().
 *
 * When using ::BUTTON_MODE_INTERRUPT, the GPIO ISR service must be installed
 * by the caller before creating any interrupt-mode buttons (via
 * `gpio_install_isr_service()`), otherwise the call fails with
 * `ESP_ERR_INVALID_STATE`.
 *
 * @param config Button configuration
 * @param[out] handle Button handle, set to NULL on failure
 * @return
 *      - `ESP_OK` on success
 *      - `ESP_ERR_INVALID_ARG` if the configuration is invalid
 *      - `ESP_ERR_NO_MEM` if the handle or the timer cannot be allocated
 *      - `ESP_ERR_INVALID_STATE` if the GPIO ISR service is not installed or the
 *        pin already serves another button
 */
esp_err_t button_create(const button_config_t *config, button_handle_t *handle);

/**
 * @brief Delete a button
 *
 * The interrupt handler is removed, the timer is stopped and the pin is released
 * back to a floating input, so the button no longer consumes the GPIO and another
 * button may use the pin.
 *
 * On ESP-IDF 6.1 and later the function waits for a timer callback that has
 * already been dispatched. On older versions `esp_timer_stop()` cannot do that:
 * the callback may still be running while the handle is freed, so the button
 * must not be deleted concurrently with its own events. Either delete the button
 * from a task that is not the callback, or make sure that no event can arrive
 * at that moment (for example, by disabling the button beforehand).
 *
 * @param handle Button handle
 * @return
 *      - `ESP_OK` on success
 *      - `ESP_ERR_INVALID_ARG` if @p handle is NULL
 */
esp_err_t button_delete(button_handle_t handle);

#ifdef __cplusplus
}
#endif

/**@}*/

#endif /* __COMPONENTS_BUTTON_H__ */
