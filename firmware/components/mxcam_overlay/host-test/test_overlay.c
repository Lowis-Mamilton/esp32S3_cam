#include <stdio.h>
#include <string.h>
#include "mxcam_overlay.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define W 16
#define H 16
#define GUARD 32
#define SENT 0xA5A5

/* Buffer with sentinel guard words before and after the canvas. */
typedef struct { uint16_t buf[GUARD + W * H + GUARD]; } guarded_t;

static void guarded_init(guarded_t *g, uint16_t fill)
{
    for (int i = 0; i < GUARD; i++) { g->buf[i] = SENT; g->buf[GUARD + W * H + i] = SENT; }
    for (int i = 0; i < W * H; i++) g->buf[GUARD + i] = fill;
}

static int guards_ok(const guarded_t *g)
{
    for (int i = 0; i < GUARD; i++)
        if (g->buf[i] != SENT || g->buf[GUARD + W * H + i] != SENT) return 0;
    return 1;
}

static void test_rect_outline(void)
{
    guarded_t g; guarded_init(&g, MXO_BLACK);
    mxo_canvas_t c = { g.buf + GUARD, W, H };
    mxo_draw_rect(&c, 2, 3, 8, 6, 1, MXO_WHITE);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int inside = x >= 2 && x < 10 && y >= 3 && y < 9;
            int border = inside && (x == 2 || x == 9 || y == 3 || y == 8);
            uint16_t exp = border ? MXO_WHITE : MXO_BLACK;
            if (c.pixels[y * W + x] != exp) { printf("FAIL rect pixel %d,%d\n", x, y); failures++; }
        }
    CHECK(guards_ok(&g));
}

static void test_rect_thick(void)
{
    guarded_t g; guarded_init(&g, MXO_BLACK);
    mxo_canvas_t c = { g.buf + GUARD, W, H };
    mxo_draw_rect(&c, 0, 0, 6, 6, 2, MXO_RED);
    CHECK(c.pixels[1 * W + 1] == MXO_RED);   /* 2px border */
    CHECK(c.pixels[2 * W + 2] == MXO_BLACK); /* interior */
    CHECK(c.pixels[3 * W + 3] == MXO_BLACK);
    CHECK(guards_ok(&g));
}

static void test_clipping(void)
{
    guarded_t g; guarded_init(&g, MXO_BLACK);
    mxo_canvas_t c = { g.buf + GUARD, W, H };
    mxo_fill_rect(&c, -5, -5, 100, 100, MXO_GREEN);
    CHECK(c.pixels[0] == MXO_GREEN && c.pixels[W * H - 1] == MXO_GREEN);
    CHECK(guards_ok(&g));
    mxo_draw_rect(&c, -3, -3, 40, 40, 3, MXO_RED);
    CHECK(guards_ok(&g));
    mxo_draw_rect(&c, 10, 10, 50, 50, 100, MXO_RED);
    CHECK(guards_ok(&g));
    mxo_fill_rect(&c, 14, 14, 10, 10, MXO_WHITE);
    CHECK(c.pixels[15 * W + 15] == MXO_WHITE);
    CHECK(guards_ok(&g));
    int w = mxo_draw_text(&c, -4, -4, "HELLO", MXO_WHITE, MXO_BLACK);
    CHECK(w == 40);
    w = mxo_draw_text(&c, 12, 12, "HELLO", MXO_WHITE, MXO_BLACK);
    CHECK(w == 40);
    w = mxo_draw_text(&c, 1000, 1000, "HELLO", MXO_WHITE, MXO_BLACK);
    CHECK(w == 40);
    w = mxo_draw_text(&c, -1000, 2, "HELLO", MXO_WHITE, MXO_BLACK);
    CHECK(w == 40);
    CHECK(guards_ok(&g));
}

