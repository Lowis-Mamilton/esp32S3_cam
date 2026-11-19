#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mxcam_face_post.h"

static int fails;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      fails++;                                            \
    }                                                     \
  } while (0)

static mxf_raw_box_t box(int x0, int y0, int x1, int y1, float score) {
  mxf_raw_box_t b = {x0, y0, x1, y1, score};
  return b;
}

static void test_confidence(void) {
  CHECK(mxf_confidence(0.0f) == 0);
  CHECK(mxf_confidence(1.0f) == 100);
  CHECK(mxf_confidence(0.92f) == 92);
  CHECK(mxf_confidence(0.594f) == 59);
  CHECK(mxf_confidence(0.596f) == 60); /* rounded, not truncated */
  CHECK(mxf_confidence(0.005f) == 1);
  CHECK(mxf_confidence(1.7f) == 100); /* clamped */
  CHECK(mxf_confidence(-0.3f) == 0);
  CHECK(mxf_confidence(NAN) == 0);
  CHECK(mxf_confidence(INFINITY) == 100);
  CHECK(mxf_confidence(-INFINITY) == 0);
}

static void test_to_result_full_frame(void) {
  mxp_result_t r;
  memset(&r, 0xAB, sizeof(r));
  mxf_raw_box_t b = box(100, 50, 160, 130, 0.87f);
  CHECK(mxf_to_result(&b, 320, 240, &r));
  CHECK(r.id == 0);
  CHECK(r.w == 60 && r.h == 80);
  CHECK(r.x == 130 && r.y == 90); /* centre; x0 = x - w/2 recovers the box */
  CHECK(r.confidence == 87);
  CHECK(r.extra_len == 0);
}

static void test_to_result_odd_size(void) {
  mxp_result_t r;
  mxf_raw_box_t b = box(10, 20, 41, 53, 0.5f); /* 31 x 33 */
  CHECK(mxf_to_result(&b, 320, 240, &r));
  CHECK(r.w == 31 && r.h == 33);
  CHECK(r.x == 10 + 15 && r.y == 20 + 16);
  CHECK(r.x - r.w / 2 == 10 && r.y - r.h / 2 == 20);
}

static void test_to_result_scaled(void) {
  /* Detector saw a 160x120 image: everything doubles. */
  mxp_result_t r;
  mxf_raw_box_t b = box(40, 30, 70, 70, 0.75f);
  CHECK(mxf_to_result(&b, 160, 120, &r));
  CHECK(r.w == 60 && r.h == 80);
  CHECK(r.x == 110 && r.y == 100);
  /* Non-integer scale rounds: 3 * 320/240 = 4, 7 * 320/240 = 9.33 -> 9. */
  mxf_raw_box_t c = box(3, 3, 7, 7, 0.75f);
  CHECK(mxf_to_result(&c, 240, 180, &r));
  CHECK(r.w == 5 && r.h == 5); /* x 4..9, y 4..9 */
  CHECK(r.x == 4 + 2 && r.y == 4 + 2);
}

static void test_to_result_clipped(void) {
  mxp_result_t r;
  mxf_raw_box_t b = box(-20, -10, 40, 30, 0.9f); /* off the top-left */
  CHECK(mxf_to_result(&b, 320, 240, &r));
  CHECK(r.w == 40 && r.h == 30);
  CHECK(r.x == 20 && r.y == 15);
  mxf_raw_box_t c = box(300, 220, 400, 300, 0.9f); /* off the bottom-right */
  CHECK(mxf_to_result(&c, 320, 240, &r));
  CHECK(r.w == 20 && r.h == 20);
  CHECK(r.x == 310 && r.y == 230);
  mxf_raw_box_t d = box(-100, -100, 1000, 1000, 0.9f); /* bigger than the frame */
  CHECK(mxf_to_result(&d, 320, 240, &r));
  CHECK(r.w == 320 && r.h == 240 && r.x == 160 && r.y == 120);
}

static void test_to_result_rejects(void) {
  mxp_result_t r;
  memset(&r, 0x5A, sizeof(r));
  mxp_result_t before = r;
  mxf_raw_box_t outside = box(330, 10, 360, 40, 0.9f);
  CHECK(!mxf_to_result(&outside, 320, 240, &r));
  mxf_raw_box_t empty = box(50, 50, 50, 80, 0.9f);
  CHECK(!mxf_to_result(&empty, 320, 240, &r));
  mxf_raw_box_t inverted = box(80, 50, 40, 90, 0.9f);
  CHECK(!mxf_to_result(&inverted, 320, 240, &r));
  mxf_raw_box_t ok = box(10, 10, 20, 20, 0.9f);
  CHECK(!mxf_to_result(&ok, 0, 240, &r));
  CHECK(!mxf_to_result(&ok, 320, 0, &r));
  CHECK(memcmp(&r, &before, sizeof(r)) == 0);
}

