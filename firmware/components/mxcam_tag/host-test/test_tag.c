#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mxcam_tag.h"
#include "tag36h11.h"

static int fails;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      fails++;                                            \
    }                                                     \
  } while (0)
#define APPROX(a, b, tol) CHECK(abs((int)(a) - (int)(b)) <= (tol))

#define W MXT_FRAME_W
#define H MXT_FRAME_H
#define PI 3.14159265358979323846

static uint8_t gray[W * H];
static uint16_t frame[W * H];
static apriltag_family_t *fam;

/* Light horizontal gradient (200..240) so the background is not perfectly flat. */
static void background(void) {
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) gray[y * W + x] = (uint8_t)(200 + (40 * x) / W + (10 * y) / H);
}

/* tag36h11 layout, 10x10 cells: ring 0 white, ring 1 black, 6x6 data bits
 * placed with the family's bit_x/bit_y (offset by the black ring), MSB first,
 * exactly as upstream apriltag_to_image() draws it. */
static void tag_cells(int id, uint8_t cells[10][10]) {
  uint64_t code = fam->codes[id];
  int border_start = (fam->total_width - fam->width_at_border) / 2;
  for (int y = 0; y < 10; y++)
    for (int x = 0; x < 10; x++) cells[y][x] = (x == 0 || x == 9 || y == 0 || y == 9) ? 255 : 0;
  for (uint32_t i = 0; i < fam->nbits; i++)
    if (code & ((uint64_t)1 << (fam->nbits - i - 1)))
      cells[fam->bit_y[i] + border_start][fam->bit_x[i] + border_start] = 255;
}

/* Draws the tag centred at (cx, cy), cell_px pixels per cell, rotated by
 * angle_deg clockwise on screen (image y axis points down). Inverse mapping
 * with nearest neighbour. */
static void draw_tag(int id, double cx, double cy, double cell_px, double angle_deg) {
  uint8_t cells[10][10];
  tag_cells(id, cells);
  double a = angle_deg * PI / 180.0, c = cos(a), s = sin(a);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
      /* forward: x' = c*u - s*v, y' = s*u + c*v; inverse is the transpose */
      double u = c * dx + s * dy, v = -s * dx + c * dy;
      double cu = u / cell_px + 5.0, cv = v / cell_px + 5.0;
      if (cu < 0 || cv < 0 || cu >= 10 || cv >= 10) continue;
      gray[y * W + x] = cells[(int)cv][(int)cu];
    }
}

static void to_frame(void) {
  for (int i = 0; i < W * H; i++) {
    uint8_t g = gray[i];
    uint16_t v = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
    frame[i] = (uint16_t)((v << 8) | (v >> 8));
  }
}

static int16_t rd16(const uint8_t *p) { return (int16_t)(uint16_t)(p[0] | (p[1] << 8)); }

/* Angle difference in centidegrees, wrapped to -18000..18000. */
static int cdeg_diff(int a, int b) {
  int d = (a - b) % 36000;
  if (d > 18000) d -= 36000;
  if (d < -18000) d += 36000;
  return d;
}

static uint32_t last_us;
static uint8_t run(mxp_result_t *out, uint8_t cap) {
  to_frame();
  return mxt_detect(frame, out, cap, &last_us);
}

/* Common checks for one record against the rendered tag. */
static void check_record(const mxp_result_t *r, int id, double cx, double cy, double angle_deg) {
  CHECK(r->id == id);
  APPROX(r->x, cx, 3);
  APPROX(r->y, cy, 3);
  CHECK(r->extra_len == 18);
  int rot = rd16(r->extra);
  if (abs(cdeg_diff(rot, (int)lround(angle_deg * 100))) > 500)
    printf("  id %d rotation %d cdeg, expected %.0f\n", id, rot, angle_deg * 100);
  CHECK(abs(cdeg_diff(rot, (int)lround(angle_deg * 100))) <= 500);
  CHECK(rot >= -18000 && rot <= 18000);
  CHECK(r->confidence <= 100);
  /* Corners: centroid near the centre, all inside the bbox. */
  int sx = 0, sy = 0, minx = 9999, maxx = -9999, miny = 9999, maxy = -9999;
  for (int k = 0; k < 4; k++) {
    int px = rd16(r->extra + 2 + 4 * k), py = rd16(r->extra + 4 + 4 * k);
    sx += px;
    sy += py;
    if (px < minx) minx = px;
    if (px > maxx) maxx = px;
    if (py < miny) miny = py;
    if (py > maxy) maxy = py;
  }
  APPROX(sx / 4, cx, 3);
  APPROX(sy / 4, cy, 3);
  APPROX(r->w, maxx - minx, 2);
  APPROX(r->h, maxy - miny, 2);
  /* Detected quad is the black border: 8 cells (48 px at 6 px/cell) per side. */
  double side = 8 * 6.0, a = angle_deg * PI / 180.0;
  double ext = side * (fabs(cos(a)) + fabs(sin(a)));
  APPROX(r->w, ext, 4);
  APPROX(r->h, ext, 4);
}

