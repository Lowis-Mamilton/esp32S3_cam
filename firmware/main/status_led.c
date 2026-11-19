#include "status_led.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

static const char *TAG = "led";

#define LED_GPIO 48
#define MIN_INTERVAL_US (100 * 1000)

static led_strip_handle_t s_strip;
static bool s_written;
static status_led_mode_t s_mode;
static uint8_t s_brightness;
static int64_t s_last_us;

esp_err_t status_led_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t e = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 init failed: %s", esp_err_to_name(e));
        s_strip = NULL;
    }
    return e;
}

void status_led_set(status_led_mode_t mode, uint8_t brightness)
{
    if (!s_strip) return;
    if (s_written && s_mode == STATUS_LED_ERROR) return; /* errors stay visible */
    if (brightness > 100) brightness = 100;
    int64_t now = esp_timer_get_time();
    if (s_written && mode == s_mode && brightness == s_brightness) return;
    if (s_written && mode != STATUS_LED_ERROR && now - s_last_us < MIN_INTERVAL_US) return;

    uint32_t v = (uint32_t)brightness * 255u / 100u;
    uint32_t r = 0, g = 0, b = 0;
    switch (mode) {
    case STATUS_LED_IDLE: b = v; break;
    case STATUS_LED_RESULT: g = v; break;
    case STATUS_LED_ERROR: r = v ? v : 255u / 2; break; /* an error shows even with brightness 0 */
    }
    if (led_strip_set_pixel(s_strip, 0, r, g, b) == ESP_OK) led_strip_refresh(s_strip);
    s_written = true;
    s_mode = mode;
    s_brightness = brightness;
    s_last_us = now;
}
