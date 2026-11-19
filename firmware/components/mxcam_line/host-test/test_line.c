#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mxcam_gray.h"
#include "mxcam_line.h"

static int fails;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      fails++;                                            \
    }                                                     \
  } while (0)
#define NEAR(a, b, tol)                                                                        \
  do {                                                                                         \
    int a_ = (int)(a), b_ = (int)(b);                                                          \
    if (abs(a_ - b_) > (tol)) {                                                                \
      printf("FAIL %s:%d: %s = %d, want %d +-%d\n", __FILE__, __LINE__, #a, a_, b_, (int)(tol)); \
      fails++;                                                                                 \
    }                                                                                          \
  } while (0)

#define FW 320
#define FH 240
#define PI_F 3.14159265f

static uint16_t frame[FW * FH];
static uint8_t half[MXG_HALF_W * MXG_HALF_H];

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
static void rect(int x, int y, int w, int h, uint16_t c) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++)
      if (i >= 0 && i < FW && j >= 0 && j < FH) frame[j * FW + i] = c;
}

/* Paints every pixel whose centre is closer than width/2 to the segment
 * (x0,y0)-(x1,y1) (round caps). Coordinates are continuous pixel units. */
static void draw_thick_line(uint16_t *f, float x0, float y0, float x1, float y1, float width, uint16_t c) {
  float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy, r = width / 2.0f;
  for (int y = 0; y < FH; y++)
    for (int x = 0; x < FW; x++) {
      float cx = x + 0.5f, cy = y + 0.5f;
      float t = len2 > 0.0f ? ((cx - x0) * dx + (cy - y0) * dy) / len2 : 0.0f;
      if (t < 0.0f) t = 0.0f;
      if (t > 1.0f) t = 1.0f;
      float ex = x0 + t * dx - cx, ey = y0 + t * dy - cy;
      if (ex * ex + ey * ey < r * r) f[y * FW + x] = c;
    }
}

static mxp_line_config_t defaults(void) {
  mxp_line_config_t c;
  mxp_line_config_defaults(&c);
  return c;
}

/* Runs detection from a fresh state; returns the count and decodes the extra. */
static uint8_t detect(const mxp_line_config_t *cfg, const uint8_t *rgb, mxp_result_t *r, mxp_line_extra_t *e) {
  mxl_state_t st;
  memset(&st, 0, sizeof st);
  memset(r, 0, sizeof *r);
  memset(e, 0, sizeof *e);
  uint8_t n = mxl_detect(frame, cfg, rgb, &st, r, NULL);
  if (n == 1) CHECK(mxp_unpack_line_extra(r, e));
  return n;
}

