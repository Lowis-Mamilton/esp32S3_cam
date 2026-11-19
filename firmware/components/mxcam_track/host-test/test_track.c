#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mxcam_track.h"

/* MXK_TEST_HOOKS (mxcam_track.c) */
void mxk_test_canary_arm(void);
bool mxk_test_canary_ok(void);
uint32_t mxk_test_macs(void); /* NCC multiply-accumulates in the last mxk_track call */

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

static uint16_t frame[FW * FH];

/* RGB888 -> big-endian RGB565 as stored by the camera (swapped on LE CPU). */
static uint16_t px(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  return (uint16_t)((v << 8) | (v >> 8));
}
#define BG ((uint16_t)0x1084) /* RGB565 0x8410 (mid grey) in camera byte order */
#define RED px(255, 0, 0)
#define WHITE px(255, 255, 255)

static void fill(uint16_t c) {
  for (int i = 0; i < FW * FH; i++) frame[i] = c;
}
/* size x size checkerboard with (possibly fractional) cells, top-left (x, y), clipped. */
static void checker(int x, int y, int size, float cell, uint16_t c0, uint16_t c1) {
  for (int j = 0; j < size; j++)
    for (int i = 0; i < size; i++) {
      int fx = x + i, fy = y + j;
      if (fx < 0 || fx >= FW || fy < 0 || fy >= FH) continue;
      frame[fy * FW + fx] = (((int)(i / cell) + (int)(j / cell)) & 1) ? c1 : c0;
    }
}
static void stripes(int x, int y, int size, int cell, uint16_t c0, uint16_t c1) {
  for (int j = 0; j < size; j++)
    for (int i = 0; i < size; i++) {
      int fx = x + i, fy = y + j;
      if (fx < 0 || fx >= FW || fy < 0 || fy >= FH) continue;
      frame[fy * FW + fx] = ((j / cell) & 1) ? c1 : c0;
    }
}
/* size x size of random red/white cell x cell blocks (fixed seed): a
 * non-periodic texture, like a real object, with the same fine detail. */
static void blocks(int x, int y, int size, int cell, uint32_t seed) {
  int n = (size + cell - 1) / cell;
  for (int bj = 0; bj < n; bj++)
    for (int bi = 0; bi < n; bi++) {
      seed = seed * 1664525u + 1013904223u;
      uint16_t c = (seed >> 16) & 1 ? WHITE : RED;
      for (int j = bj * cell; j < (bj + 1) * cell && j < size; j++)
        for (int i = bi * cell; i < (bi + 1) * cell && i < size; i++) {
          int fx = x + i, fy = y + j;
          if (fx >= 0 && fx < FW && fy >= 0 && fy < FH) frame[fy * FW + fx] = c;
        }
    }
}
/* The standard target: 40x40, 8 px red/white cells on grey. */
static void scene(int x, int y) {
  fill(BG);
  checker(x, y, 40, 8.0f, RED, WHITE);
}

static mxp_track_config_t def_cfg(void) {
  mxp_track_config_t c;
  mxp_track_config_defaults(&c);
  return c;
}

/* Learns the standard target at (100, 80) and resets the tracker. */
static bool learn_std(mxk_model_t *m, mxk_state_t *st) {
  scene(100, 80);
  if (!mxk_learn(frame, 100, 80, 40, 40, m)) return false;
  mxk_reset(st, m);
  return true;
}

static void test_learn_ok(void) {
  mxk_model_t m;
  scene(100, 80);
  CHECK(mxk_learn(frame, 100, 80, 40, 40, &m));
  CHECK(m.tw >= MXK_TMPL_MIN && m.tw <= MXK_TMPL_MAX);
  CHECK(m.th >= MXK_TMPL_MIN && m.th <= MXK_TMPL_MAX);
  NEAR(m.box_w, 40, 0);
  NEAR(m.box_h, 40, 0);
  NEAR(m.cx, 120, 0);
  NEAR(m.cy, 100, 0);
  CHECK(mxk_model_valid(&m));
  /* histogram sums to 65535; mean colour is between red and white */
  uint32_t sum = 0;
  for (int i = 0; i < MXK_HIST_BINS; i++) sum += m.hist[i];
  NEAR(sum, 65535, 0);
  CHECK(m.rgb[0] >= 250 && m.rgb[1] > 100 && m.rgb[1] < 160);
}

