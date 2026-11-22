#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mxcam_shape.h"

/* MXSH_TEST_HOOKS (mxcam_shape.c) */
void mxsh_test_canary_arm(void);
bool mxsh_test_canary_ok(void);

static int fails;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      fails++;                                            \
    }                                                     \
  } while (0)
#define NEAR(a, b, tol)                                                                          \
  do {                                                                                           \
    int a_ = (int)(a), b_ = (int)(b);                                                            \
    if (abs(a_ - b_) > (tol)) {                                                                  \
      printf("FAIL %s:%d: %s = %d, want %d +-%d\n", __FILE__, __LINE__, #a, a_, b_, (int)(tol)); \
      fails++;                                                                                   \
    }                                                                                            \
  } while (0)

#define FW 320
#define FH 240
#define PI_F 3.14159265f

static uint16_t frame[FW * FH];

/* RGB888 -> big-endian RGB565 as stored by the camera (swapped on LE CPU). */
static uint16_t px(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  return (uint16_t)((v << 8) | (v >> 8));
}
static uint16_t grey(uint8_t v) { return px(v, v, v); }
#define WHITE 0xFFFF
#define BLACK 0x0000

static void fill(uint16_t c) {
  for (int i = 0; i < FW * FH; i++) frame[i] = c;
}
static void fill_rect(int x, int y, int w, int h, uint16_t c) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++)
      if (i >= 0 && i < FW && j >= 0 && j < FH) frame[j * FW + i] = c;
}
/* Pixels whose centre is at distance [r_in, r) from (cx, cy). */
static void ring(float cx, float cy, float r, float r_in, uint16_t c) {
  for (int y = 0; y < FH; y++)
    for (int x = 0; x < FW; x++) {
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d2 = dx * dx + dy * dy;
      if (d2 < r * r && d2 >= r_in * r_in) frame[y * FW + x] = c;
    }
}
static void fill_circle(float cx, float cy, float r, uint16_t c) { ring(cx, cy, r, 0.0f, c); }
/* Pixels whose centre is inside the polygon (crossing number). */
static void fill_polygon(const float (*v)[2], int n, uint16_t c) {
  for (int y = 0; y < FH; y++)
    for (int x = 0; x < FW; x++) {
      float px_ = x + 0.5f, py = y + 0.5f;
      bool in = false;
      for (int i = 0, j = n - 1; i < n; j = i++) {
        if ((v[i][1] > py) != (v[j][1] > py) &&
            px_ < (v[j][0] - v[i][0]) * (py - v[i][1]) / (v[j][1] - v[i][1]) + v[i][0])
          in = !in;
      }
      if (in) frame[y * FW + x] = c;
    }
}
/* Regular n-gon, circumradius R, first vertex straight up, rotated by deg (clockwise on screen). */
static void regular(float cx, float cy, float R, int n, float deg, float (*v)[2]) {
  for (int i = 0; i < n; i++) {
    float a = (deg - 90.0f) * PI_F / 180.0f + 2.0f * PI_F * (float)i / (float)n;
    v[i][0] = cx + R * cosf(a);
    v[i][1] = cy + R * sinf(a);
  }
}
static void rot_rect(float cx, float cy, float w, float h, float deg, float (*v)[2]) {
  const float c[4][2] = {{-w / 2, -h / 2}, {w / 2, -h / 2}, {w / 2, h / 2}, {-w / 2, h / 2}};
  float a = deg * PI_F / 180.0f, ca = cosf(a), sa = sinf(a);
  for (int i = 0; i < 4; i++) {
    v[i][0] = cx + c[i][0] * ca - c[i][1] * sa;
    v[i][1] = cy + c[i][0] * sa + c[i][1] * ca;
  }
}
static void poly_regular(float cx, float cy, float R, int n, float deg, uint16_t c) {
  float v[8][2];
  regular(cx, cy, R, n, deg, v);
  fill_polygon(v, n, c);
}
static void poly_rect(float cx, float cy, float w, float h, float deg, uint16_t c) {
  float v[4][2];
  rot_rect(cx, cy, w, h, deg, v);
  fill_polygon(v, 4, c);
}

static mxp_shape_config_t defaults(void) {
  mxp_shape_config_t c;
  mxp_shape_config_defaults(&c);
  return c;
}
static mxp_result_t res[MXP_MAX_RESULTS];
static uint8_t detect(const mxp_shape_config_t *cfg) {
  memset(res, 0, sizeof res);
  return mxsh_detect(frame, cfg, res, MXP_MAX_RESULTS);
}
static uint8_t detect_default(void) {
  mxp_shape_config_t c = defaults();
  return detect(&c);
}
/* One record of the given id, or prints what was found. */
static void expect_one(uint16_t id, int line) {
  uint8_t n = detect_default();
  if (n != 1 || res[0].id != id) {
    printf("FAIL %s:%d: want 1 x id %u, got %u record(s)", __FILE__, line, id, n);
    for (int i = 0; i < n; i++) printf(" [id %u conf %u]", res[i].id, res[i].confidence);
    printf("\n");
    fails++;
  }
}

