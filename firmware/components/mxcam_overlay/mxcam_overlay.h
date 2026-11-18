#ifndef MXCAM_OVERLAY_H
#define MXCAM_OVERLAY_H

/* Pure C99 drawing on an RGB565 buffer. All operations clip to the canvas. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Colour byte order: camera frames are sent to the ILI9341 without swapping,
 * so the in-memory byte order is big-endian RGB565 (high byte first). On the
 * little-endian ESP32-S3 the uint16_t value is therefore the usual RGB565
 * value with its two bytes swapped. MXO_RGB565 does that swap for constants.
 */
#define MXO_RGB565(v) ((uint16_t)((((v) & 0xFF) << 8) | (((v) >> 8) & 0xFF)))

#define MXO_WHITE MXO_RGB565(0xFFFF)
#define MXO_BLACK MXO_RGB565(0x0000)
#define MXO_GREEN MXO_RGB565(0x07E0)
#define MXO_RED   MXO_RGB565(0xF800)

typedef struct {
    uint16_t *pixels; /* width*height pixels, row-major */
    int width, height;
} mxo_canvas_t;

void mxo_fill_rect(mxo_canvas_t *c, int x, int y, int w, int h, uint16_t color);
/* Hollow rectangle; the border is drawn inside the (x,y,w,h) box. */
void mxo_draw_rect(mxo_canvas_t *c, int x, int y, int w, int h, int thickness, uint16_t color);
/* 8x8 font, printable ASCII only (others blank). Returns text width in pixels (8 per char). */
int  mxo_draw_text(mxo_canvas_t *c, int x, int y, const char *ascii, uint16_t fg, uint16_t bg);

#ifdef __cplusplus
}
#endif

#endif
