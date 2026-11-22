#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mxcam_qr_post.h"

static int fails;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      fails++;                                            \
    }                                                     \
  } while (0)

/* A code with the given corners (TL, TR, BR, BL) and a text payload. */
static mxq_code_t code4(int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, const char *text) {
  mxq_code_t c;
  memset(&c, 0, sizeof(c));
  c.x[0] = (int16_t)x0;
  c.y[0] = (int16_t)y0;
  c.x[1] = (int16_t)x1;
  c.y[1] = (int16_t)y1;
  c.x[2] = (int16_t)x2;
  c.y[2] = (int16_t)y2;
  c.x[3] = (int16_t)x3;
  c.y[3] = (int16_t)y3;
  if (text) mxq_set_payload(&c, (const uint8_t *)text, strlen(text));
  return c;
}

/* Axis-aligned square code: top-left (x, y), side s (corners at x .. x + s). */
static mxq_code_t square(int x, int y, int s, const char *text) {
  return code4(x, y, x + s, y, x + s, y + s, x, y + s, text);
}

/* Square of side `side` centred on (cx, cy), turned `deg` clockwise on screen
 * (y down), corners rounded to integers. */
static mxq_code_t rotated(double cx, double cy, double side, double deg) {
  const double pi = 3.14159265358979323846;
  double a = deg * pi / 180.0, h = side / 2.0;
  const double px[4] = {-h, h, h, -h}, py[4] = {-h, -h, h, h};
  int xs[4], ys[4];
  for (int k = 0; k < 4; k++) {
    xs[k] = (int)lround(cx + px[k] * cos(a) - py[k] * sin(a));
    ys[k] = (int)lround(cy + px[k] * sin(a) + py[k] * cos(a));
  }
  return code4(xs[0], ys[0], xs[1], ys[1], xs[2], ys[2], xs[3], ys[3], "R");
}

static void test_to_result_axis_aligned(void) {
  mxq_code_t c = code4(100, 60, 160, 60, 160, 120, 100, 120, "HELLO");
  mxp_result_t r;
  memset(&r, 0xAB, sizeof(r));
  mxq_to_result(&c, 3, &r);
  CHECK(r.id == 3);
  CHECK(r.x == 130);
  CHECK(r.y == 90);
  CHECK(r.w == 61);
  CHECK(r.h == 61);
  CHECK(r.confidence == 255);
  CHECK(r.extra_len == 18);
  mxp_corners_extra_t e;
  CHECK(mxp_unpack_corners_extra(&r, &e));
  CHECK(e.rotation_cdeg == 0);
  CHECK(e.x[0] == 100 && e.y[0] == 60);
  CHECK(e.x[1] == 160 && e.y[1] == 60);
  CHECK(e.x[2] == 160 && e.y[2] == 120);
  CHECK(e.x[3] == 100 && e.y[3] == 120);

  mxq_to_result(&c, 0, &r);
  CHECK(r.id == 0);
}

static void test_to_result_rotated(void) {
  mxp_result_t r;
  mxp_corners_extra_t e;
  mxq_code_t c = rotated(160, 120, 80, 30);
  mxq_to_result(&c, 1, &r);
  CHECK(mxp_unpack_corners_extra(&r, &e));
  CHECK(abs(e.rotation_cdeg - 3000) <= 50);
  /* Corners are the code's own, unchanged. */
  for (int k = 0; k < 4; k++) CHECK(e.x[k] == c.x[k] && e.y[k] == c.y[k]);
  /* Box still centred on the code. */
  CHECK(abs(r.x - 160) <= 1 && abs(r.y - 120) <= 1);

  c = rotated(160, 120, 80, -90);
  mxq_to_result(&c, 1, &r);
  CHECK(mxp_unpack_corners_extra(&r, &e));
  CHECK(abs(e.rotation_cdeg + 9000) <= 50);

  /* Upside down reads +-180 degrees. */
  c = rotated(160, 120, 80, 180);
  mxq_to_result(&c, 1, &r);
  CHECK(mxp_unpack_corners_extra(&r, &e));
  CHECK(abs(abs(e.rotation_cdeg) - 18000) <= 50);
}