/* Densely sampled outline of a polygon (unit steps, rounded). */
static int outline(const float (*v)[2], int nv, int16_t (*pts)[2]) {
  int n = 0;
  for (int i = 0; i < nv; i++) {
    const float *a = v[i], *b = v[(i + 1) % nv];
    float len = hypotf(b[0] - a[0], b[1] - a[1]);
    int steps = (int)ceilf(len);
    for (int k = 0; k < steps; k++) {
      float t = (float)k / (float)steps;
      pts[n][0] = (int16_t)lroundf(a[0] + t * (b[0] - a[0]));
      pts[n][1] = (int16_t)lroundf(a[1] + t * (b[1] - a[1]));
      n++;
    }
  }
  return n;
}

static int16_t pts[MXSH_MAX_CONTOUR + 8][2];

static void test_classify_ideal(void) {
  uint8_t conf = 0;
  int n = 252;
  for (int i = 0; i < n; i++) {
    float a = 2.0f * PI_F * (float)i / (float)n;
    pts[i][0] = (int16_t)lroundf(100.0f + 40.0f * cosf(a));
    pts[i][1] = (int16_t)lroundf(100.0f + 40.0f * sinf(a));
  }
  CHECK(mxsh_classify((const int16_t(*)[2])pts, n, &conf) == MXP_SHAPE_CIRCLE);
  CHECK(conf >= 90);

  float v[4][2];
  regular(100.0f, 100.0f, 60.0f, 3, 0.0f, v);
  n = outline((const float(*)[2])v, 3, pts);
  conf = 0;
  CHECK(mxsh_classify((const int16_t(*)[2])pts, n, &conf) == MXP_SHAPE_TRIANGLE);
  CHECK(conf >= 90);

  rot_rect(100.0f, 100.0f, 80.0f, 80.0f, 0.0f, v);
  n = outline((const float(*)[2])v, 4, pts);
  conf = 0;
  CHECK(mxsh_classify((const int16_t(*)[2])pts, n, &conf) == MXP_SHAPE_SQUARE);
  CHECK(conf >= 90);

  rot_rect(100.0f, 100.0f, 120.0f, 60.0f, 0.0f, v);
  n = outline((const float(*)[2])v, 4, pts);
  conf = 0;
  CHECK(mxsh_classify((const int16_t(*)[2])pts, n, &conf) == MXP_SHAPE_RECT);
  CHECK(conf >= 90);
  CHECK(mxsh_classify((const int16_t(*)[2])pts, n, NULL) == MXP_SHAPE_RECT);

  /* degenerate input */
  CHECK(mxsh_classify((const int16_t(*)[2])pts, 2, &conf) == 0);
  CHECK(mxsh_classify((const int16_t(*)[2])pts, MXSH_MAX_CONTOUR + 1, &conf) == 0);
  for (int i = 0; i < 10; i++) pts[i][0] = pts[i][1] = (int16_t)i; /* collinear */
  CHECK(mxsh_classify((const int16_t(*)[2])pts, 10, &conf) == 0);
}

static void test_circle_filled(void) {
  fill(WHITE);
  fill_circle(160.0f, 120.0f, 40.0f, BLACK);
  uint8_t n = detect_default();
  CHECK(n == 1);
  if (n != 1) return;
  CHECK(res[0].id == MXP_SHAPE_CIRCLE);
  NEAR(res[0].x, 160, 3);
  NEAR(res[0].y, 120, 3);
  NEAR(res[0].w, 81, 4);
  NEAR(res[0].h, 81, 4);
  CHECK(res[0].confidence >= 85);
  CHECK(res[0].confidence <= 100);
  CHECK(res[0].extra_len == 0);

  /* a camera tilted ~26 degrees sees the circle as a 0.9 ellipse: still a circle */
  fill(WHITE);
  for (int y = 0; y < FH; y++)
    for (int x = 0; x < FW; x++) {
      float dx = (x + 0.5f - 160.0f) / 45.0f, dy = (y + 0.5f - 120.0f) / 40.5f;
      if (dx * dx + dy * dy < 1.0f) frame[y * FW + x] = BLACK;
    }
  expect_one(MXP_SHAPE_CIRCLE, __LINE__);
}

static void test_triangle_filled(void) {
  const float degs[2] = {0.0f, 20.0f};
  for (int i = 0; i < 2; i++) {
    fill(WHITE);
    poly_regular(160.0f, 125.0f, 100.0f / sqrtf(3.0f), 3, degs[i], BLACK);
    expect_one(MXP_SHAPE_TRIANGLE, __LINE__);
  }
}