static void test_degenerate(void)
{
    guarded_t g; guarded_init(&g, MXO_BLACK);
    mxo_canvas_t c = { g.buf + GUARD, W, H };
    mxo_fill_rect(&c, 0, 0, 0, 5, MXO_WHITE);
    mxo_fill_rect(&c, 0, 0, 5, -3, MXO_WHITE);
    mxo_draw_rect(&c, 0, 0, -4, 4, 1, MXO_WHITE);
    mxo_draw_rect(&c, 0, 0, 4, 4, 0, MXO_WHITE);
    mxo_draw_rect(&c, 0, 0, 4, 4, -2, MXO_WHITE);
    for (int i = 0; i < W * H; i++) CHECK(c.pixels[i] == MXO_BLACK);
    CHECK(mxo_draw_text(&c, 0, 0, "", MXO_WHITE, MXO_BLACK) == 0);
    CHECK(mxo_draw_text(&c, 0, 0, NULL, MXO_WHITE, MXO_BLACK) == 0);
    mxo_canvas_t bad = { NULL, W, H };
    mxo_fill_rect(&bad, 0, 0, 4, 4, MXO_WHITE);
    mxo_canvas_t empty = { g.buf + GUARD, 0, 0 };
    mxo_fill_rect(&empty, 0, 0, 4, 4, MXO_WHITE);
    CHECK(guards_ok(&g));
    for (int i = 0; i < W * H; i++) CHECK(c.pixels[i] == MXO_BLACK);
}

static void test_text(void)
{
    guarded_t g; guarded_init(&g, MXO_BLACK);
    mxo_canvas_t c = { g.buf + GUARD, W, H };
    int w = mxo_draw_text(&c, 0, 0, "A", MXO_WHITE, MXO_BLACK);
    CHECK(w == 8);
    int fg = 0;
    for (int i = 0; i < W * H; i++) if (c.pixels[i] == MXO_WHITE) fg++;
    CHECK(fg >= 1);
    /* Background box covers the full 8x8 cell. */
    guarded_init(&g, MXO_RED);
    mxo_draw_text(&c, 4, 4, "A", MXO_WHITE, MXO_BLACK);
    int box = 0, red = 0;
    for (int i = 0; i < W * H; i++) {
        if (c.pixels[i] == MXO_BLACK || c.pixels[i] == MXO_WHITE) box++;
        if (c.pixels[i] == MXO_RED) red++;
    }
    CHECK(box == 64);
    CHECK(red == W * H - 64);
    /* Non-printable renders blank (bg only) but still advances 8. */
    guarded_init(&g, MXO_RED);
    w = mxo_draw_text(&c, 0, 0, "\x01\x7f", MXO_WHITE, MXO_BLACK);
    CHECK(w == 16);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 16; x++) CHECK(c.pixels[y * W + x] == MXO_BLACK);
    CHECK(guards_ok(&g));
}

static void test_colors(void)
{
    /* Colours are big-endian RGB565 held in a uint16 on a little-endian CPU. */
    CHECK(MXO_WHITE == 0xFFFF);
    CHECK(MXO_BLACK == 0x0000);
    CHECK(MXO_RED == 0x00F8);   /* 0xF800 byte-swapped */
    CHECK(MXO_GREEN == 0xE007); /* 0x07E0 byte-swapped */
    const uint16_t v = MXO_GREEN;
    CHECK(((const unsigned char *)&v)[0] == 0x07 && ((const unsigned char *)&v)[1] == 0xE0);
}

/* The panel shows the frame mirrored left-right (ADR-0010): text drawn with
 * mxo_draw_text_mirrored at panel position x must equal the left-right mirror
 * of normal text at x, so it reads correctly on the panel. */
static void check_mirror_of_normal(int x, int y, const char *s)
{
    guarded_t a, b;
    guarded_init(&a, MXO_RED);
    guarded_init(&b, MXO_RED);
    mxo_canvas_t ca = { a.buf + GUARD, W, H }, cb = { b.buf + GUARD, W, H };
    int wa = mxo_draw_text(&ca, x, y, s, MXO_WHITE, MXO_BLACK);
    int wb = mxo_draw_text_mirrored(&cb, x, y, s, MXO_WHITE, MXO_BLACK);
    CHECK(wa == wb);
    int same = 1;
    for (int yy = 0; yy < H; yy++)
        for (int xx = 0; xx < W; xx++)
            if (cb.pixels[yy * W + xx] != ca.pixels[yy * W + (W - 1 - xx)]) same = 0;
    CHECK(same);
    CHECK(guards_ok(&a) && guards_ok(&b));
}