static void test_learn_too_small(void) {
  mxk_model_t m, before;
  memset(&m, 0x5A, sizeof m);
  before = m;
  scene(100, 80);
  CHECK(!mxk_learn(frame, 100, 80, 12, 12, &m));
  CHECK(memcmp(&m, &before, sizeof m) == 0);
}

static void test_learn_flat(void) {
  mxk_model_t m, before;
  memset(&m, 0x5A, sizeof m);
  before = m;
  fill(BG);
  CHECK(!mxk_learn(frame, 100, 80, 40, 40, &m));
  CHECK(memcmp(&m, &before, sizeof m) == 0);
}

/* Review point 3: the region is clipped to the frame. */
static void test_learn_region_clipped(void) {
  mxk_model_t m;
  scene(280, 200); /* target sits in the bottom-right corner */
  CHECK(mxk_learn(frame, 300, 220, 40, 40, &m));
  NEAR(m.box_w, 20, 0);
  NEAR(m.box_h, 20, 0);
  NEAR(m.cx, 310, 0);
  NEAR(m.cy, 230, 0);
  CHECK(mxk_model_valid(&m));
  /* fully outside -> false */
  CHECK(!mxk_learn(frame, 320, 100, 40, 40, &m));
}

static void test_track_same_frame(void) {
  mxk_model_t m;
  mxk_state_t st;
  mxp_track_config_t cfg = def_cfg();
  mxp_result_t r;
  CHECK(learn_std(&m, &st));
  CHECK(mxk_track(frame, &m, &cfg, &st, &r) == 1);
  NEAR(r.x, 120, 2);
  NEAR(r.y, 100, 2);
  CHECK(r.confidence >= 90);
  NEAR(r.id, 1, 0);
  NEAR(r.extra_len, 0, 0);
  CHECK(!st.lost);
}

static void test_track_moved(void) {
  mxk_model_t m;
  mxk_state_t st;
  mxp_track_config_t cfg = def_cfg();
  mxp_result_t r;
  CHECK(learn_std(&m, &st));
  scene(112, 72);
  CHECK(mxk_track(frame, &m, &cfg, &st, &r) == 1);
  NEAR(r.x, 132, 2);
  NEAR(r.y, 92, 2);
  NEAR(r.w, 40, 4);
}

static void test_track_scaled(void) {
  mxk_model_t m;
  mxk_state_t st;
  mxp_track_config_t cfg = def_cfg();
  mxp_result_t r;
  CHECK(learn_std(&m, &st));
  fill(BG);
  checker(98, 78, 44, 8.8f, RED, WHITE); /* 1.1x, same centre (120, 100) */
  CHECK(mxk_track(frame, &m, &cfg, &st, &r) == 1);
  NEAR(r.w, 44, 6); /* within 15 % */
  CHECK(r.w > 40);  /* the 1.1 scale won */
  NEAR(r.h, 44, 6);
  NEAR(r.x, 120, 2);
  NEAR(r.y, 100, 2);
  /* scale off: still found at the learned size */
  cfg.scale = 0;
  mxk_reset(&st, &m);
  CHECK(mxk_track(frame, &m, &cfg, &st, &r) == 1);
  NEAR(r.w, 40, 0);
  NEAR(r.x, 120, 3);
}

/* Moves the standard target right 10 px per frame until it has fully left;
 * returns with st lost (checked). */
