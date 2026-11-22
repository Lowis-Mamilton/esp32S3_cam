#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mxcam_gray.h"
#include "mxcam_motion.h"

/* MXM_TEST_HOOKS (mxcam_motion.c) */
void mxm_test_canary_arm(void);
bool mxm_test_canary_ok(void);

static int fails;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      fails++;                                            \
    }                                                     \
  } while (0)
#define CHECK_N(got, want)                                                                \
  do {                                                                                    \
    int g_ = (int)(got), w_ = (int)(want);                                                \
    if (g_ != w_) {                                                                       \
      printf("FAIL %s:%d: %s = %d, want %d\n", __FILE__, __LINE__, #got, g_, w_);          \
      fails++;                                                                            \
    }                                                                                     \
  } while (0)

#define FW 320
#define FH 240

static uint16_t frame[FW * FH];

/* RGB888 -> big-endian RGB565 as stored by the camera (swapped on LE CPU). */
static uint16_t px(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  return (uint16_t)((v << 8) | (v >> 8));
}
static uint16_t grey(int v) {
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return px((uint8_t)v, (uint8_t)v, (uint8_t)v);
}
#define BLACK 0x0000

/* Fixed-seed grey texture: 8x8 blocks, grey 40..200 (no clipping at +30). */
static int tex_at(int x, int y) {
  uint32_t h = (uint32_t)((x >> 3) + 1000) * 73856093u ^ (uint32_t)((y >> 3) + 1000) * 19349663u;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return 40 + (int)(h % 161u);
}
/* Texture shifted left by dx pixels (camera panned) and brightened by add. */
static void texture(int dx, int add) {
  for (int y = 0; y < FH; y++)
    for (int x = 0; x < FW; x++) frame[y * FW + x] = grey(tex_at(x + dx, y) + add);
}
static void flat(int v) {
  for (int i = 0; i < FW * FH; i++) frame[i] = grey(v);
}
static void rect(int x, int y, int w, int h, uint16_t c) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++)
      if (i >= 0 && i < FW && j >= 0 && j < FH) frame[j * FW + i] = c;
}

static mxp_motion_config_t defaults(void) {
  mxp_motion_config_t c;
  mxp_motion_config_defaults(&c);
  return c;
}
static mxp_result_t res[MXP_MAX_RESULTS];
static mxm_stats_t st;
static uint8_t run(const mxp_motion_config_t *c) {
  memset(res, 0, sizeof res);
  memset(&st, 0xEE, sizeof st);
  return mxm_detect(frame, c, res, MXP_MAX_RESULTS, &st);
}
/* Reset and feed the static texture through the warm-up. */
static void warm_texture(const mxp_motion_config_t *c) {
  mxm_reset();
  for (int i = 0; i < MXM_WARMUP; i++) {
    texture(0, 0);
    CHECK_N(run(c), 0);
  }
}

/* Review point 3: the first frames after switching to function 12 never report. */
static void test_motion_warmup(void) {
  mxp_motion_config_t c = defaults();
  mxm_reset();
  for (int i = 0; i < MXM_WARMUP; i++) {
    texture(0, 0);
    rect(40 + 30 * i, 100, 40, 40, BLACK);
    CHECK_N(run(&c), 0);
    CHECK(st.global_reset == 0);
    CHECK(st.changed_cells == 0);
  }
  texture(0, 0);
  rect(40 + 30 * MXM_WARMUP, 100, 40, 40, BLACK);
  CHECK(run(&c) >= 1);
  /* mxm_reset starts the warm-up again */
  mxm_reset();
  texture(0, 0);
  CHECK_N(run(&c), 0);
}

static void test_static_scene_no_motion(void) {
  mxp_motion_config_t c = defaults();
  mxm_reset();
  for (int i = 0; i < 20; i++) {
    texture(0, 0);
    CHECK_N(run(&c), 0);
    CHECK(st.global_reset == 0);
    if (i >= MXM_WARMUP) CHECK_N(st.changed_cells, 0);
  }
}

/* The centre is (min + max) / 2 rounded down, so the box starts at x - (w - 1) / 2. */
static bool in_frame(const mxp_result_t *r) {
  int x0 = r->x - (r->w - 1) / 2, y0 = r->y - (r->h - 1) / 2;
  return r->w > 0 && r->h > 0 && x0 >= 0 && y0 >= 0 && x0 + r->w <= FW && y0 + r->h <= FH;
}
/* x0 <= bx && bx + bw <= x0 + w (within tol) */
static bool box_contains(const mxp_result_t *r, int bx, int by, int bw, int bh, int tol) {
  int x0 = r->x - (r->w - 1) / 2, y0 = r->y - (r->h - 1) / 2;
  return x0 <= bx + tol && y0 <= by + tol && x0 + r->w >= bx + bw - tol && y0 + r->h >= by + bh - tol;
}

