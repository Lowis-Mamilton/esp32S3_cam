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

/* Draws a 10x10 cell grid centred at (cx, cy), cell_px pixels per cell,
 * rotated by angle_deg clockwise on screen (image y axis points down). Inverse
 * mapping with nearest neighbour. */
static void draw_cells(uint8_t cells[10][10], double cx, double cy, double cell_px, double angle_deg) {
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

/* "Upright" is the official AprilRobotics image (apriltag-imgs PNG, same as
 * upstream apriltag_to_image), which the MXCam rotation/corner output uses.
 * draw_tag() renders that grid as is. draw_tag_opencv() renders the marker
 * OpenCV/ArUco generates for DICT_APRILTAG_36h11, which is the official image
 * turned 180 degrees (checked with OpenCV 5.1 for IDs 3 and 12). */
static void draw_tag(int id, double cx, double cy, double cell_px, double angle_deg) {
  uint8_t cells[10][10];
  tag_cells(id, cells);
  draw_cells(cells, cx, cy, cell_px, angle_deg);
}

static void draw_tag_opencv(int id, double cx, double cy, double cell_px, double angle_deg) {
  uint8_t up[10][10], cells[10][10];
  tag_cells(id, up);
  for (int y = 0; y < 10; y++)
    for (int x = 0; x < 10; x++) cells[y][x] = up[9 - y][9 - x];
  draw_cells(cells, cx, cy, cell_px, angle_deg);
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

/* Corners must come as the official tag's TL, TR, BR, BL (clockwise): the black
 * square's corners at (-4,-4), (4,-4), (4,4), (-4,4) cells, rotated with the tag. */
static void check_corners(const mxp_result_t *r, double cx, double cy, double cell, double angle_deg, int tol) {
  static const int su[4] = { -1, 1, 1, -1 }, sv[4] = { -1, -1, 1, 1 };
  double a = angle_deg * PI / 180.0, c = cos(a), s = sin(a), h = 4 * cell;
  for (int k = 0; k < 4; k++) {
    double u = su[k] * h, v = sv[k] * h;
    double ex = cx + c * u - s * v, ey = cy + s * u + c * v;
    int px = rd16(r->extra + 2 + 4 * k), py = rd16(r->extra + 4 + 4 * k);
    if (abs(px - (int)lround(ex)) > tol || abs(py - (int)lround(ey)) > tol)
      printf("  corner %d at (%d,%d), expected (%.0f,%.0f)\n", k, px, py, ex, ey);
    APPROX(px, lround(ex), tol);
    APPROX(py, lround(ey), tol);
  }
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
  check_corners(r, cx, cy, 6, angle_deg, 2);
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
}

/* Upstream only decimates by exactly 1.5 or an integer factor, but rescales
 * corners by the raw value, so other values would give wrong coordinates. */
static void test_decimate_setter(void) {
  mxp_result_t out[MXP_MAX_RESULTS];
  background();
  draw_tag(3, 100, 120, 6, 0);
  static const float good[] = {1.0f, 1.5f, 3.0f, 4.0f, 2.0f}; /* ends on the default */
  for (unsigned i = 0; i < sizeof good / sizeof good[0]; i++) {
    CHECK(mxt_set_quad_decimate(good[i]));
    uint8_t n = run(out, MXP_MAX_RESULTS);
    if (n != 1) printf("  decimate %.1f: %u results\n", (double)good[i], n);
    CHECK(n == 1);
    if (n == 1) check_record(&out[0], 3, 100, 120, 0);
  }
  static const float bad[] = {2.5f, 1.2f, 0.5f, 0.0f, -2.0f, 5.0f, 1.75f};
  for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    CHECK(!mxt_set_quad_decimate(bad[i]));
    uint8_t n = run(out, MXP_MAX_RESULTS); /* still decimate 2: unchanged */
    CHECK(n == 1);
    if (n == 1) check_record(&out[0], 3, 100, 120, 0);
  }
  CHECK(!mxt_set_quad_decimate((float)NAN));
  CHECK(!mxt_set_quad_decimate((float)INFINITY));
  CHECK(run(out, MXP_MAX_RESULTS) == 1);
  if (out[0].extra_len == 18) check_record(&out[0], 3, 100, 120, 0);
  /* accepted value survives deinit/init */
  CHECK(mxt_set_quad_decimate(3.0f));
  mxt_deinit();
  CHECK(mxt_init());
  CHECK(run(out, MXP_MAX_RESULTS) == 1);
  CHECK(mxt_set_quad_decimate(2.0f));
}

/* Tag turned 45 degrees near the right edge: its right corner (fitted from the
 * visible edges) lies outside the frame. Corners are reported unclipped, the
 * bbox is clipped. x is the homography centre, not the bbox centre. */
static void test_clipped_bbox(void) {
  mxp_result_t out[MXP_MAX_RESULTS];
  background();
  draw_tag(3, 290, 120, 6, 45);
  uint8_t n = run(out, MXP_MAX_RESULTS);
  CHECK(n == 1);
  if (n != 1) return;
  CHECK(out[0].id == 3);
  APPROX(out[0].x, 290, 3);
  int minx = 9999, maxx = -9999;
  for (int k = 0; k < 4; k++) {
    int px = rd16(out[0].extra + 2 + 4 * k);
    if (px < minx) minx = px;
    if (px > maxx) maxx = px;
  }
  CHECK(maxx > MXT_FRAME_W);              /* a corner is off-frame */
  CHECK(out[0].w > 0);
  CHECK(out[0].w < maxx - minx);          /* clipped */
  CHECK(minx + out[0].w <= MXT_FRAME_W + 1); /* bbox right edge at the frame edge (rounding) */
  APPROX(minx + out[0].w, MXT_FRAME_W, 1);
  CHECK(out[0].h > 0);
}

/* The OpenCV/ArUco-generated marker drawn upright is the official image upside
 * down: rotation reads +-180 and the official TL is its bottom-right corner. */
static void test_opencv_orientation(void) {
  mxp_result_t out[MXP_MAX_RESULTS];
  background();
  draw_tag_opencv(12, 160, 120, 6, 0);
  uint8_t n = run(out, MXP_MAX_RESULTS);
  CHECK(n == 1);
  if (n == 1) check_record(&out[0], 12, 160, 120, 180);
}

/* Half-res mode: 160x120 gray, coordinates scaled back to 320x240. */
static void check_half(const mxp_result_t *r, int id, double cx, double cy, double angle_deg, double cell) {
  CHECK(r->id == id);
  APPROX(r->x, cx, 3);
  APPROX(r->y, cy, 3);
  CHECK(r->extra_len == 18);
  int rot = rd16(r->extra);
  CHECK(abs(cdeg_diff(rot, (int)lround(angle_deg * 100))) <= 500);
  double a = angle_deg * PI / 180.0, ext = 8 * cell * (fabs(cos(a)) + fabs(sin(a)));
  APPROX(r->w, ext, 5);
  APPROX(r->h, ext, 5);
  check_corners(r, cx, cy, cell, angle_deg, 3);
}

/* The table-driven gray conversion equals the reference formula for every pixel. */
static void test_luma_exhaustive(void) {
  int bad = 0;
  for (unsigned v = 0; v < 65536; v++) {
    unsigned r5 = v >> 11, g6 = (v >> 5) & 0x3Fu, b5 = v & 0x1Fu;
    unsigned r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
    uint8_t want = (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
    uint8_t mem[2] = { (uint8_t)(v >> 8), (uint8_t)v }; /* big-endian in memory */
    uint16_t px;
    memcpy(&px, mem, 2);
    if (mxt_luma_be(px) != want) bad++;
  }
  CHECK(bad == 0);
}

static void test_half_res(void) {
  mxt_tuning_t def, t;
  mxt_get_tuning(&def);
  CHECK(def.half_res == 0 && def.min_white_black_diff == 20 && def.max_nmaxima == 10 &&
        def.min_cluster_pixels == 5 && def.refine_edges == 1 && def.max_line_fit_mse == 10);
  t = def;
  t.max_nmaxima = 0;
  CHECK(!mxt_set_tuning(&t));
  t = def;
  t.max_line_fit_mse = 0;
  CHECK(!mxt_set_tuning(&t));
  t = def;
  t.half_res = 1;
  CHECK(mxt_set_tuning(&t));
  mxt_tuning_t got;
  mxt_get_tuning(&got);
  CHECK(got.half_res == 1);

  mxp_result_t out[MXP_MAX_RESULTS];
  background();
  CHECK(run(out, MXP_MAX_RESULTS) == 0);
  background();
  draw_tag(3, 100, 120, 8, 0);
  uint8_t n = run(out, MXP_MAX_RESULTS);
  CHECK(n == 1);
  if (n >= 1) check_half(&out[0], 3, 100, 120, 0, 8);
  background();
  draw_tag(100, 200, 110, 8, 30);
  n = run(out, MXP_MAX_RESULTS);
  CHECK(n == 1);
  if (n >= 1) check_half(&out[0], 100, 200, 110, 30, 8);
  printf("half-res detect: %u us (host)\n", (unsigned)last_us);
  mxt_profile_t pr;
  mxt_last_profile(&pr);
  CHECK(pr.total_us == last_us && pr.nquads >= 1);
  CHECK(mxt_set_tuning(&def));
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0); /* keep output if a test crashes */
  fam = tag36h11_create();
  CHECK(mxt_init());
  test_empty();
  test_upright_id3();
  test_rotated_id100();
  test_negative_rotation();
  test_two_tags();
  report_timing();
  test_lifecycle();
  test_decimate_setter();
  test_clipped_bbox();
  test_opencv_orientation();
  test_half_res();
  test_luma_exhaustive();
  mxt_deinit();
  tag36h11_destroy(fam);
  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("all tag tests passed\n");
  return 0;
}