static void leave_right(const mxk_model_t *m, const mxp_track_config_t *cfg, mxk_state_t *st) {
  mxp_result_t r;
  for (int x = 110; x < FW + 20; x += 10) {
    scene(x, 80);
    uint8_t n = mxk_track(frame, m, cfg, st, &r);
    if (x >= FW) {
      CHECK(n == 0);
      CHECK(st->lost);
    } else if (x + 40 <= FW) {
      CHECK(n == 1); /* fully visible: tracked */
      if (n) NEAR(r.x, x + 20, 2);
    }
  }
}

/* Review point 3: leaving the frame -> lost; reappearing far away -> found
 * again with reacquire, never with reacquire off; buffers never overrun. */
static void test_object_leaves_and_returns(void) {
  mxk_model_t m;
  mxk_state_t st;
  mxp_track_config_t cfg = def_cfg();
  mxp_result_t r;
  mxk_test_canary_arm();
  CHECK(learn_std(&m, &st));
  leave_right(&m, &cfg, &st);
  int found_at = -1;
  for (int i = 0; i < 2 && found_at < 0; i++) {
    scene(40, 160);
    if (mxk_track(frame, &m, &cfg, &st, &r) == 1) found_at = i;
  }
  CHECK(found_at >= 0);
  if (found_at >= 0) {
    NEAR(r.x, 60, 2);
    NEAR(r.y, 180, 2);
    CHECK(!st.lost);
  }
  CHECK(mxk_test_canary_ok());

  cfg.reacquire = 0;
  mxk_reset(&st, &m);
  leave_right(&m, &cfg, &st);
  for (int i = 0; i < 4; i++) {
    scene(40, 160);
    CHECK(mxk_track(frame, &m, &cfg, &st, &r) == 0);
    CHECK(st.lost);
  }
  NEAR(st.lost_frames >= 5, 1, 0);
  /* edges and corners: partially visible targets never overrun */
  cfg.reacquire = 1;
  const int pos[][2] = {{-20, -20}, {300, -20}, {-20, 220}, {300, 220}, {0, 0}, {280, 200}};
  for (int i = 0; i < 6; i++) {
    scene(pos[i][0], pos[i][1]);
    (void)mxk_track(frame, &m, &cfg, &st, &r);
  }
  CHECK(mxk_test_canary_ok());
}

static void test_distractor_rejected(void) {
  mxk_model_t m;
  mxk_state_t st;
  mxp_track_config_t cfg = def_cfg();
  mxp_result_t r;
  CHECK(learn_std(&m, &st));
  /* same colours, horizontal stripes: first where the target was, then elsewhere */
  for (int i = 0; i < 10; i++) {
    fill(BG);
    if (i < 5) stripes(100, 80, 40, 8, RED, WHITE); else stripes(200, 150, 40, 8, RED, WHITE);
    CHECK(mxk_track(frame, &m, &cfg, &st, &r) == 0);
  }
  CHECK(st.lost);
}

static void test_no_drift(void) {
  mxk_model_t m;
  mxk_state_t st;
  mxp_track_config_t cfg = def_cfg();
  mxp_result_t r;
  for (int pass = 0; pass < 2; pass++) {
    fill(BG);
    checker(100, 80, 40, 8.0f, px(160, 20, 20), px(200, 200, 200));
    CHECK(mxk_learn(frame, 100, 80, 40, 40, &m));
    mxk_reset(&st, &m);
    cfg.adapt = pass == 0 ? 10 : 0;
    int found = 0;
    for (int k = 1; k <= 40; k++) {
      fill(BG);
      checker(100, 80, 40, 8.0f, px((uint8_t)(160 + k), (uint8_t)(20 + k), (uint8_t)(20 + k)),
              px((uint8_t)(200 + k), (uint8_t)(200 + k), (uint8_t)(200 + k)));
      if (mxk_track(frame, &m, &cfg, &st, &r) == 1) {
        found++;
        NEAR(r.x, 120, 2);
        NEAR(r.y, 100, 2);
      }
    }
    NEAR(found, 40, 0);
    if (pass == 1) CHECK(memcmp(st.live, m.tmpl, sizeof st.live) == 0);
  }
}

