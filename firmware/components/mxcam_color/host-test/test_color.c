#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mxcam_color.h"

void mxc_test_canary_arm(void);
bool mxc_test_canary_ok(void);

static int fails;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      fails++;                                            \
    }                                                     \
  } while (0)
#define NEAR(a, b, tol) CHECK(abs((int)(a) - (int)(b)) <= (tol))

static uint16_t frame[MXC_FRAME_W * MXC_FRAME_H];

/* RGB888 -> big-endian RGB565 as stored by the camera (swapped on LE CPU). */
static uint16_t px(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  return (uint16_t)((v << 8) | (v >> 8));
}
static void clear(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t p = px(r, g, b);
  for (int i = 0; i < MXC_FRAME_W * MXC_FRAME_H; i++) frame[i] = p;
}
static void rect(int x, int y, int w, int h, const uint8_t c[3]) {
  uint16_t p = px(c[0], c[1], c[2]);
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++)
      if (i >= 0 && i < MXC_FRAME_W && j >= 0 && j < MXC_FRAME_H) frame[j * MXC_FRAME_W + i] = p;
}

static const uint8_t RED[3] = {230, 20, 20};     /* hue ~0 */
static const uint8_t RED5[3] = {230, 40, 20};    /* hue ~5 */
static const uint8_t RED355[3] = {230, 20, 40};  /* hue ~355 */
static const uint8_t GREEN[3] = {20, 200, 20};
static const uint8_t BLUE[3] = {20, 20, 220};

static mxc_params_t params(uint8_t tol, uint16_t min_area, uint8_t maxr) {
  mxc_params_t p = {tol, min_area, maxr};
  return p;
}
static mxc_learned_t learn(uint8_t id, const uint8_t c[3]) {
  mxc_learned_t l = {id, {c[0], c[1], c[2]}};
  return l;
}

static void test_basic_colors(void) {
  clear(0, 0, 0);
  rect(20, 30, 60, 40, RED);
  rect(120, 100, 50, 50, GREEN);
  rect(220, 150, 40, 80, BLUE);
  mxc_learned_t l[3] = {learn(1, RED), learn(2, GREEN), learn(3, BLUE)};
  mxc_params_t p = params(30, 100, 8);
  mxp_result_t out[8];
  uint8_t n = mxc_detect(frame, l, 3, &p, out, 8);
  CHECK(n == 3);
  if (n != 3) return;
  /* areas: red 2400, green 2500, blue 3200 -> blue, green, red */
  CHECK(out[0].id == 3);
  NEAR(out[0].x, 240, 2); NEAR(out[0].y, 190, 2); NEAR(out[0].w, 40, 2); NEAR(out[0].h, 80, 2);
  CHECK(out[1].id == 2);
  NEAR(out[1].x, 145, 2); NEAR(out[1].y, 125, 2); NEAR(out[1].w, 50, 2); NEAR(out[1].h, 50, 2);
  CHECK(out[2].id == 1);
  NEAR(out[2].x, 50, 2); NEAR(out[2].y, 50, 2); NEAR(out[2].w, 60, 2); NEAR(out[2].h, 40, 2);
  CHECK(out[0].confidence == 255 && out[0].extra_len == 0);
}

static void test_hue_wraparound(void) {
  clear(0, 0, 0);
  rect(100, 80, 40, 40, RED355);
  mxc_learned_t l[1] = {learn(7, RED5)};
  mxc_params_t p = params(30, 100, 8);
  mxp_result_t out[8];
  CHECK(mxc_detect(frame, l, 1, &p, out, 8) == 1);
  CHECK(out[0].id == 7);
  /* a green blob must not match a red learned colour */
  clear(0, 0, 0);
  rect(100, 80, 40, 40, GREEN);
  CHECK(mxc_detect(frame, l, 1, &p, out, 8) == 0);
}

static void test_min_area(void) {
  clear(0, 0, 0);
  rect(10, 10, 10, 10, RED);   /* 100 px */
  rect(100, 100, 40, 40, RED); /* 1600 px */
  mxc_learned_t l[1] = {learn(1, RED)};
  mxc_params_t p = params(30, 500, 8);
  mxp_result_t out[8];
  CHECK(mxc_detect(frame, l, 1, &p, out, 8) == 1);
  NEAR(out[0].x, 120, 2);
  p = params(30, 50, 8);
  CHECK(mxc_detect(frame, l, 1, &p, out, 8) == 2);
}

