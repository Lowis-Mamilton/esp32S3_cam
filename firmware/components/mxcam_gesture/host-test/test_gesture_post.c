#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mxcam_gesture_post.h"

static int fails;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      fails++;                                            \
    }                                                     \
  } while (0)

static mxh_raw_box_t box(int x0, int y0, int x1, int y1, float score) {
  mxh_raw_box_t b = {x0, y0, x1, y1, score};
  return b;
}

static int same_box(const mxh_raw_box_t *b, int x0, int y0, int x1, int y1) {
  return b->x0 == x0 && b->y0 == y0 && b->x1 == x1 && b->y1 == y1;
}

static void test_gesture_ids(void) {
  CHECK(mxh_gesture_id("one") == 1);
  CHECK(mxh_gesture_id("two") == 2);
  CHECK(mxh_gesture_id("three") == 3);
  CHECK(mxh_gesture_id("four") == 4);
  CHECK(mxh_gesture_id("five") == 5);
  CHECK(mxh_gesture_id("like") == 6);
  CHECK(mxh_gesture_id("ok") == 7);
  CHECK(mxh_gesture_id("call") == 8);
  CHECK(mxh_gesture_id("dislike") == 9);
  CHECK(mxh_gesture_id("no_gesture") == 0);
  CHECK(mxh_gesture_id("no_hand") == 0);
  CHECK(mxh_gesture_id(NULL) == 0);
  CHECK(mxh_gesture_id("One") == 0); /* exact match only */
  CHECK(mxh_gesture_id("palm") == 0); /* a class upstream may add later */
  CHECK(mxh_gesture_id("") == 0);
  CHECK(mxh_gesture_id("ok ") == 0);
  /* The values are the protocol's. */
  CHECK(MXP_GESTURE_UNKNOWN == 0);
  CHECK(mxh_gesture_id("one") == MXP_GESTURE_ONE);
  CHECK(mxh_gesture_id("two") == MXP_GESTURE_TWO);
  CHECK(mxh_gesture_id("three") == MXP_GESTURE_THREE);
  CHECK(mxh_gesture_id("four") == MXP_GESTURE_FOUR);
  CHECK(mxh_gesture_id("five") == MXP_GESTURE_FIVE);
  CHECK(mxh_gesture_id("like") == MXP_GESTURE_LIKE);
  CHECK(mxh_gesture_id("ok") == MXP_GESTURE_OK);
  CHECK(mxh_gesture_id("call") == MXP_GESTURE_CALL);
  CHECK(mxh_gesture_id("dislike") == MXP_GESTURE_DISLIKE);
  CHECK(MXP_GESTURE_MAX == 9);
  CHECK(MXP_FUNC_GESTURE == 8);
}

static void test_confidence(void) {
  CHECK(mxh_confidence(0.0f) == 0);
  CHECK(mxh_confidence(1.0f) == 100);
  CHECK(mxh_confidence(0.92f) == 92);
  CHECK(mxh_confidence(0.594f) == 59);
  CHECK(mxh_confidence(0.596f) == 60); /* rounded, not truncated */
  CHECK(mxh_confidence(0.006f) == 1);
  CHECK(mxh_confidence(1.7f) == 100); /* clamped */
  CHECK(mxh_confidence(-0.3f) == 0);
  CHECK(mxh_confidence(NAN) == 0);
  CHECK(mxh_confidence(INFINITY) == 100);
  CHECK(mxh_confidence(-INFINITY) == 0);
}

