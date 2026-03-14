#include <stdio.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <button.h>

static const char *TAG = "button_example";

static const char *states[] =
{
    [BUTTON_PRESSED]      = "pressed",
    [BUTTON_RELEASED]     = "released",
    [BUTTON_CLICKED]      = "clicked",
    [BUTTON_PRESSED_LONG] = "pressed long",
};

static button_handle_t btn1, btn2;

static void on_button(const button_event_t *event, void *ctx)
{
    ESP_LOGI(TAG, "%s button %s", event->sender == btn1 ? "First" : "Second", states[event->type]);
}

void app_main()
{
    // First button connected between GPIO and GND
    // pressed logic level 0, no autorepeat, interrupt mode
    button_config_t config1 = BUTTON_DEFAULT_CONFIG();
    config1.gpio = CONFIG_EXAMPLE_BUTTON1_GPIO;
    config1.mode = BUTTON_MODE_INTERRUPT;
    config1.pressed_level = 0;
    config1.autorepeat = false;
    config1.callback = on_button;

    // Second button connected between GPIO and +3.3V
    // pressed logic level 1, autorepeat enabled
    button_config_t config2 = BUTTON_DEFAULT_CONFIG();
    config2.gpio = CONFIG_EXAMPLE_BUTTON2_GPIO;
    config2.pressed_level = 1;
    config2.autorepeat = true;
    config2.callback = on_button;

    // Install GPIO ISR service (required before creating interrupt-mode buttons)
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    ESP_ERROR_CHECK(button_create(&config1, &btn1));
    ESP_ERROR_CHECK(button_create(&config2, &btn2));
}