static void test_max_results_and_two_blobs(void) {
  clear(0, 0, 0);
  rect(10, 10, 20, 20, RED);
  rect(100, 10, 40, 40, RED);
  rect(200, 10, 30, 30, RED);
  mxc_learned_t l[1] = {learn(1, RED)};
  mxc_params_t p = params(30, 10, 8);
  mxp_result_t out[8];
  CHECK(mxc_detect(frame, l, 1, &p, out, 8) == 3);
  CHECK(out[0].w >= 38 && out[1].w >= 28 && out[2].w >= 18 && out[2].w <= 22);
  p = params(30, 10, 2);
  CHECK(mxc_detect(frame, l, 1, &p, out, 8) == 2);
  CHECK(out[0].w >= 38 && out[1].w >= 28 && out[1].w <= 32);
  p = params(30, 10, 8);
  CHECK(mxc_detect(frame, l, 1, &p, out, 1) == 1);
  CHECK(out[0].w >= 38);
}

static void test_edge_clipping(void) {
  clear(0, 0, 0);
  rect(0, 0, 40, 30, BLUE);
  rect(290, 210, 60, 60, GREEN); /* clipped to 30x30 */
  mxc_learned_t l[2] = {learn(3, BLUE), learn(2, GREEN)};
  mxc_params_t p = params(30, 100, 8);
  mxp_result_t out[8];
  uint8_t n = mxc_detect(frame, l, 2, &p, out, 8);
  CHECK(n == 2);
  if (n != 2) return;
  CHECK(out[0].id == 3);
  NEAR(out[0].x, 20, 2); NEAR(out[0].y, 15, 2); NEAR(out[0].w, 40, 2); NEAR(out[0].h, 30, 2);
  CHECK(out[1].id == 2);
  NEAR(out[1].x, 305, 2); NEAR(out[1].y, 225, 2); NEAR(out[1].w, 30, 2); NEAR(out[1].h, 30, 2);
  for (int i = 0; i < 2; i++) {
    CHECK(out[i].x - out[i].w / 2 >= 0 && out[i].x + out[i].w / 2 <= MXC_FRAME_W);
    CHECK(out[i].y - out[i].h / 2 >= 0 && out[i].y + out[i].h / 2 <= MXC_FRAME_H);
  }
}

static void test_grey_never_matches(void) {
  static const uint8_t greys[3][3] = {{128, 128, 128}, {255, 255, 255}, {0, 0, 0}};
  mxc_learned_t l[1] = {learn(1, RED)};
  mxc_params_t p = params(100, 10, 8);
  mxp_result_t out[8];
  for (int i = 0; i < 3; i++) {
    clear(greys[i][0], greys[i][1], greys[i][2]);
    CHECK(mxc_detect(frame, l, 1, &p, out, 8) == 0);
    uint8_t rgb[3];
    CHECK(!mxc_sample(frame, 100, 100, 50, 50, rgb));
  }
  /* a learned grey colour never produces detections either */
  static const uint8_t grey[3] = {128, 128, 128};
  mxc_learned_t g[1] = {learn(1, grey)};
  clear(128, 128, 128);
  CHECK(mxc_detect(frame, g, 1, &p, out, 8) == 0);
}

static void test_sample(void) {
  clear(0, 0, 0);
  rect(100, 100, 40, 40, GREEN);
  uint8_t rgb[3];
  CHECK(mxc_sample(frame, 100, 100, 40, 40, rgb));
  NEAR(rgb[0], GREEN[0], 8); NEAR(rgb[1], GREEN[1], 8); NEAR(rgb[2], GREEN[2], 8);
  /* region mostly background: <25% qualify */
  CHECK(!mxc_sample(frame, 100, 100, 160, 160, rgb));
  /* region extending past the frame is clamped */
  rect(300, 220, 20, 20, BLUE);
  CHECK(mxc_sample(frame, 300, 220, 100, 100, rgb));
  NEAR(rgb[2], BLUE[2], 8);
  CHECK(!mxc_sample(frame, 400, 300, 10, 10, rgb));
}

static void test_same_color(void) {
  const uint8_t b[3] = {230, 80, 20}; /* hue ~15 vs RED hue 0 */
  CHECK(mxc_same_color(RED, RED, 10));
  CHECK(mxc_same_color(RED, b, 10) == mxc_same_color(b, RED, 10));
  CHECK(!mxc_same_color(RED, b, 10)); /* window +-6 deg */
  CHECK(mxc_same_color(RED, b, 40));  /* window +-24 deg */
  CHECK(mxc_same_color(RED355, RED5, 30));
  CHECK(!mxc_same_color(RED, GREEN, 100));
  const uint8_t grey[3] = {100, 100, 100};
  CHECK(!mxc_same_color(grey, grey, 100));
}