static void test_pick_hands_threshold_sort(void) {
  mxh_raw_box_t in[3] = {box(10, 10, 60, 70, 0.9f), box(100, 50, 220, 200, 0.7f), box(200, 20, 260, 90, 0.3f)};
  mxh_raw_box_t out[MXH_MAX_HANDS];
  memset(out, 0, sizeof(out));
  uint8_t n = mxh_pick_hands(in, 3, 40, 2, out);
  CHECK(n == 2);
  CHECK(same_box(&out[0], 10, 10, 60, 70) && out[0].score == 0.9f);
  CHECK(same_box(&out[1], 100, 50, 220, 200) && out[1].score == 0.7f);

  n = mxh_pick_hands(in, 3, 40, 1, out);
  CHECK(n == 1);
  CHECK(same_box(&out[0], 10, 10, 60, 70));

  n = mxh_pick_hands(in, 3, 95, 2, out);
  CHECK(n == 0);

  /* Threshold is inclusive: 0.3 -> 30 passes threshold 30. */
  n = mxh_pick_hands(in, 3, 30, 2, out);
  CHECK(n == 2);

  /* Best last in detector order still wins. */
  mxh_raw_box_t rev[3] = {in[2], in[1], in[0]};
  n = mxh_pick_hands(rev, 3, 10, 2, out);
  CHECK(n == 2);
  CHECK(same_box(&out[0], 10, 10, 60, 70));
  CHECK(same_box(&out[1], 100, 50, 220, 200));

  /* Same score: larger area first. */
  mxh_raw_box_t tie[2] = {box(10, 10, 60, 70, 0.8f), box(100, 50, 220, 200, 0.8f)};
  n = mxh_pick_hands(tie, 2, 40, 2, out);
  CHECK(n == 2);
  CHECK(same_box(&out[0], 100, 50, 220, 200));
  CHECK(same_box(&out[1], 10, 10, 60, 70));

  /* Same score and area: input order. */
  mxh_raw_box_t same[2] = {box(10, 10, 60, 70, 0.8f), box(200, 100, 250, 160, 0.8f)};
  n = mxh_pick_hands(same, 2, 40, 2, out);
  CHECK(n == 2);
  CHECK(same_box(&out[0], 10, 10, 60, 70));
  CHECK(same_box(&out[1], 200, 100, 250, 160));

  /* Empty input. */
  CHECK(mxh_pick_hands(in, 0, 0, 2, out) == 0);
  CHECK(mxh_pick_hands(NULL, 0, 0, 2, out) == 0);
}

static void test_pick_hands_clip_and_min_size(void) {
  mxh_raw_box_t out[MXH_MAX_HANDS];
  mxh_raw_box_t a = box(-20, -10, 40, 60, 0.9f);
  CHECK(mxh_pick_hands(&a, 1, 40, 2, out) == 1);
  CHECK(same_box(&out[0], 0, 0, 40, 60));

  mxh_raw_box_t b = box(300, 200, 400, 300, 0.9f);
  CHECK(mxh_pick_hands(&b, 1, 40, 2, out) == 1);
  CHECK(same_box(&out[0], 300, 200, 320, 240));

  mxh_raw_box_t narrow = box(100, 100, 112, 200, 0.9f); /* 12 wide */
  CHECK(mxh_pick_hands(&narrow, 1, 40, 2, out) == 0);
  mxh_raw_box_t low = box(100, 100, 200, 115, 0.9f); /* 15 high */
  CHECK(mxh_pick_hands(&low, 1, 40, 2, out) == 0);
  mxh_raw_box_t just = box(100, 100, 116, 116, 0.9f); /* 16 x 16: kept */
  CHECK(mxh_pick_hands(&just, 1, 40, 2, out) == 1);
  mxh_raw_box_t edge = box(310, 0, 360, 50, 0.9f); /* clipped to 10 wide */
  CHECK(mxh_pick_hands(&edge, 1, 40, 2, out) == 0);
  mxh_raw_box_t outside = box(400, 300, 500, 400, 0.9f);
  CHECK(mxh_pick_hands(&outside, 1, 40, 2, out) == 0);
  mxh_raw_box_t left = box(-100, 10, -10, 100, 0.9f);
  CHECK(mxh_pick_hands(&left, 1, 40, 2, out) == 0);
  mxh_raw_box_t inverted = box(200, 200, 100, 100, 0.9f);
  CHECK(mxh_pick_hands(&inverted, 1, 40, 2, out) == 0);
  mxh_raw_box_t huge = box(-1000, -1000, 100000, 100000, 0.9f);
  CHECK(mxh_pick_hands(&huge, 1, 40, 2, out) == 1);
  CHECK(same_box(&out[0], 0, 0, 320, 240));

  /* max_hands is clamped to 1..2; never more than 2 written. */
  mxh_raw_box_t three[3] = {box(0, 0, 50, 50, 0.9f), box(100, 0, 150, 50, 0.8f), box(200, 0, 250, 50, 0.7f)};
  mxh_raw_box_t guard[MXH_MAX_HANDS + 1];
  memset(guard, 0x5A, sizeof(guard));
  CHECK(mxh_pick_hands(three, 3, 10, 0, guard) == 1);
  CHECK(same_box(&guard[0], 0, 0, 50, 50));
  memset(guard, 0x5A, sizeof(guard));
  CHECK(mxh_pick_hands(three, 3, 10, 5, guard) == 2);
  CHECK(same_box(&guard[0], 0, 0, 50, 50));
  CHECK(same_box(&guard[1], 100, 0, 150, 50));
  unsigned char sentinel[sizeof(mxh_raw_box_t)];
  memset(sentinel, 0x5A, sizeof(sentinel));
  CHECK(memcmp(&guard[2], sentinel, sizeof(sentinel)) == 0);
  CHECK(mxh_pick_hands(three, 3, 10, 2, guard) == 2);

  /* Area ties use the clipped box: a big box mostly off-frame loses. */
  mxh_raw_box_t clipped_tie[2] = {box(-200, -200, 30, 30, 0.8f), box(100, 100, 150, 150, 0.8f)};
  CHECK(mxh_pick_hands(clipped_tie, 2, 10, 2, out) == 2);
  CHECK(same_box(&out[0], 100, 100, 150, 150));
  CHECK(same_box(&out[1], 0, 0, 30, 30));
}