static void test_model_valid(void) {
  mxk_model_t m, bad;
  scene(100, 80);
  CHECK(mxk_learn(frame, 100, 80, 40, 40, &m));
  CHECK(mxk_model_valid(&m));
  bad = m;
  bad.tw = 40;
  CHECK(!mxk_model_valid(&bad));
  bad = m;
  bad.th = 7;
  CHECK(!mxk_model_valid(&bad));
  bad = m;
  bad.box_w = 4;
  CHECK(!mxk_model_valid(&bad));
  bad = m;
  bad.box_h = 241;
  CHECK(!mxk_model_valid(&bad));
  bad = m;
  bad.cx = 320;
  CHECK(!mxk_model_valid(&bad));
  bad = m;
  bad.cy = -1;
  CHECK(!mxk_model_valid(&bad));
}

/* Large box (72 px, non-periodic 8 px blocks): the fallbacks keep the NCC work
 * within MXK_MAC_BUDGET and the target is still tracked while moving. Prints
 * host timings. (A periodic 8 px checker this large can lock onto a one-period
 * alias: see ADR-0018.) */
static void test_mac_budget_large_box(void) {
  mxk_model_t m;
  mxk_state_t st;
  mxp_track_config_t cfg = def_cfg();
  mxp_result_t r;
  fill(BG);
  blocks(120, 80, 72, 8, 7u);
  CHECK(mxk_learn(frame, 120, 80, 72, 72, &m));
  NEAR(m.tw, 32, 0);
  mxk_reset(&st, &m);
  uint32_t max_macs = 0;
  for (int k = 0; k < 8; k++) {
    fill(BG);
    blocks(120 + 4 * k, 80 - 2 * k, 72, 8, 7u);
    CHECK(mxk_track(frame, &m, &cfg, &st, &r) == 1);
    NEAR(r.x, 156 + 4 * k, 2);
    NEAR(r.y, 116 - 2 * k, 2);
    if (mxk_test_macs() > max_macs) max_macs = mxk_test_macs();
  }
  CHECK(max_macs <= MXK_MAC_BUDGET);
  printf("info: 72 px box: max %lu NCC MACs/frame (budget %lu)\n", (unsigned long)max_macs,
         (unsigned long)MXK_MAC_BUDGET);

  /* host timing */
  clock_t t0 = clock();
  for (int i = 0; i < 100; i++) (void)mxk_track(frame, &m, &cfg, &st, &r);
  double big = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 100.0;
  CHECK(learn_std(&m, &st));
  scene(112, 72);
  t0 = clock();
  uint32_t macs40 = 0;
  for (int i = 0; i < 100; i++) {
    (void)mxk_track(frame, &m, &cfg, &st, &r);
    macs40 = mxk_test_macs();
  }
  double small = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 100.0;
  /* lost + reacquire: back-projection, then NCC at the candidate, every frame */
  scene(40, 160);
  t0 = clock();
  for (int i = 0; i < 100; i++) {
    st.lost = 1;
    CHECK(mxk_track(frame, &m, &cfg, &st, &r) == 1);
  }
  double lost = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 100.0;
  printf("info: mxk_track avg on host: 40 px box %.3f ms (%lu MACs), 72 px box %.3f ms, lost+reacquire %.3f ms\n",
         small, (unsigned long)macs40, big, lost);
}

int main(void) {
  test_learn_ok();
  test_learn_too_small();
  test_learn_flat();
  test_learn_region_clipped();
  test_track_same_frame();
  test_track_moved();
  test_track_scaled();
  test_object_leaves_and_returns();
  test_distractor_rejected();
  test_no_drift();
  test_model_valid();
  test_mac_budget_large_box();
  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