static void fill_pattern(int mode, const uint8_t *c[3]) {
  for (int y = 0; y < MXC_FRAME_H; y++)
    for (int x = 0; x < MXC_FRAME_W; x++) {
      int cell = ((x / 2) + 2 * (y / 2));
      const uint8_t *col = mode == 2 ? c[(x / 2 + y / 2) % 2] : c[cell % 3];
      frame[y * MXC_FRAME_W + x] = px(col[0], col[1], col[2]);
    }
}

static void test_label_overflow(void) {
  mxc_learned_t l[3] = {learn(1, RED), learn(2, GREEN), learn(3, BLUE)};
  const uint8_t *cols[3] = {RED, GREEN, BLUE};
  mxp_result_t out[8];
  mxc_params_t p = params(30, 1, 8);
  for (int mode = 2; mode <= 3; mode++) {
    fill_pattern(mode, cols);
    mxc_test_canary_arm();
    uint8_t n = mxc_detect(frame, l, mode == 2 ? 2 : 3, &p, out, 8);
    CHECK(mxc_test_canary_ok());
    CHECK(n <= 8);
  }
}

static void test_concave_blob(void) {
  clear(0, 0, 0);
  rect(40, 40, 20, 80, RED);   /* left arm */
  rect(100, 40, 20, 80, RED);  /* right arm */
  rect(40, 100, 80, 20, RED);  /* base joins them (U shape) */
  mxc_learned_t l[1] = {learn(1, RED)};
  mxc_params_t p = params(30, 10, 8);
  mxp_result_t out[8];
  CHECK(mxc_detect(frame, l, 1, &p, out, 8) == 1);
  NEAR(out[0].x, 80, 2); NEAR(out[0].y, 80, 2); NEAR(out[0].w, 80, 2); NEAR(out[0].h, 80, 2);
}

static void test_nearest_hue_wins(void) {
  const uint8_t orange[3] = {230, 120, 20}; /* hue ~28 */
  const uint8_t yellowish[3] = {230, 200, 20}; /* hue ~51 */
  const uint8_t blob[3] = {230, 135, 20};  /* hue ~32, nearer orange */
  clear(0, 0, 0);
  rect(100, 80, 40, 40, blob);
  mxc_learned_t l[2] = {learn(1, yellowish), learn(2, orange)};
  mxc_params_t p = params(60, 100, 8);
  mxp_result_t out[8];
  CHECK(mxc_detect(frame, l, 2, &p, out, 8) == 1);
  CHECK(out[0].id == 2);
}

static void test_odd_offset(void) {
  clear(0, 0, 0);
  rect(101, 57, 41, 33, GREEN);
  mxc_learned_t l[1] = {learn(2, GREEN)};
  mxc_params_t p = params(30, 100, 8);
  mxp_result_t out[8];
  CHECK(mxc_detect(frame, l, 1, &p, out, 8) == 1);
  NEAR(out[0].x, 101 + 20, 2); NEAR(out[0].y, 57 + 16, 2); NEAR(out[0].w, 41, 2); NEAR(out[0].h, 33, 2);
}

static void test_timing(void) {
  clear(0, 0, 0);
  rect(20, 30, 60, 40, RED);
  rect(120, 100, 50, 50, GREEN);
  rect(220, 150, 40, 80, BLUE);
  mxc_learned_t l[3] = {learn(1, RED), learn(2, GREEN), learn(3, BLUE)};
  mxc_params_t p = params(30, 100, 8);
  mxp_result_t out[8];
  clock_t t0 = clock();
  for (int i = 0; i < 100; i++) mxc_detect(frame, l, 3, &p, out, 8);
  double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 100.0;
  printf("info: mxc_detect avg %.3f ms/frame on host (100 runs)\n", ms);
}

int main(void) {
  test_basic_colors();
  test_hue_wraparound();
  test_min_area();
  test_max_results_and_two_blobs();
  test_edge_clipping();
  test_grey_never_matches();
  test_sample();
  test_same_color();
  test_label_overflow();
  test_concave_blob();
  test_nearest_hue_wins();
  test_odd_offset();
  test_timing();
  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("all color tests passed\n");
  return 0;
}
