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
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("overlay: all tests passed\n");
    return 0;
}