static void test_to_result_clips_bbox(void) {
  mxq_code_t c = code4(-20, -10, 40, -10, 40, 50, -20, 50, "EDGE");
  mxp_result_t r;
  mxq_to_result(&c, 0, &r);
  CHECK(r.w == 41);
  CHECK(r.h == 51);
  CHECK(r.x - (int)(r.w / 2) == 0); /* left edge of the box is x0 = 0 */
  CHECK(r.y - (int)(r.h / 2) == 0);
  mxp_corners_extra_t e;
  CHECK(mxp_unpack_corners_extra(&r, &e));
  CHECK(e.x[0] == -20 && e.y[0] == -10); /* extra corners are not clipped */
  CHECK(e.x[2] == 40 && e.y[2] == 50);

  /* Bottom-right edge: clipped to 319 / 239. */
  c = code4(300, 220, 340, 220, 340, 260, 300, 260, "BR");
  mxq_to_result(&c, 0, &r);
  CHECK(r.w == 20); /* 300 .. 319 */
  CHECK(r.h == 20); /* 220 .. 239 */
  CHECK(r.x == 309 && r.y == 229);
  CHECK(mxp_unpack_corners_extra(&r, &e));
  CHECK(e.x[1] == 340 && e.y[2] == 260);
}

static void test_area2(void) {
  mxq_code_t c = square(0, 0, 10, NULL);
  CHECK(mxq_area2(&c) == 200);
  /* Counter-clockwise order gives the same (absolute) area. */
  c = code4(0, 0, 0, 10, 10, 10, 10, 0, NULL);
  CHECK(mxq_area2(&c) == 200);
  c = code4(0, 0, 20, 0, 20, 5, 0, 5, NULL);
  CHECK(mxq_area2(&c) == 200);
  /* Large coordinates do not overflow. */
  c = code4(-32768, -32768, 32767, -32768, 32767, 32767, -32768, 32767, NULL);
  CHECK(mxq_area2(&c) > 0);
}

static void test_sort_limit_keeps_largest(void) {
  mxq_code_t codes[6];
  const int sides[6] = {10, 50, 30, 70, 20, 40};
  char text[6][2] = {"A", "B", "C", "D", "E", "F"};
  for (int i = 0; i < 6; i++) codes[i] = square(i * 10, i * 5, sides[i], text[i]);
  uint8_t n = mxq_sort_limit(codes, 6, 4);
  CHECK(n == 4);
  CHECK(codes[0].bytes[0] == 'D'); /* 70 */
  CHECK(codes[1].bytes[0] == 'B'); /* 50 */
  CHECK(codes[2].bytes[0] == 'F'); /* 40 */
  CHECK(codes[3].bytes[0] == 'C'); /* 30 */
  for (int i = 1; i < 4; i++) CHECK(mxq_area2(&codes[i - 1]) >= mxq_area2(&codes[i]));

  /* Fewer than max: count unchanged, order unchanged when already sorted. */
  mxq_code_t two[2] = {square(0, 0, 40, "X"), square(100, 100, 20, "Y")};
  n = mxq_sort_limit(two, 2, 4);
  CHECK(n == 2);
  CHECK(two[0].bytes[0] == 'X' && two[1].bytes[0] == 'Y');

  /* Equal areas keep input order (stable). */
  mxq_code_t eq[3] = {square(0, 0, 20, "P"), square(50, 0, 30, "Q"), square(100, 0, 20, "R")};
  n = mxq_sort_limit(eq, 3, 3);
  CHECK(n == 3);
  CHECK(eq[0].bytes[0] == 'Q' && eq[1].bytes[0] == 'P' && eq[2].bytes[0] == 'R');

  CHECK(mxq_sort_limit(eq, 0, 4) == 0);
  CHECK(mxq_sort_limit(eq, 3, 0) == 0);
}