static void test_moving_block_detected(void) {
  mxp_motion_config_t c = defaults();
  mxm_reset();
  for (int i = 0; i < 10; i++) {
    int bx = 40 + 10 * i;
    texture(0, 0);
    rect(bx, 100, 40, 40, BLACK);
    uint8_t n = run(&c);
    if (i < MXM_WARMUP) {
      CHECK_N(n, 0);
      continue;
    }
    CHECK_N(n, 1);
    if (n != 1) continue;
    CHECK(res[0].id == 0);
    CHECK(box_contains(&res[0], bx, 100, 40, 40, 2));
    CHECK(res[0].confidence > 0 && res[0].confidence <= 100);
    CHECK(res[0].extra_len == 0);
    CHECK(in_frame(&res[0]));
    CHECK(st.changed_cells > 0 && st.global_reset == 0);
  }
  /* stats may be NULL */
  texture(0, 0);
  rect(150, 100, 40, 40, BLACK);
  CHECK(mxm_detect(frame, &c, res, MXP_MAX_RESULTS, NULL) == 1);
}

/* Review point 3: auto exposure / a lamp brightening the whole picture. */
static void test_motion_global_brightness_shift(void) {
  mxp_motion_config_t c = defaults();
  warm_texture(&c);
  for (int i = 0; i < 3; i++) {
    texture(0, 0);
    CHECK_N(run(&c), 0);
  }
  for (int i = 0; i < 6; i++) {
    texture(0, 30);
    CHECK_N(run(&c), 0);
  }
  /* and back down */
  for (int i = 0; i < 6; i++) {
    texture(0, 0);
    CHECK_N(run(&c), 0);
  }
}

/* Review point 3: the camera is moved (held in the hand). */
static void test_motion_camera_pan_resets(void) {
  mxp_motion_config_t c = defaults();
  warm_texture(&c);
  for (int i = 0; i < 2; i++) {
    texture(0, 0);
    CHECK_N(run(&c), 0);
  }
  texture(20, 0);
  CHECK_N(run(&c), 0);
  CHECK(st.global_reset == 1);
  CHECK(st.changed_cells > MXM_W * MXM_H / 2);
  for (int i = 0; i < 3; i++) {
    texture(20, 0);
    CHECK_N(run(&c), 0);
    CHECK(st.global_reset == 0);
  }
  /* after the reset, real motion is reported again */
  texture(20, 0);
  rect(100, 100, 40, 40, BLACK);
  CHECK_N(run(&c), 1);
}

/* Exposure compensation uses the median difference: a dark object covering a
 * quarter of the frame would shift a mean by ~30 grey levels and make every
 * static cell look changed (-> global reset, nothing reported). */
static void test_large_object_not_global(void) {
  mxp_motion_config_t c = defaults();
  warm_texture(&c);
  for (int i = 0; i < 3; i++) {
    int bx = 60 + 20 * i;
    texture(0, 0);
    rect(bx, 60, 160, 120, BLACK);
    CHECK_N(run(&c), 1);
    CHECK(st.global_reset == 0);
    CHECK(box_contains(&res[0], bx, 60, 160, 120, 2));
    CHECK(res[0].w <= 160 + 20 * i + 4);
  }
}

static void test_small_motion_ignored(void) {
  mxp_motion_config_t c = defaults(); /* min_size 24 */
  warm_texture(&c);
  const int pos[3][2] = {{100, 100}, {180, 140}, {60, 40}};
  for (int i = 0; i < 3; i++) {
    texture(0, 0);
    rect(pos[i][0], pos[i][1], 12, 12, BLACK);
    CHECK_N(run(&c), 0);
    CHECK(st.changed_cells > 0);
  }
  c.min_size = 8;
  warm_texture(&c);
  texture(0, 0);
  rect(100, 100, 12, 12, BLACK);
  CHECK_N(run(&c), 1);
  CHECK_N(res[0].w, 12);
  CHECK_N(res[0].h, 12);
  CHECK_N(res[0].confidence, 100);
}

