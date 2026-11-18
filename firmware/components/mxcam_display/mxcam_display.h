#ifndef MXCAM_DISPLAY_H
#define MXCAM_DISPLAY_H

#include <stdint.h>
#include "esp_err.h"

#define MXCAM_LCD_W 320
#define MXCAM_LCD_H 240

/* ILI9341 on SPI2, landscape 320x240. */
esp_err_t mxcam_display_init(void);
/*
 * Push a full 320x240 RGB565 frame. Pixels are sent as-is (no byte swap): the
 * buffer must already be in the panel byte order (what the camera produces).
 * The source may live in PSRAM; it is copied in strips through internal
 * DMA-capable bounce buffers. Returns once the last strip is queued.
 */
esp_err_t mxcam_display_draw(const uint16_t *pixels);

#endif
