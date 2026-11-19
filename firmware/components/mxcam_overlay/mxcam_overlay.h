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
/* Straight line (Bresenham) from (x0,y0) to (x1,y1), both ends included. Each
 * point is a thickness x thickness square centred on it (top-left at
 * p - thickness/2). The segment is first clipped to the canvas (plus the pen
 * margin), so far-away or huge coordinates cost nothing and never write
 * outside; the clipped path may differ from the unclipped one by one pixel.
 * thickness <= 0 draws nothing. */
void mxo_draw_line(mxo_canvas_t *c, int x0, int y0, int x1, int y1, int thickness, uint16_t color);
/* 8x8 font, printable ASCII only (others blank). Returns text width in pixels (8 per char). */
int  mxo_draw_text(mxo_canvas_t *c, int x, int y, const char *ascii, uint16_t fg, uint16_t bg);

/*
 * Mirrored panel (ADR-0010): the TFT shows the frame mirrored left-right, so
 * text that must read correctly on the panel is drawn mirrored into the
 * frame. (x, y) is the text's top-left corner AS SEEN ON THE PANEL; the result
 * is exactly the left-right mirror of mxo_draw_text(c, x, y, ...). Same
 * clipping and return value (8 px per char).
 */
int  mxo_draw_text_mirrored(mxo_canvas_t *c, int x, int y, const char *ascii, uint16_t fg, uint16_t bg);
/* Frame x of a span of width w that appears at panel x (and vice versa). */
int  mxo_mirror_x(const mxo_canvas_t *c, int x, int w);

#ifdef __cplusplus
}
#endif

#endif