static void test_threshold_param(void) {
  CHECK(abs((int)mxg_luma_be(grey(116)) - (int)mxg_luma_be(grey(100))) >= 14);
  CHECK(abs((int)mxg_luma_be(grey(116)) - (int)mxg_luma_be(grey(100))) <= 17);
  for (int pass = 0; pass < 2; pass++) {
    mxp_motion_config_t c = defaults();
    c.threshold = pass == 0 ? 25 : 10;
    mxm_reset();
    for (int i = 0; i < MXM_WARMUP; i++) {
      flat(100);
      CHECK_N(run(&c), 0);
    }
    for (int i = 0; i < 8; i++) {
      flat(100);
      rect(40 + 10 * i, 100, 40, 40, grey(116));
      CHECK_N(run(&c), pass == 0 ? 0 : 1);
    }
  }
}

static void test_max_results(void) {
  mxm_test_canary_arm();
  for (int pass = 0; pass < 2; pass++) {
    mxp_motion_config_t c = defaults();
    c.max_results = pass == 0 ? 2 : 8;
    mxm_reset();
    for (int i = 0; i < MXM_WARMUP; i++) {
      flat(160);
      CHECK_N(run(&c), 0);
    }
    /* flat background: the fading trail behind each block stays one piece */
    for (int i = 0; i < 6; i++) {
      flat(160);
      rect(20 + 16 * i, 20, 56, 56, BLACK);
      rect(20 + 16 * i, 160, 40, 40, BLACK);
      rect(180 + 8 * i, 112, 24, 24, BLACK);
      uint8_t n = run(&c);
      CHECK_N(n, pass == 0 ? 2 : 3);
      for (int k = 1; k < n; k++) CHECK(res[k - 1].w * res[k - 1].h >= res[k].w * res[k].h);
    }
  }
  /* cap below max_results */
  mxp_motion_config_t c = defaults();
  warm_texture(&c);
  texture(0, 0);
  rect(200, 20, 56, 56, BLACK);
  rect(20, 160, 40, 40, BLACK);
  CHECK(mxm_detect(frame, &c, res, 1, NULL) == 1);
  /* blocks at the frame edges and corners never overrun */
  const int pos[][2] = {{-20, -20}, {300, -20}, {-20, 220}, {300, 220}, {0, 0}, {280, 200}};
  for (int i = 0; i < 6; i++) {
    texture(0, 0);
    rect(pos[i][0], pos[i][1], 40, 40, BLACK);
    uint8_t n = run(&c);
    for (int k = 0; k < n; k++) {
      CHECK(in_frame(&res[k]));
    }
  }
  CHECK(mxm_test_canary_ok());
}

static void test_still_object_absorbed(void) {
  mxp_motion_config_t c = defaults();
  warm_texture(&c);
  for (int i = 0; i < 5; i++) {
    texture(0, 0);
    rect(60 + 10 * i, 100, 40, 40, BLACK);
    CHECK_N(run(&c), 1);
  }
  int first_zero = -1;
  uint8_t n = 0;
  for (int i = 0; i < 30; i++) {
    texture(0, 0);
    rect(100, 100, 40, 40, BLACK);
    n = run(&c);
    if (n == 0 && first_zero < 0) first_zero = i;
  }
  CHECK_N(n, 0);
  CHECK(first_zero >= 5); /* not absorbed at once */
  printf("info: a stopped object blends in after %d frames\n", first_zero + 1);
}

static void test_timing(void) {
  mxp_motion_config_t c = defaults();
  warm_texture(&c);
  clock_t t0 = clock();
  for (int i = 0; i < 200; i++) {
    texture(0, 0);
    rect(40 + (i % 20) * 10, 100, 40, 40, BLACK);
    (void)run(&c);
  }
  clock_t t1 = clock();
  for (int i = 0; i < 200; i++) {
    texture(0, 0);
    rect(40 + (i % 20) * 10, 100, 40, 40, BLACK);
  }
  clock_t t2 = clock();
  double ms = (double)((t1 - t0) - (t2 - t1)) * 1000.0 / CLOCKS_PER_SEC / 200.0;
  printf("info: mxm_detect avg on host: %.3f ms\n", ms);
}

int main(void) {
  test_motion_warmup();
  test_static_scene_no_motion();
  test_moving_block_detected();
  test_motion_global_brightness_shift();
  test_motion_camera_pan_resets();
  test_large_object_not_global();
  test_small_motion_ignored();
  test_threshold_param();
  test_max_results();
  test_still_object_absorbed();
  test_timing();
  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