static void test_pick_for_learn(void) {
  /* A centred (30, 30) small; B centred (200, 150) large. */
  mxq_code_t codes[3] = {square(20, 20, 20, "A"), square(160, 110, 80, "B"), square(180, 130, 40, "C")};
  /* Region holding only B's (and C's) centre: B is larger. */
  CHECK(mxq_pick_for_learn(codes, 3, 150, 100, 100, 100) == 1);
  /* Region holding only A's centre. */
  CHECK(mxq_pick_for_learn(codes, 3, 0, 0, 60, 60) == 0);
  /* Only B (two codes): region contains B's centre only. */
  CHECK(mxq_pick_for_learn(codes, 2, 190, 140, 20, 20) == 1);
  /* Both A and B centres inside (whole frame): the larger. */
  CHECK(mxq_pick_for_learn(codes, 2, 0, 0, 320, 240) == 1);
  /* None inside. */
  CHECK(mxq_pick_for_learn(codes, 3, 250, 0, 50, 50) == -1);
  CHECK(mxq_pick_for_learn(codes, 0, 0, 0, 320, 240) == -1);
  /* Region reaching past the frame still selects a code centred at (310, 220). */
  mxq_code_t edge = square(305, 215, 10, "E");
  mxp_result_t r;
  mxq_to_result(&edge, 0, &r);
  CHECK(r.x == 310 && r.y == 220);
  CHECK(mxq_pick_for_learn(&edge, 1, 300, 200, 100, 100) == 0);
  /* Region edges: x .. x + w - 1 inside, x + w outside. */
  CHECK(mxq_pick_for_learn(&edge, 1, 310, 220, 1, 1) == 0);
  CHECK(mxq_pick_for_learn(&edge, 1, 300, 200, 10, 30) == -1);
  CHECK(mxq_pick_for_learn(&edge, 1, 311, 200, 9, 30) == -1);
  /* Maximum region sizes do not overflow. */
  CHECK(mxq_pick_for_learn(&edge, 1, 0, 0, 0xFFFF, 0xFFFF) == 0);
}

static void test_set_payload_truncates(void) {
  mxq_code_t c;
  memset(&c, 0xEE, sizeof(c));
  mxq_set_payload(&c, (const uint8_t *)"HELLO", 5);
  CHECK(c.len == 5);
  CHECK(c.total_len == 5);
  CHECK(memcmp(c.bytes, "HELLO", 5) == 0);

  uint8_t big[300];
  for (int i = 0; i < 300; i++) big[i] = (uint8_t)i;
  mxq_set_payload(&c, big, 300);
  CHECK(c.len == 128);
  CHECK(c.total_len == 300);
  CHECK(memcmp(c.bytes, big, 128) == 0);

  mxq_set_payload(&c, big, 128);
  CHECK(c.len == 128 && c.total_len == 128);

  mxq_set_payload(&c, NULL, 0);
  CHECK(c.len == 0 && c.total_len == 0);

  /* total_len saturates (quirc's payload limit is far below this anyway). */
  mxq_set_payload(&c, big, 70000);
  CHECK(c.len == 128 && c.total_len == 65535);
}

static int label_is(uint16_t id, const mxq_code_t *c, const char *want) {
  char buf[32];
  int n = mxq_label(id, c, buf, sizeof(buf));
  return n == (int)strlen(want) && strcmp(buf, want) == 0;
}

static void test_label(void) {
  mxq_code_t c = square(0, 0, 10, "HELLO");
  CHECK(label_is(3, &c, "QR3 HELLO"));
  CHECK(label_is(0, &c, "QR HELLO"));
  c = square(0, 0, 10, "ABCDEFGHIJK");
  CHECK(label_is(1, &c, "QR1 ABCDEFGH"));
  c = square(0, 0, 10, "");
  CHECK(label_is(8, &c, "QR8"));
  CHECK(label_is(0, &c, "QR"));

  /* Snprintf semantics. */
  char buf[16];
  c = square(0, 0, 10, "HELLO");
  memset(buf, 'X', sizeof(buf));
  CHECK(mxq_label(3, &c, buf, 5) == 9);
  CHECK(memcmp(buf, "QR3 ", 4) == 0);
  CHECK(buf[4] == 0);
  int canary_ok = 1;
  for (size_t i = 5; i < sizeof(buf); i++)
    if (buf[i] != 'X') canary_ok = 0;
  CHECK(canary_ok);
  CHECK(mxq_label(3, &c, NULL, 0) == 9);
}

static void test_label_non_ascii(void) {
  mxq_code_t c = square(0, 0, 10, NULL);
  const uint8_t raw[4] = {0x41, 0x00, 0xE4, 0x7F};
  mxq_set_payload(&c, raw, 4);
  CHECK(label_is(0, &c, "QR A???"));
  /* Space and '~' are printable. */
  mxq_set_payload(&c, (const uint8_t *)" ~", 2);
  CHECK(label_is(2, &c, "QR2  ~"));
}

int main(void) {
  test_to_result_axis_aligned();
  test_to_result_rotated();
  test_to_result_clips_bbox();
  test_area2();
  test_sort_limit_keeps_largest();
  test_pick_for_learn();
  test_set_payload_truncates();
  test_label();
  test_label_non_ascii();
  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("all qr post tests passed\n");
  return 0;
}
