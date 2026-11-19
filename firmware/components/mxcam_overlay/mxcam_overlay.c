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

/* Liang-Barsky clip of p0->p1 against [lo_x,hi_x] x [lo_y,hi_y]; false when
 * nothing is left. Doubles: the input may be any int (no overflow). */
static int clip_segment(double *x0, double *y0, double *x1, double *y1,
                        double lo_x, double lo_y, double hi_x, double hi_y)
{
    double dx = *x1 - *x0, dy = *y1 - *y0, t0 = 0.0, t1 = 1.0;
    const double p[4] = { -dx, dx, -dy, dy };
    const double q[4] = { *x0 - lo_x, hi_x - *x0, *y0 - lo_y, hi_y - *y0 };
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0) return 0; /* parallel and outside */
            continue;
        }
        double t = q[i] / p[i];
        if (p[i] < 0.0) { if (t > t1) return 0; if (t > t0) t0 = t; }
        else            { if (t < t0) return 0; if (t < t1) t1 = t; }
    }
    double ox = *x0, oy = *y0;
    if (t1 < 1.0) { *x1 = ox + t1 * dx; *y1 = oy + t1 * dy; } /* untouched ends stay exact */
    if (t0 > 0.0) { *x0 = ox + t0 * dx; *y0 = oy + t0 * dy; }
    return 1;
}

static int round_d(double v) { return (int)(v < 0.0 ? v - 0.5 : v + 0.5); }

void mxo_draw_line(mxo_canvas_t *c, int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    if (!canvas_ok(c) || thickness <= 0) return;
    /* A pen at p covers [p - t/2, p - t/2 + t - 1]; keep points whose pen can touch the canvas. */
    double lo = -(double)(thickness - thickness / 2 - 1), half = (double)(thickness / 2);
    double fx0 = x0, fy0 = y0, fx1 = x1, fy1 = y1;
    if (!clip_segment(&fx0, &fy0, &fx1, &fy1, lo, lo,
                      (double)(c->width - 1) + half, (double)(c->height - 1) + half)) return;
    int ax = round_d(fx0), ay = round_d(fy0), bx = round_d(fx1), by = round_d(fy1);
    int dx = bx > ax ? bx - ax : ax - bx, sx = ax < bx ? 1 : -1;
    int dy = by > ay ? ay - by : by - ay, sy = ay < by ? 1 : -1; /* dy <= 0 */
    int err = dx + dy;
    for (;;) {
        if (thickness == 1) {
            if (ax >= 0 && ax < c->width && ay >= 0 && ay < c->height)
                c->pixels[(size_t)ay * (size_t)c->width + (size_t)ax] = color;
        } else {
            mxo_fill_rect(c, ax - thickness / 2, ay - thickness / 2, thickness, thickness, color);
        }
        if (ax == bx && ay == by) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; ax += sx; }
        if (e2 <= dx) { err += dx; ay += sy; }
    }
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

int mxo_draw_text_mirrored(mxo_canvas_t *c, int x, int y, const char *ascii, uint16_t fg, uint16_t bg)
{
    if (!ascii) return 0;
    int width = 0;
    for (const unsigned char *s = (const unsigned char *)ascii; *s; s++, width += 8) {
        if (!canvas_ok(c) || y >= c->height || y <= -8) continue;
        const unsigned char *glyph = (*s >= 0x20 && *s <= 0x7E) ? mxo_font8x8[*s - 0x20] : NULL;
        for (int row = 0; row < 8; row++) {
            int py = y + row;
            if (py < 0 || py >= c->height) continue;
            unsigned bits = glyph ? glyph[row] : 0u;
            for (int col = 0; col < 8; col++) {
                /* panel column x + width + col is frame column width-1-(...) */
                int px = c->width - 1 - (x + width + col);
                if (px < 0 || px >= c->width) continue;
                c->pixels[(size_t)py * (size_t)c->width + (size_t)px] = ((bits >> col) & 1u) ? fg : bg;
            }
        }
    }
    return width;
}

int mxo_mirror_x(const mxo_canvas_t *c, int x, int w)
{
    return c->width - x - w;
}