static void test_text_mirrored(void)
{
    check_mirror_of_normal(0, 0, "AB");  /* fills the 16-px canvas */
    check_mirror_of_normal(4, 3, "A");   /* offset */
    check_mirror_of_normal(12, 0, "Fx"); /* clipped on the panel's right edge */
    check_mirror_of_normal(-5, -2, "Q"); /* clipped left/top */
    /* An asymmetric glyph really is reversed: 'F' has its stem on the left. */
    guarded_t g;
    guarded_init(&g, MXO_BLACK);
    mxo_canvas_t c = { g.buf + GUARD, W, H };
    mxo_draw_text_mirrored(&c, 0, 0, "F", MXO_WHITE, MXO_BLACK);
    int right = 0, left = 0;
    for (int y = 0; y < 8; y++) {
        if (c.pixels[y * W + W - 1] == MXO_WHITE || c.pixels[y * W + W - 2] == MXO_WHITE) right++;
        if (c.pixels[y * W + W - 8] == MXO_WHITE) left++;
    }
    CHECK(right >= 5 && left <= 2); /* stem at the frame's right = panel's left */
}

static void test_mirror_x(void)
{
    mxo_canvas_t c = { NULL, 320, 240 };
    CHECK(mxo_mirror_x(&c, 0, 10) == 310);
    CHECK(mxo_mirror_x(&c, 310, 10) == 0);
    CHECK(mxo_mirror_x(&c, 100, 0) == 220);
    CHECK(mxo_mirror_x(&c, mxo_mirror_x(&c, 37, 12), 12) == 37); /* involution */
}

static int count_color(const mxo_canvas_t *c, uint16_t color)
{
    int n = 0;
    for (int i = 0; i < c->width * c->height; i++) if (c->pixels[i] == color) n++;
    return n;
}

static void test_draw_line_horizontal_vertical_diagonal(void)
{
    guarded_t g; guarded_init(&g, MXO_BLACK);
    mxo_canvas_t c = { g.buf + GUARD, W, H };
    mxo_draw_line(&c, 1, 2, 12, 2, 1, MXO_WHITE); /* horizontal */
    for (int x = 1; x <= 12; x++) CHECK(c.pixels[2 * W + x] == MXO_WHITE);
    CHECK(c.pixels[2 * W + 0] == MXO_BLACK && c.pixels[2 * W + 13] == MXO_BLACK);
    CHECK(c.pixels[1 * W + 6] == MXO_BLACK && c.pixels[3 * W + 6] == MXO_BLACK);
    CHECK(count_color(&c, MXO_WHITE) == 12);

    guarded_init(&g, MXO_BLACK);
    mxo_draw_line(&c, 5, 14, 5, 3, 1, MXO_RED); /* vertical, drawn bottom-up */
    for (int y = 3; y <= 14; y++) CHECK(c.pixels[y * W + 5] == MXO_RED);
    CHECK(c.pixels[2 * W + 5] == MXO_BLACK && c.pixels[15 * W + 5] == MXO_BLACK);
    CHECK(count_color(&c, MXO_RED) == 12);

    guarded_init(&g, MXO_BLACK);
    mxo_draw_line(&c, 15, 15, 0, 0, 1, MXO_GREEN); /* diagonal, reversed */
    for (int i = 0; i < 16; i++) CHECK(c.pixels[i * W + i] == MXO_GREEN);
    CHECK(count_color(&c, MXO_GREEN) == 16);

    guarded_init(&g, MXO_BLACK);
    mxo_draw_line(&c, 0, 0, 12, 4, 1, MXO_WHITE); /* shallow slope: one pixel per column */
    CHECK(c.pixels[0] == MXO_WHITE && c.pixels[4 * W + 12] == MXO_WHITE);
    CHECK(c.pixels[2 * W + 6] == MXO_WHITE); /* midpoint */
    CHECK(count_color(&c, MXO_WHITE) == 13);

    guarded_init(&g, MXO_BLACK);
    mxo_draw_line(&c, 7, 9, 7, 9, 1, MXO_WHITE); /* single point */
    CHECK(c.pixels[9 * W + 7] == MXO_WHITE && count_color(&c, MXO_WHITE) == 1);
    CHECK(guards_ok(&g));
}