/* Reference luma: Y = (77R + 150G + 29B) >> 8 on 8-bit expanded channels. */
static uint8_t ref_luma(uint16_t be) {
  uint16_t v = (uint16_t)((be >> 8) | (be << 8));
  unsigned r5 = v >> 11, g6 = (v >> 5) & 63u, b5 = v & 31u;
  unsigned r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
  return (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
}

static void test_gray_values(void) {
  CHECK(mxg_luma_be(WHITE) == 255);
  CHECK(mxg_luma_be(BLACK) == 0);
  /* pure red 0xF800 stored big-endian: (77 * 255) >> 8 = 76 (same as mxt_luma_be) */
  CHECK(mxg_luma_be(px(255, 0, 0)) == 76);
  CHECK(px(255, 0, 0) == 0x00F8);
  long bad = 0;
  for (uint32_t p = 0; p <= 0xFFFFu; p++)
    if (mxg_luma_be((uint16_t)p) != ref_luma((uint16_t)p)) bad++;
  CHECK(bad == 0);

  /* mxg_half: rounded mean of each 2x2 block */
  fill(WHITE);
  frame[0] = WHITE; frame[1] = WHITE; frame[FW] = BLACK; frame[FW + 1] = BLACK;   /* (255*2)/4 -> 128 */
  frame[2] = WHITE; frame[3] = BLACK; frame[FW + 2] = BLACK; frame[FW + 3] = BLACK; /* 255/4 -> 64 */
  rect(318, 238, 2, 2, grey(100));
  memset(half, 0xAA, sizeof half);
  mxg_half(frame, half);
  CHECK(half[0] == 128);
  CHECK(half[1] == 64);
  CHECK(half[2] == 255);
  CHECK(half[MXG_HALF_W] == 255);
  CHECK(half[MXG_HALF_W * MXG_HALF_H - 1] == mxg_luma_be(grey(100)));
  frame[5 * FW + 10] = px(255, 0, 0); /* (76 + 3*255 + 2) >> 2 = 210 */
  mxg_half(frame, half);
  CHECK(half[2 * MXG_HALF_W + 5] == (76 + 3 * 255 + 2) / 4);
}

static void test_vertical_line(void) {
  fill(WHITE);
  draw_thick_line(frame, 200, 0, 200, 240, 12, BLACK);
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  CHECK(detect(&cfg, NULL, &r, &e) == 1);
  CHECK(r.id == 1);
  CHECK(abs(e.angle_cdeg) <= 100);
  NEAR(e.offset_x, 40, 2);
  NEAR(e.x0, 200, 2);
  NEAR(e.x1, 200, 2);
  CHECK(e.y0 > e.y1);
  CHECK(e.y0 >= 222 && e.y1 >= 96 && e.y1 < 114); /* bottom and top band centres */
  CHECK(r.confidence == 100);
  CHECK(e.flags == 0);
  CHECK(r.extra_len == 14);
  NEAR(r.x, 200, 2);
  CHECK(r.y == (e.y0 + e.y1) / 2);
  CHECK(r.w >= 2 && r.w <= 5);
  CHECK(r.h == (uint16_t)(e.y0 - e.y1 + 1));
}

static void test_tilted_line(void) {
  fill(WHITE);
  draw_thick_line(frame, 100, 239, 220, 96, 12, BLACK);
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  CHECK(detect(&cfg, NULL, &r, &e) == 1);
  int want = (int)lroundf(atan2f(120.0f, 143.0f) * 18000.0f / PI_F);
  NEAR(e.angle_cdeg, want, 200);
  CHECK(e.angle_cdeg > 0);
  CHECK(e.x1 > e.x0);
  CHECK(r.w == (uint16_t)(e.x1 - e.x0 + 1));
}

static void test_empty_frame(void) {
  fill(WHITE);
  mxp_line_config_t cfg = defaults();
  mxl_state_t st = {1, 80};
  mxp_result_t r;
  CHECK(mxl_detect(frame, &cfg, NULL, &st, &r, NULL) == 0);
  CHECK(st.has_last == 0);
}

static void test_low_contrast_noise(void) {
  srand(12345);
  for (int i = 0; i < FW * FH; i++) frame[i] = grey((uint8_t)(120 + rand() % 21));
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  CHECK(detect(&cfg, NULL, &r, &e) == 0);
}

static void test_light_line_mode(void) {
  fill(BLACK);
  draw_thick_line(frame, 120, 0, 120, 240, 12, WHITE);
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  cfg.mode = MXP_LINE_MODE_LIGHT;
  CHECK(detect(&cfg, NULL, &r, &e) == 1);
  NEAR(e.x0, 120, 2);
  cfg.mode = MXP_LINE_MODE_DARK;
  CHECK(detect(&cfg, NULL, &r, &e) == 0);
}

static void test_fixed_threshold(void) {
  fill(WHITE);
  draw_thick_line(frame, 150, 0, 150, 240, 12, grey(90));
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  cfg.threshold = 100;
  CHECK(detect(&cfg, NULL, &r, &e) == 1);
  NEAR(e.x0, 150, 2);
  cfg.threshold = 50; /* the line (~89) is not darker than 50 */
  CHECK(detect(&cfg, NULL, &r, &e) == 0);
}

static void test_branch_y(void) {
  fill(WHITE);
  draw_thick_line(frame, 160, 239, 160, 170, 12, BLACK);
  draw_thick_line(frame, 160, 170, 100, 96, 12, BLACK);
  draw_thick_line(frame, 160, 170, 220, 96, 12, BLACK);
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  CHECK(detect(&cfg, NULL, &r, &e) == 1);
  CHECK(e.flags & MXP_LINE_FLAG_BRANCH);
  /* one main line: the fit runs up the trunk into one branch, so it leans */
  NEAR(e.x0, 160, 20);
  CHECK(abs(e.angle_cdeg) >= 500);
}

static void test_crossing(void) {
  fill(WHITE);
  draw_thick_line(frame, 160, 0, 160, 240, 12, BLACK);
  /* Horizontal cross line 14 px wide. y = 158 keeps it inside one band (band 4
   * of the default 8 covers rows 150..167); a 14 px line straddling a band
   * boundary is under half of either band and is not seen in that frame. */
  draw_thick_line(frame, -10, 158, 330, 158, 14, BLACK);
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  CHECK(detect(&cfg, NULL, &r, &e) == 1);
  CHECK(e.flags & MXP_LINE_FLAG_CROSS);
  CHECK(abs(e.angle_cdeg) <= 100);
  NEAR(e.x0, 160, 2);
}

static void test_learned_color(void) {
  static const uint8_t line_rgb[3] = {230, 40, 40};
  fill(grey(128));
  draw_thick_line(frame, 200, 0, 200, 240, 12, px(230, 40, 40));
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  cfg.mode = MXP_LINE_MODE_LEARNED;
  CHECK(detect(&cfg, line_rgb, &r, &e) == 1);
  NEAR(e.x0, 200, 2);
  /* the same frame in mode 0: red (~96) on grey 128 is below the Otsu contrast floor */
  cfg.mode = MXP_LINE_MODE_DARK;
  CHECK(detect(&cfg, line_rgb, &r, &e) == 0);
  cfg.mode = MXP_LINE_MODE_LEARNED;
  fill(grey(128));
  draw_thick_line(frame, 200, 0, 200, 240, 12, px(40, 40, 230));
  CHECK(detect(&cfg, line_rgb, &r, &e) == 0);
}

static void test_learned_mode_without_color_falls_back(void) {
  fill(WHITE);
  draw_thick_line(frame, 200, 0, 200, 240, 12, BLACK);
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  cfg.mode = MXP_LINE_MODE_LEARNED;
  CHECK(detect(&cfg, NULL, &r, &e) == 1);
  NEAR(e.x0, 200, 2);
}

static void test_line_above_roi(void) {
  fill(WHITE);
  draw_thick_line(frame, 160, 0, 160, 60, 12, BLACK);
  mxp_line_config_t cfg = defaults();
  CHECK(cfg.roi_pct == 60);
  mxp_result_t r;
  mxp_line_extra_t e;
  CHECK(detect(&cfg, NULL, &r, &e) == 0);
}

static void test_short_stub(void) {
  fill(WHITE);
  draw_thick_line(frame, 160, 228, 160, 240, 12, BLACK); /* only band 0 (rows 222..239) */
  mxp_line_config_t cfg = defaults();
  mxl_state_t st = {1, 80};
  mxp_result_t r;
  mxl_trace_t tr;
  CHECK(mxl_detect(frame, &cfg, NULL, &st, &r, &tr) == 0);
  CHECK(st.has_last == 0);
  CHECK(tr.n == 1);
}

static void test_edge_line(void) {
  fill(WHITE);
  rect(0, 0, 6, FH, BLACK);
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  CHECK(detect(&cfg, NULL, &r, &e) == 1);
  NEAR(e.x0, 3, 2);
  CHECK(e.x0 >= 0 && e.x1 >= 0);
  NEAR(e.offset_x, 3 - 160, 2);
}

static void test_continuity(void) {
  fill(WHITE);
  draw_thick_line(frame, 80, 0, 80, 240, 12, BLACK);
  draw_thick_line(frame, 240, 0, 240, 240, 12, BLACK);
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  mxl_state_t st = {1, 40};
  CHECK(mxl_detect(frame, &cfg, NULL, &st, &r, NULL) == 1);
  CHECK(mxp_unpack_line_extra(&r, &e));
  NEAR(e.x0, 80, 2);
  CHECK(st.has_last == 1);
  NEAR(st.last_x0, 40, 1);
  st.last_x0 = 120;
  CHECK(mxl_detect(frame, &cfg, NULL, &st, &r, NULL) == 1);
  CHECK(mxp_unpack_line_extra(&r, &e));
  NEAR(e.x0, 240, 2);
  NEAR(st.last_x0, 120, 1);
}

static void test_shadow_half_frame(void) {
  fill(WHITE);
  rect(0, 0, 160, FH, grey(60));
  draw_thick_line(frame, 220, 0, 220, 240, 12, BLACK);
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  CHECK(detect(&cfg, NULL, &r, &e) == 1);
  NEAR(e.x0, 220, 2);
  NEAR(e.x1, 220, 2);
}

static void test_trace_points(void) {
  fill(WHITE);
  draw_thick_line(frame, 200, 0, 200, 240, 12, BLACK);
  mxp_line_config_t cfg = defaults();
  mxl_state_t st;
  memset(&st, 0, sizeof st);
  mxp_result_t r;
  mxl_trace_t tr;
  memset(&tr, 0xAA, sizeof tr);
  CHECK(mxl_detect(frame, &cfg, NULL, &st, &r, &tr) == 1);
  CHECK(tr.n == cfg.bands);
  CHECK(tr.roi_top == 96);
  for (int i = 0; i < tr.n && i < MXL_MAX_BANDS; i++) {
    NEAR(tr.pts[i][0], 200, 2);
    if (i > 0) CHECK(tr.pts[i][1] < tr.pts[i - 1][1]);
    CHECK(tr.pts[i][1] >= 96 && tr.pts[i][1] < 240);
  }
  /* other band counts and ROI sizes */
  cfg.bands = 12;
  cfg.roi_pct = 100;
  CHECK(mxl_detect(frame, &cfg, NULL, &st, &r, &tr) == 1);
  CHECK(tr.n == 12);
  CHECK(tr.roi_top == 0);
  cfg.bands = 4;
  cfg.roi_pct = 20;
  CHECK(mxl_detect(frame, &cfg, NULL, &st, &r, &tr) == 1);
  CHECK(tr.n == 4);
  CHECK(r.confidence == 100);
}

/* Band rows in 320 units with the defaults (8 bands of 18 rows over 96..239):
 * band k covers rows 222 - 18k .. 239 - 18k. */

static void test_line_only_in_upper_bands(void) {
  fill(WHITE);
  rect(194, 0, 12, 204, BLACK); /* rows 0..203: bands 2..7 only, bands 0 and 1 empty */
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  mxl_trace_t tr;
  mxl_state_t st;
  memset(&st, 0, sizeof st);
  CHECK(mxl_detect(frame, &cfg, NULL, &st, &r, &tr) == 1);
  CHECK(mxp_unpack_line_extra(&r, &e));
  NEAR(e.x0, 200, 2);
  NEAR(e.y0, 195, 1); /* centre of band 2 */
  CHECK(tr.n == 6);
  CHECK(r.confidence == 6 * 100 / 8);
}

static void test_off_centre_line_band0_empty(void) {
  fill(WHITE);
  rect(254, 0, 12, 222, BLACK); /* x 254..265 (100 px right of centre), bands 1..7 */
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  CHECK(detect(&cfg, NULL, &r, &e) == 1);
  NEAR(e.x0, 260, 2);
  NEAR(e.offset_x, 100, 2);
  CHECK(abs(e.angle_cdeg) <= 100);
}

static void test_left_leaning_line(void) {
  fill(WHITE);
  draw_thick_line(frame, 220, 239, 100, 96, 12, BLACK);
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  CHECK(detect(&cfg, NULL, &r, &e) == 1);
  int want = -(int)lroundf(atan2f(120.0f, 143.0f) * 18000.0f / PI_F);
  NEAR(e.angle_cdeg, want, 200);
  CHECK(e.angle_cdeg < 0);
  CHECK(e.x1 < e.x0);
  CHECK(e.offset_x > 0);
}

static void test_gap_stops_walk(void) {
  fill(WHITE);
  rect(154, 186, 12, 54, BLACK); /* bands 0..2 */
  rect(154, 96, 12, 54, BLACK); /* bands 5..7; bands 3 and 4 empty */
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxp_line_extra_t e;
  mxl_trace_t tr;
  mxl_state_t st;
  memset(&st, 0, sizeof st);
  CHECK(mxl_detect(frame, &cfg, NULL, &st, &r, &tr) == 1);
  CHECK(mxp_unpack_line_extra(&r, &e));
  CHECK(tr.n == 3); /* two empty bands in a row after the first pick stop the walk */
  NEAR(e.y0, 231, 1); /* band 0 centre */
  NEAR(e.y1, 195, 1); /* band 2 centre: the upper part is not used */
  NEAR(e.x0, 160, 2);
  CHECK(r.confidence == 3 * 100 / 8);
  /* one empty band is bridged */
  rect(154, 150, 12, 18, BLACK); /* fill band 4 back in: only band 3 is empty */
  CHECK(mxl_detect(frame, &cfg, NULL, &st, &r, &tr) == 1);
  CHECK(tr.n == 7);
}

/* From the third pick on, the prediction is extrapolated from the last two
 * picks. A distractor placed beside the line on the "behind" side is nearer
 * the last pick but farther from the extrapolated position than the line. */
static void test_extrapolation_follows_slope(void) {
  fill(WHITE);
  draw_thick_line(frame, 60, 239, 251, 96, 12, BLACK); /* ~24 px right per band */
  mxp_line_config_t cfg = defaults();
  mxp_result_t r;
  mxl_trace_t ref, tr;
  mxl_state_t st;
  memset(&st, 0, sizeof st);
  CHECK(mxl_detect(frame, &cfg, NULL, &st, &r, &ref) == 1);
  CHECK(ref.n == 8);
  if (ref.n < 4) return;
  int step = ref.pts[2][0] - ref.pts[1][0];
  CHECK(step >= 18 && step <= 30);
  /* distractor centred 12 px left of the band 2 pick, in band 3 (rows 168..185) */
  int dx = (ref.pts[2][0] - 12) & ~1;
  rect(dx - 3, 168, 6, 18, BLACK);
  memset(&st, 0, sizeof st);
  CHECK(mxl_detect(frame, &cfg, NULL, &st, &r, &tr) == 1);
  CHECK(tr.n == 8);
  NEAR(tr.pts[3][0], ref.pts[3][0], 2); /* still on the line, not on the distractor */
  CHECK(abs(tr.pts[3][0] - dx) > 10);
}

static void test_timing(void) {
  fill(WHITE);
  draw_thick_line(frame, 100, 239, 220, 96, 12, BLACK);
  mxp_line_config_t cfg = defaults();
  mxl_state_t st;
  memset(&st, 0, sizeof st);
  mxp_result_t r;
  clock_t t0 = clock();
  for (int i = 0; i < 200; i++) mxl_detect(frame, &cfg, NULL, &st, &r, NULL);
  double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 200.0;
  printf("info: mxl_detect avg %.3f ms/frame on host (200 runs)\n", ms);
}

int main(void) {
  test_gray_values();
  test_vertical_line();
  test_tilted_line();
  test_empty_frame();
  test_low_contrast_noise();
  test_light_line_mode();
  test_fixed_threshold();
  test_branch_y();
  test_crossing();
  test_learned_color();
  test_learned_mode_without_color_falls_back();
  test_line_above_roi();
  test_short_stub();
  test_edge_line();
  test_continuity();
  test_shadow_half_frame();
  test_trace_points();
  test_line_only_in_upper_bands();
  test_off_centre_line_band0_empty();
  test_left_leaning_line();
  test_gap_stops_walk();
  test_extrapolation_follows_slope();
  test_timing();
  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("all line tests passed\n");
  return 0;
}
