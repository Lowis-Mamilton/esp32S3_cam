#include "mxcam_overlay.h"
#include "font8x8.h"

#include <stddef.h>

static int canvas_ok(const mxo_canvas_t *c)
{
    return c && c->pixels && c->width > 0 && c->height > 0;
}

void mxo_fill_rect(mxo_canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    if (!canvas_ok(c) || w <= 0 || h <= 0) return;
    if (x >= c->width || y >= c->height) return;
    if (x < 0) { if (w <= -x) return; w += x; x = 0; }
    if (y < 0) { if (h <= -y) return; h += y; y = 0; }
    if (w > c->width - x) w = c->width - x;
    if (h > c->height - y) h = c->height - y;
    for (int row = 0; row < h; row++) {
        uint16_t *p = c->pixels + (size_t)(y + row) * (size_t)c->width + (size_t)x;
        for (int i = 0; i < w; i++) p[i] = color;
    }
}

void mxo_draw_rect(mxo_canvas_t *c, int x, int y, int w, int h, int thickness, uint16_t color)
{
    if (!canvas_ok(c) || w <= 0 || h <= 0 || thickness <= 0) return;
    if (thickness >= (w + 1) / 2 || thickness >= (h + 1) / 2) {
        mxo_fill_rect(c, x, y, w, h, color); /* no interior left */
        return;
    }
    mxo_fill_rect(c, x, y, w, thickness, color);                                              /* top */
    mxo_fill_rect(c, x, y + h - thickness, w, thickness, color);                              /* bottom */
    mxo_fill_rect(c, x, y + thickness, thickness, h - 2 * thickness, color);                  /* left */
    mxo_fill_rect(c, x + w - thickness, y + thickness, thickness, h - 2 * thickness, color);  /* right */
}

int mxo_draw_text(mxo_canvas_t *c, int x, int y, const char *ascii, uint16_t fg, uint16_t bg)
{
    if (!ascii) return 0;
    int width = 0;
    for (const unsigned char *s = (const unsigned char *)ascii; *s; s++, width += 8) {
        int cx = x + width;
        if (!canvas_ok(c) || cx >= c->width || cx <= -8 || y >= c->height || y <= -8) continue;
        const unsigned char *glyph = (*s >= 0x20 && *s <= 0x7E) ? mxo_font8x8[*s - 0x20] : NULL;
        for (int row = 0; row < 8; row++) {
            int py = y + row;
            if (py < 0 || py >= c->height) continue;
            unsigned bits = glyph ? glyph[row] : 0u;
            for (int col = 0; col < 8; col++) {
                int px = cx + col;
                if (px < 0 || px >= c->width) continue;
                c->pixels[(size_t)py * (size_t)c->width + (size_t)px] = ((bits >> col) & 1u) ? fg : bg;
            }
        }
    }
    return width;
}