static void test_draw_line_clips(void)
{
    guarded_t g; guarded_init(&g, MXO_BLACK);
    mxo_canvas_t c = { g.buf + GUARD, W, H };
    mxo_draw_line(&c, -20, -12, 40, 36, 1, MXO_WHITE); /* passes through the canvas */
    CHECK(guards_ok(&g));
    CHECK(count_color(&c, MXO_WHITE) > 8);
    /* the ideal line y = -12 + 0.8 (x + 20) passes (5,8) and (10,12); clipping
     * may move the Bresenham path by at most one pixel */
    CHECK(c.pixels[8 * W + 5] == MXO_WHITE || c.pixels[7 * W + 5] == MXO_WHITE || c.pixels[9 * W + 5] == MXO_WHITE);
    CHECK(c.pixels[12 * W + 10] == MXO_WHITE || c.pixels[11 * W + 10] == MXO_WHITE || c.pixels[13 * W + 10] == MXO_WHITE);
    CHECK(c.pixels[0] == MXO_BLACK && c.pixels[15 * W + 0] == MXO_BLACK);
    guarded_init(&g, MXO_BLACK);
    mxo_draw_line(&c, -100, 5, 100, 5, 3, MXO_RED); /* wider than canvas, thick */
    CHECK(guards_ok(&g));
    for (int x = 0; x < W; x++) CHECK(c.pixels[4 * W + x] == MXO_RED && c.pixels[6 * W + x] == MXO_RED);
    guarded_init(&g, MXO_BLACK);
    mxo_draw_line(&c, -50, -50, -10, 40, 5, MXO_RED);      /* fully outside (left) */
    mxo_draw_line(&c, 20, -30, 90, -3, 2, MXO_RED);        /* fully outside (above) */
    mxo_draw_line(&c, 2000000000, 0, -2000000000, 3, 1, MXO_GREEN); /* huge coordinates */
    mxo_draw_line(&c, 0, 0, 5, 5, 0, MXO_RED);             /* thickness 0: nothing */
    CHECK(guards_ok(&g));
    CHECK(count_color(&c, MXO_RED) == 0);
    CHECK(count_color(&c, MXO_GREEN) > 0); /* the huge line still crosses the canvas */
    mxo_canvas_t bad = { NULL, W, H };
    mxo_draw_line(&bad, 0, 0, 5, 5, 1, MXO_WHITE);
    CHECK(guards_ok(&g));
}

static void test_draw_line_thickness(void)
{
    guarded_t g; guarded_init(&g, MXO_BLACK);
    mxo_canvas_t c = { g.buf + GUARD, W, H };
    mxo_draw_line(&c, 3, 8, 12, 8, 3, MXO_WHITE);
    for (int x = 3; x <= 12; x++)
        for (int y = 7; y <= 9; y++) CHECK(c.pixels[y * W + x] == MXO_WHITE);
    for (int x = 0; x < W; x++) CHECK(c.pixels[6 * W + x] == MXO_BLACK && c.pixels[10 * W + x] == MXO_BLACK);
    guarded_init(&g, MXO_BLACK);
    mxo_draw_line(&c, 8, 2, 8, 13, 3, MXO_WHITE); /* vertical, 3 px wide */
    for (int y = 2; y <= 13; y++)
        CHECK(c.pixels[y * W + 7] == MXO_WHITE && c.pixels[y * W + 8] == MXO_WHITE && c.pixels[y * W + 9] == MXO_WHITE);
    for (int y = 0; y < H; y++) CHECK(c.pixels[y * W + 6] == MXO_BLACK && c.pixels[y * W + 10] == MXO_BLACK);
    guarded_init(&g, MXO_BLACK);
    mxo_draw_line(&c, 0, 0, 15, 15, 4, MXO_WHITE); /* thick diagonal hugging the corners */
    CHECK(guards_ok(&g));
    CHECK(c.pixels[0] == MXO_WHITE && c.pixels[15 * W + 15] == MXO_WHITE);
    CHECK(c.pixels[0 * W + 15] == MXO_BLACK && c.pixels[15 * W + 0] == MXO_BLACK);
}

int main(void)
{
    test_rect_outline();
    test_rect_thick();
    test_clipping();
    test_degenerate();
    test_text();
    test_colors();
    test_text_mirrored();
    test_mirror_x();
    test_draw_line_horizontal_vertical_diagonal();
    test_draw_line_clips();
    test_draw_line_thickness();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("overlay: all tests passed\n");
    return 0;
}