static void test_square(void) {
  fill(WHITE);
  fill_rect(120, 80, 80, 80, BLACK);
  expect_one(MXP_SHAPE_SQUARE, __LINE__);
  if (res[0].id == MXP_SHAPE_SQUARE) {
    NEAR(res[0].x, 160, 2);
    NEAR(res[0].y, 120, 2);
    NEAR(res[0].w, 80, 2);
    CHECK(res[0].confidence >= 85);
  }
  /* review point 4: a square turned 45 degrees is still a square */
  fill(WHITE);
  poly_rect(160.0f, 120.0f, 80.0f, 80.0f, 45.0f, BLACK);
  expect_one(MXP_SHAPE_SQUARE, __LINE__);
  fill(WHITE);
  poly_rect(160.0f, 120.0f, 80.0f, 80.0f, 15.0f, BLACK);
  expect_one(MXP_SHAPE_SQUARE, __LINE__);
}

static void test_rectangle(void) {
  const float degs[2] = {0.0f, 30.0f};
  for (int i = 0; i < 2; i++) {
    fill(WHITE);
    poly_rect(160.0f, 120.0f, 140.0f, 70.0f, degs[i], BLACK);
    expect_one(MXP_SHAPE_RECT, __LINE__);
  }
}

/* Review point 4: outlines drawn with a pen. */
static void test_shape_outline(void) {
  fill(WHITE);
  ring(160.0f, 120.0f, 50.0f, 46.0f, BLACK);
  expect_one(MXP_SHAPE_CIRCLE, __LINE__);
  fill(WHITE);
  fill_rect(115, 75, 90, 90, BLACK);
  fill_rect(119, 79, 82, 82, WHITE);
  expect_one(MXP_SHAPE_SQUARE, __LINE__);
}

/* Review point 4: a regular pentagon is round-ish (circularity ~0.82) but its
 * centroid distance varies more than a circle's (CV ~0.064 in theory). */
static void test_pentagon_not_reported(void) {
  const float degs[4] = {0.0f, 9.0f, 17.0f, 36.0f};
  const float radii[3] = {30.0f, 50.0f, 70.0f};
  for (int s = 0; s < 3; s++)
    for (int i = 0; i < 4; i++) {
      fill(WHITE);
      poly_regular(160.0f, 120.0f, radii[s], 5, degs[i], BLACK);
      uint8_t n = detect_default();
      if (n != 0) printf("FAIL pentagon R %.0f deg %.0f: id %u\n", radii[s], degs[i], res[0].id);
      CHECK(n == 0);
    }
}

/* Review point 4: shapes cut by the frame edge and the edge of a sheet of
 * paper are not shapes. */
static void test_shape_border_touching_ignored(void) {
  fill(WHITE);
  fill_rect(-40, 80, 80, 80, BLACK);
  CHECK(detect_default() == 0);
  fill(BLACK);
  fill_rect(40, 30, 240, 180, WHITE);
  CHECK(detect_default() == 0);
  fill_circle(160.0f, 120.0f, 30.0f, BLACK);
  expect_one(MXP_SHAPE_CIRCLE, __LINE__);
}

static void test_small_ignored(void) {
  fill(WHITE);
  fill_rect(148, 108, 24, 24, BLACK);
  mxp_shape_config_t c = defaults();
  CHECK(detect(&c) == 0);
  c.min_size = 16;
  CHECK(detect(&c) == 1);
  CHECK(res[0].id == MXP_SHAPE_SQUARE);
  NEAR(res[0].w, 24, 2);
}

static void test_light_mode(void) {
  fill(BLACK);
  fill_circle(90.0f, 120.0f, 40.0f, WHITE);
  fill_rect(190, 80, 80, 80, WHITE);
  mxp_shape_config_t c = defaults();
  c.mode = 1;
  uint8_t n = detect(&c);
  CHECK(n == 2);
  if (n == 2) {
    CHECK((res[0].id == MXP_SHAPE_CIRCLE && res[1].id == MXP_SHAPE_SQUARE) ||
          (res[0].id == MXP_SHAPE_SQUARE && res[1].id == MXP_SHAPE_CIRCLE));
  }
  c.mode = 0;
  CHECK(detect(&c) == 0);
}

static void test_fixed_threshold(void) {
  fill(WHITE);
  fill_rect(120, 80, 80, 80, grey(100));
  mxp_shape_config_t c = defaults();
  c.threshold = 120;
  CHECK(detect(&c) == 1);
  CHECK(res[0].id == MXP_SHAPE_SQUARE);
  c.threshold = 80; /* 100 is not below 80 */
  CHECK(detect(&c) == 0);
}

