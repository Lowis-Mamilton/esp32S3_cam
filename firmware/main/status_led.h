/* WS2812 status LED on GPIO48 (docs/SPEC.md 4): blue = idle, green = the
 * current frame has results, red = init error. */
#ifndef MXCAM_STATUS_LED_H
#define MXCAM_STATUS_LED_H

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    STATUS_LED_IDLE,   /* blue */
    STATUS_LED_RESULT, /* green */
    STATUS_LED_ERROR,  /* red; sticky: later calls cannot leave it */
} status_led_mode_t;

esp_err_t status_led_init(void);

/* brightness is the config value 0-100 (0 = off). The LED is rewritten only
 * when mode or brightness changed, at most every 100 ms (ERROR immediately).
 * Call from one task at a time; a no-op when init failed. */
void status_led_set(status_led_mode_t mode, uint8_t brightness);

#endif