static void test_to_result_known(void) {
  mxp_result_t r;
  memset(&r, 0xAB, sizeof(r));
  mxh_raw_box_t h = box(100, 60, 180, 160, 0.8f);
  mxh_to_result(&h, "ok", 0.92f, 60, &r);
  CHECK(r.id == 7);
  CHECK(r.x == 140 && r.y == 110);
  CHECK(r.w == 80 && r.h == 100);
  CHECK(r.confidence == 92);
  CHECK(r.extra_len == 0);

  /* Odd size: centre is x0 + w/2 (floor). */
  mxh_raw_box_t odd = box(10, 20, 41, 53, 0.8f);
  mxh_to_result(&odd, "dislike", 0.7f, 60, &r);
  CHECK(r.id == 9);
  CHECK(r.w == 31 && r.h == 33);
  CHECK(r.x == 25 && r.y == 36);
  CHECK(r.confidence == 70);

  /* Threshold is inclusive. */
  mxh_to_result(&h, "like", 0.6f, 60, &r);
  CHECK(r.id == 6 && r.confidence == 60);
}

static void test_to_result_low_score(void) {
  mxp_result_t r;
  mxh_raw_box_t h = box(100, 60, 180, 160, 0.8f);
  mxh_to_result(&h, "ok", 0.55f, 60, &r);
  CHECK(r.id == 0);
  CHECK(r.confidence == 80); /* the hand score */
  CHECK(r.x == 140 && r.y == 110 && r.w == 80 && r.h == 100);
  CHECK(r.extra_len == 0);

  mxh_to_result(&h, "ok", 0.0f, 0, &r);
  CHECK(r.id == 7);
  CHECK(r.confidence == 0);

  mxh_to_result(&h, "ok", NAN, 60, &r);
  CHECK(r.id == 0 && r.confidence == 80);
}

static void test_to_result_no_gesture(void) {
  mxp_result_t r;
  mxh_raw_box_t h = box(100, 60, 180, 160, 0.8f);
  mxh_to_result(&h, "no_gesture", 0.99f, 60, &r);
  CHECK(r.id == 0 && r.confidence == 80);
  mxh_to_result(&h, "no_hand", 0.99f, 60, &r);
  CHECK(r.id == 0 && r.confidence == 80);
  mxh_to_result(&h, "fist", 0.99f, 60, &r);
  CHECK(r.id == 0 && r.confidence == 80);
  mxh_to_result(&h, NULL, 0.99f, 0, &r);
  CHECK(r.id == 0 && r.confidence == 80);
  CHECK(r.x == 140 && r.y == 110 && r.w == 80 && r.h == 100 && r.extra_len == 0);
}

static void test_label(void) {
  char buf[32];
  CHECK(mxh_label(7, 92, buf, sizeof(buf)) == 5 && strcmp(buf, "OK 92") == 0);
  CHECK(mxh_label(0, 81, buf, sizeof(buf)) == 7 && strcmp(buf, "HAND 81") == 0);
  CHECK(mxh_label(9, 100, buf, sizeof(buf)) == 11 && strcmp(buf, "DISLIKE 100") == 0);
  CHECK(mxh_label(12, 50, buf, sizeof(buf)) == 7 && strcmp(buf, "HAND 50") == 0);
  CHECK(mxh_label(1, 5, buf, sizeof(buf)) == 5 && strcmp(buf, "ONE 5") == 0);

  memset(buf, 'X', sizeof(buf));
  CHECK(mxh_label(7, 92, buf, 4) == 5);
  CHECK(memcmp(buf, "OK ", 3) == 0);
  CHECK(buf[3] == 0);
  int canary_ok = 1;
  for (size_t i = 4; i < sizeof(buf); i++)
    if (buf[i] != 'X') canary_ok = 0;
  CHECK(canary_ok);

  CHECK(mxh_label(7, 92, NULL, 0) == 5);

  CHECK(strcmp(mxh_ascii_name(0), "HAND") == 0);
  CHECK(strcmp(mxh_ascii_name(1), "ONE") == 0);
  CHECK(strcmp(mxh_ascii_name(2), "TWO") == 0);
  CHECK(strcmp(mxh_ascii_name(3), "THREE") == 0);
  CHECK(strcmp(mxh_ascii_name(4), "FOUR") == 0);
  CHECK(strcmp(mxh_ascii_name(5), "FIVE") == 0);
  CHECK(strcmp(mxh_ascii_name(6), "LIKE") == 0);
  CHECK(strcmp(mxh_ascii_name(7), "OK") == 0);
  CHECK(strcmp(mxh_ascii_name(8), "CALL") == 0);
  CHECK(strcmp(mxh_ascii_name(9), "DISLIKE") == 0);
  CHECK(strcmp(mxh_ascii_name(10), "HAND") == 0);
  CHECK(strcmp(mxh_ascii_name(0xFFFF), "HAND") == 0);
}