static void test_empty(void) {
  mxp_result_t out[MXP_MAX_RESULTS];
  background();
  CHECK(run(out, MXP_MAX_RESULTS) == 0);
}

static void test_upright_id3(void) {
  mxp_result_t out[MXP_MAX_RESULTS];
  background();
  draw_tag(3, 100, 120, 6, 0);
  uint8_t n = run(out, MXP_MAX_RESULTS);
  CHECK(n == 1);
  if (n >= 1) check_record(&out[0], 3, 100, 120, 0);
}

static void test_rotated_id100(void) {
  mxp_result_t out[MXP_MAX_RESULTS];
  background();
  draw_tag(100, 200, 110, 6, 30);
  uint8_t n = run(out, MXP_MAX_RESULTS);
  CHECK(n == 1);
  if (n >= 1) check_record(&out[0], 100, 200, 110, 30);
}

static void test_negative_rotation(void) {
  mxp_result_t out[MXP_MAX_RESULTS];
  background();
  draw_tag(3, 160, 120, 6, -150);
  uint8_t n = run(out, MXP_MAX_RESULTS);
  CHECK(n == 1);
  if (n >= 1) check_record(&out[0], 3, 160, 120, -150);
}

static void test_two_tags(void) {
  mxp_result_t out[MXP_MAX_RESULTS];
  background();
  draw_tag(3, 80, 120, 6, 0);
  draw_tag(100, 230, 120, 6, 30);
  uint8_t n = run(out, MXP_MAX_RESULTS);
  CHECK(n == 2);
  if (n == 2) {
    const mxp_result_t *t3 = out[0].id == 3 ? &out[0] : &out[1];
    const mxp_result_t *t100 = out[0].id == 3 ? &out[1] : &out[0];
    check_record(t3, 3, 80, 120, 0);
    check_record(t100, 100, 230, 120, 30);
    CHECK(out[0].confidence >= out[1].confidence); /* sorted by margin */
  }
  /* cap limits the count */
  CHECK(run(out, 1) == 1);
  CHECK(run(out, 0) == 0);
}

/* Informational: average host time over several runs (clock() is coarse). */
static void report_timing(void) {
  mxp_result_t out[MXP_MAX_RESULTS];
  enum { RUNS = 50 };
  background();
  draw_tag(3, 80, 120, 6, 0);
  draw_tag(100, 230, 120, 6, 30);
  unsigned long total = 0;
  for (int i = 0; i < RUNS; i++) {
    run(out, MXP_MAX_RESULTS);
    total += last_us;
  }
  printf("host detect, 2 tags, 320x240, decimate 2: avg %lu us over %d runs\n", total / RUNS, RUNS);
}

static void test_lifecycle(void) {
  mxp_result_t out[MXP_MAX_RESULTS];
  background();
  draw_tag(3, 100, 120, 6, 0);
  mxt_deinit();
  CHECK(run(out, MXP_MAX_RESULTS) == 0); /* not initialised */
  mxt_deinit();                          /* double deinit is harmless */
  CHECK(mxt_init());
  CHECK(mxt_init()); /* idempotent */
  CHECK(run(out, MXP_MAX_RESULTS) == 1);
  /* setter: decimate 1 still finds the tag */
  mxt_set_quad_decimate(1.0f);
  CHECK(run(out, MXP_MAX_RESULTS) == 1);
  mxt_set_quad_decimate(2.0f);
}

int main(void) {
  fam = tag36h11_create();
  CHECK(mxt_init());
  test_empty();
  test_upright_id3();
  test_rotated_id100();
  test_negative_rotation();
  test_two_tags();
  report_timing();
  test_lifecycle();
  mxt_deinit();
  tag36h11_destroy(fam);
  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("all tag tests passed\n");
  return 0;
}