static uint32_t s_rng = 12345u;
static uint32_t rnd(void) {
  s_rng = s_rng * 1664525u + 1013904223u;
  return s_rng >> 8;
}

/* Review point 4: sensor noise on a blank sheet is not a shape. */
static void test_shape_noise_frame(void) {
  for (int i = 0; i < FW * FH; i++) frame[i] = grey((uint8_t)(120 + rnd() % 21));
  CHECK(detect_default() == 0);
}

/* 6 shapes with clearly different areas; the 4 largest come back largest first. */
static void test_max_results_sort(void) {
  mxsh_test_canary_arm();
  fill(WHITE);
  fill_rect(55 - 18, 60 - 18, 36, 36, BLACK);       /* square 36: 1296 */
  fill_circle(160.0f, 60.0f, 25.0f, BLACK);         /* circle 50: 1963 */
  fill_rect(265 - 27, 60 - 27, 54, 54, BLACK);      /* square 54: 2916 */
  fill_circle(55.0f, 180.0f, 35.0f, BLACK);         /* circle 70: 3848 */
  fill_rect(160 - 37, 180 - 37, 74, 74, BLACK);     /* square 74: 5476 */
  fill_circle(265.0f, 180.0f, 48.0f, BLACK);        /* circle 96: 7238 */
  mxp_shape_config_t c = defaults();
  c.max_results = 8;
  CHECK(detect(&c) == 6);
  c.max_results = 4;
  uint8_t n = detect(&c);
  CHECK(n == 4);
  if (n == 4) {
    const uint16_t ids[4] = {MXP_SHAPE_CIRCLE, MXP_SHAPE_SQUARE, MXP_SHAPE_CIRCLE, MXP_SHAPE_SQUARE};
    const int ws[4] = {96, 74, 70, 54};
    for (int i = 0; i < 4; i++) {
      CHECK(res[i].id == ids[i]);
      NEAR(res[i].w, ws[i], 4);
    }
  }
  /* cap below max_results */
  CHECK(mxsh_detect(frame, &c, res, 2) == 2);
  CHECK(res[0].w > res[1].w);
  CHECK(mxsh_test_canary_ok());

  clock_t t0 = clock();
  for (int i = 0; i < 100; i++) (void)detect(&c);
  double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 100.0;
  printf("info: mxsh_detect avg on host, 6 shapes: %.3f ms\n", ms);
  CHECK(mxsh_test_canary_ok());
}

/* Worst case: random black/white 4 px blocks give many ragged regions (long
 * contours, Douglas-Peucker on garbage). Nothing overruns; timing for Task 5. */
static void test_clutter_bounded(void) {
  mxsh_test_canary_arm();
  mxp_shape_config_t c = defaults();
  c.min_size = 16;
  c.max_results = 8;
  double worst = 0.0;
  for (int pass = 0; pass < 4; pass++) {
    for (int y = 0; y < FH; y += 4)
      for (int x = 0; x < FW; x += 4) fill_rect(x, y, 4, 4, (int)(rnd() % 100u) < 45 + 5 * pass ? BLACK : WHITE);
    clock_t t0 = clock();
    for (int i = 0; i < 50; i++) (void)detect(&c);
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 50.0;
    if (ms > worst) worst = ms;
  }
  printf("info: mxsh_detect avg on host, random 4 px blocks (worst of 4): %.3f ms\n", worst);
  CHECK(mxsh_test_canary_ok());
}

static void test_label(void) {
  char buf[32];
  CHECK(mxsh_label(3, 90, buf, sizeof buf) == 9);
  CHECK(strcmp(buf, "SQUARE 90") == 0);
  CHECK(mxsh_label(9, 50, buf, sizeof buf) == 4);
  CHECK(strcmp(buf, "? 50") == 0);
  CHECK(mxsh_label(1, 100, buf, 4) == 10);
  CHECK(strcmp(buf, "CIR") == 0);
  CHECK(strcmp(mxsh_ascii_name(MXP_SHAPE_TRIANGLE), "TRIANGLE") == 0);
  CHECK(strcmp(mxsh_ascii_name(MXP_SHAPE_RECT), "RECT") == 0);
  CHECK(strcmp(mxsh_ascii_name(0), "?") == 0);
}

int main(void) {
  test_classify_ideal();
  test_circle_filled();
  test_triangle_filled();
  test_square();
  test_rectangle();
  test_shape_outline();
  test_pentagon_not_reported();
  test_shape_border_touching_ignored();
  test_small_ignored();
  test_light_mode();
  test_fixed_threshold();
  test_shape_noise_frame();
  test_max_results_sort();
  test_clutter_bounded();
  test_label();
  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