static void test_expand_box(void) {
  mxh_raw_box_t e;
  /* 100x80 -> 125x100 around the same centre (odd growth: extra pixel right/bottom). */
  CHECK(mxh_expand_box(&(mxh_raw_box_t){100, 80, 200, 160, 0.8f}, &e));
  CHECK(same_box(&e, 88, 70, 213, 170) && e.score == 0.8f);
  /* 40x40 -> 50x50, clipped at the top-left corner. */
  CHECK(mxh_expand_box(&(mxh_raw_box_t){0, 0, 40, 40, 0.5f}, &e));
  CHECK(same_box(&e, 0, 0, 45, 45));
  /* Clipped at the bottom-right corner. */
  CHECK(mxh_expand_box(&(mxh_raw_box_t){300, 220, 320, 240, 0.5f}, &e));
  CHECK(same_box(&e, 298, 218, 320, 240));
  /* An input already partly outside is clipped after growing. */
  CHECK(mxh_expand_box(&(mxh_raw_box_t){-20, 10, 40, 70, 0.5f}, &e));
  CHECK(same_box(&e, 0, 3, 48, 78));
  /* Whole frame stays the whole frame. */
  CHECK(mxh_expand_box(&(mxh_raw_box_t){0, 0, 320, 240, 0.5f}, &e));
  CHECK(same_box(&e, 0, 0, 320, 240));
  /* Too small after clipping (10x10 -> 13x13 < MXH_MIN_HAND_PX): no crop. */
  CHECK(!mxh_expand_box(&(mxh_raw_box_t){100, 100, 110, 110, 0.5f}, &e));
  /* 13x13 -> 16x16 (rounded): just enough. */
  CHECK(mxh_expand_box(&(mxh_raw_box_t){100, 100, 113, 113, 0.5f}, &e));
  CHECK(e.x1 - e.x0 == 16 && e.y1 - e.y0 == 16);
  /* Empty, inverted or off-frame boxes. */
  CHECK(!mxh_expand_box(&(mxh_raw_box_t){50, 50, 50, 90, 0.5f}, &e));
  CHECK(!mxh_expand_box(&(mxh_raw_box_t){90, 50, 50, 90, 0.5f}, &e));
  CHECK(!mxh_expand_box(&(mxh_raw_box_t){-100, -100, -60, -60, 0.5f}, &e));
  CHECK(!mxh_expand_box(&(mxh_raw_box_t){400, 10, 450, 60, 0.5f}, &e));
  CHECK(!mxh_expand_box(NULL, &e));
  CHECK(!mxh_expand_box(&(mxh_raw_box_t){100, 80, 200, 160, 0.8f}, NULL));
}

static void test_hand_lost(void) {
  CHECK(mxh_hand_lost(NULL));
  CHECK(mxh_hand_lost("no_hand"));
  CHECK(!mxh_hand_lost("no_gesture")); /* still a hand */
  CHECK(!mxh_hand_lost("one"));
  CHECK(!mxh_hand_lost("dislike"));
  CHECK(!mxh_hand_lost("palm"));
}