static void test_select_threshold_and_sort(void) {
  mxf_raw_box_t b[5] = {
      box(0, 0, 20, 20, 0.55f),     /* 55 */
      box(100, 0, 140, 40, 0.95f),  /* 95 */
      box(200, 0, 230, 30, 0.40f),  /* 40: below threshold */
      box(0, 100, 50, 150, 0.70f),  /* 70 */
      box(400, 400, 450, 450, 0.99f) /* outside: dropped */
  };
  mxp_result_t out[8];
  uint8_t n = mxf_select(b, 5, 320, 240, 50, 8, out, 8);
  CHECK(n == 3);
  CHECK(out[0].confidence == 95 && out[0].x == 120);
  CHECK(out[1].confidence == 70 && out[1].y == 125);
  CHECK(out[2].confidence == 55 && out[2].x == 10);
}

static void test_select_threshold_inclusive(void) {
  mxf_raw_box_t b[2] = {box(0, 0, 20, 20, 0.60f), box(50, 0, 70, 20, 0.594f)};
  mxp_result_t out[8];
  uint8_t n = mxf_select(b, 2, 320, 240, 60, 8, out, 8);
  CHECK(n == 1 && out[0].confidence == 60);
  CHECK(mxf_select(b, 2, 320, 240, 0, 8, out, 8) == 2);
  CHECK(mxf_select(b, 2, 320, 240, 101, 8, out, 8) == 0);
}

static void test_select_caps(void) {
  mxf_raw_box_t b[12];
  for (int i = 0; i < 12; i++) b[i] = box(i * 20, 0, i * 20 + 15, 15, 0.50f + 0.04f * (float)i);
  mxp_result_t out[12];
  memset(out, 0, sizeof(out));
  /* max_results caps. */
  uint8_t n = mxf_select(b, 12, 320, 240, 0, 3, out, 12);
  CHECK(n == 3);
  CHECK(out[0].confidence == 94 && out[1].confidence == 90 && out[2].confidence == 86);
  /* cap caps. */
  n = mxf_select(b, 12, 320, 240, 0, 8, out, 2);
  CHECK(n == 2 && out[0].confidence == 94 && out[1].confidence == 90);
  /* MXP_MAX_RESULTS caps, and nothing is written past it. */
  memset(out, 0x77, sizeof(out));
  n = mxf_select(b, 12, 320, 240, 0, 200, out, 12);
  CHECK(n == MXP_MAX_RESULTS);
  CHECK(out[MXP_MAX_RESULTS - 1].confidence == 66);
  CHECK(out[MXP_MAX_RESULTS].confidence == 0x77);
  /* Zero limits. */
  CHECK(mxf_select(b, 12, 320, 240, 0, 0, out, 8) == 0);
  CHECK(mxf_select(b, 12, 320, 240, 0, 8, out, 0) == 0);
  CHECK(mxf_select(b, 0, 320, 240, 0, 8, out, 8) == 0);
}

static void test_select_ties(void) {
  /* Same confidence: larger area first, then detector order. */
  mxf_raw_box_t b[3] = {box(0, 0, 10, 10, 0.8f), box(100, 0, 140, 40, 0.8f), box(200, 0, 210, 10, 0.8f)};
  mxp_result_t out[8];
  uint8_t n = mxf_select(b, 3, 320, 240, 50, 8, out, 8);
  CHECK(n == 3);
  CHECK(out[0].x == 120);
  CHECK(out[1].x == 5);
  CHECK(out[2].x == 205);
}

static void test_select_best_survive_cap(void) {
  /* The best boxes come last in detector order: a cap must still keep them. */
  mxf_raw_box_t b[10];
  for (int i = 0; i < 10; i++) b[i] = box(i * 30, 0, i * 30 + 20, 20, 0.30f + 0.07f * (float)i);
  mxp_result_t out[8];
  uint8_t n = mxf_select(b, 10, 320, 240, 0, 2, out, 8);
  CHECK(n == 2);
  CHECK(out[0].confidence == 93 && out[0].x == 9 * 30 + 10);
  CHECK(out[1].confidence == 86 && out[1].x == 8 * 30 + 10);
}

int main(void) {
  test_confidence();
  test_to_result_full_frame();
  test_to_result_odd_size();
  test_to_result_scaled();
  test_to_result_clipped();
  test_to_result_rejects();
  test_select_threshold_and_sort();
  test_select_threshold_inclusive();
  test_select_caps();
  test_select_ties();
  test_select_best_survive_cap();
  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("all face post tests passed\n");
  return 0;
}