static void test_track_schedule(void) {
  mxh_track_t t;
  mxh_raw_box_t hands[MXH_MAX_HANDS] = {box(10, 20, 110, 140, 0.82f), box(200, 30, 280, 130, 0.7f)};
  mxh_track_reset(&t);
  CHECK(t.n == 0);
  CHECK(mxh_track_need_detect(&t, 40, 60, 1, 3)); /* nothing tracked yet */
  CHECK(mxh_track_need_detect(NULL, 40, 60, 1, 3));

  mxh_track_detected(&t, hands, 1, false, 40, 60, 1);
  CHECK(t.n == 1 && t.age == 0 && same_box(&t.hands[0], 10, 20, 110, 140) && t.hands[0].score == 0.82f);
  /* Every 3 frames: D R R D R R ... */
  CHECK(!mxh_track_need_detect(&t, 40, 60, 1, 3));
  mxh_track_reused(&t);
  CHECK(t.age == 1);
  CHECK(!mxh_track_need_detect(&t, 40, 60, 1, 3));
  mxh_track_reused(&t);
  CHECK(mxh_track_need_detect(&t, 40, 60, 1, 3));
  CHECK(!mxh_track_need_detect(&t, 40, 60, 1, 4)); /* N = 4 allows one more reuse */
  mxh_track_detected(&t, hands, 1, false, 40, 60, 1);
  CHECK(!mxh_track_need_detect(&t, 40, 60, 1, 3));

  /* N <= 1: detect on every frame (no reuse). */
  CHECK(mxh_track_need_detect(&t, 40, 60, 1, 1));
  CHECK(mxh_track_need_detect(&t, 40, 60, 1, 0));

  /* Age saturates instead of wrapping back to "fresh". */
  for (int i = 0; i < 300; i++) mxh_track_reused(&t);
  CHECK(t.age == 255);
  CHECK(mxh_track_need_detect(&t, 40, 60, 1, 255));
}

static void test_track_config_and_hands(void) {
  mxh_track_t t;
  mxh_raw_box_t hands[MXH_MAX_HANDS] = {box(10, 20, 110, 140, 0.82f), box(200, 30, 280, 130, 0.7f)};
  mxh_track_reset(&t);
  mxh_track_detected(&t, hands, 1, false, 40, 60, 1);
  /* Any config change: detect. */
  CHECK(mxh_track_need_detect(&t, 41, 60, 1, 3));
  CHECK(mxh_track_need_detect(&t, 40, 59, 1, 3));
  CHECK(mxh_track_need_detect(&t, 40, 60, 2, 3));
  /* max_hands 2 with one hand tracked: keep looking for the second one. */
  mxh_track_detected(&t, hands, 1, false, 40, 60, 2);
  CHECK(mxh_track_need_detect(&t, 40, 60, 2, 3));
  mxh_track_detected(&t, hands, 2, false, 40, 60, 2);
  CHECK(t.n == 2 && same_box(&t.hands[1], 200, 30, 280, 130));
  CHECK(!mxh_track_need_detect(&t, 40, 60, 2, 3));
  /* max_hands is compared clamped (0 and 1 are the same, 2 and 9 too). */
  mxh_track_detected(&t, hands, 2, false, 40, 60, 9);
  CHECK(!mxh_track_need_detect(&t, 40, 60, 2, 3));
  mxh_track_detected(&t, hands, 1, false, 40, 60, 0);
  CHECK(!mxh_track_need_detect(&t, 40, 60, 1, 3));
  /* More hands than MXH_MAX_HANDS: only the first ones are kept. */
  mxh_track_detected(&t, hands, 7, false, 40, 60, 2);
  CHECK(t.n == MXH_MAX_HANDS);

  /* A crop the classifier calls no_hand on the detection frame: nothing to reuse. */
  mxh_track_detected(&t, hands, 1, true, 40, 60, 1);
  CHECK(t.n == 0);
  CHECK(mxh_track_need_detect(&t, 40, 60, 1, 3));
  /* No hands found: detect on the next frame too. */
  mxh_track_detected(&t, hands, 0, false, 40, 60, 1);
  CHECK(t.n == 0);
  CHECK(mxh_track_need_detect(&t, 40, 60, 1, 3));
  mxh_track_detected(&t, NULL, 1, false, 40, 60, 1);
  CHECK(t.n == 0);

  /* Reset (function switch, heap-guard skip, stale frame): detect. */
  mxh_track_detected(&t, hands, 1, false, 40, 60, 1);
  mxh_track_reset(&t);
  CHECK(t.n == 0 && mxh_track_need_detect(&t, 40, 60, 1, 3));

  /* NULL tracker: the calls are no-ops. */
  mxh_track_reset(NULL);
  mxh_track_reused(NULL);
  mxh_track_detected(NULL, hands, 1, false, 40, 60, 1);
}

int main(void) {
  test_expand_box();
  test_hand_lost();
  test_track_schedule();
  test_track_config_and_hands();
  test_gesture_ids();
  test_confidence();
  test_pick_hands_threshold_sort();
  test_pick_hands_clip_and_min_size();
  test_to_result_known();
  test_to_result_low_score();
  test_to_result_no_gesture();
  test_label();
  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("all gesture post tests passed\n");
  return 0;
}
