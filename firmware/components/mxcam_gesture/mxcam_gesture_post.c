#include "mxcam_gesture_post.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Index = gesture id (MXP_GESTURE_*). Class names as in esp-dl's
 * hand_gesture_category_name.hpp; matched by string, not by model index. */
static const char *const k_cat[MXP_GESTURE_MAX + 1] = {NULL,   "one", "two", "three", "four",
                                                       "five", "like", "ok", "call",  "dislike"};
static const char *const k_ascii[MXP_GESTURE_MAX + 1] = {"HAND", "ONE",  "TWO", "THREE", "FOUR",
                                                         "FIVE", "LIKE", "OK",  "CALL",  "DISLIKE"};

uint8_t mxh_gesture_id(const char *cat_name) {
  if (!cat_name) return MXP_GESTURE_UNKNOWN;
  for (uint8_t id = 1; id <= MXP_GESTURE_MAX; id++)
    if (strcmp(cat_name, k_cat[id]) == 0) return id;
  return MXP_GESTURE_UNKNOWN;
}

uint8_t mxh_confidence(float score) {
  if (!(score > 0.0f)) return 0; /* also NaN */
  float c = score * 100.0f + 0.5f;
  if (c >= 100.0f) return 100;
  return (uint8_t)c;
}

static int32_t clip(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

static mxh_raw_box_t clip_box(const mxh_raw_box_t *b) {
  mxh_raw_box_t c = *b;
  c.x0 = clip(b->x0, 0, MXH_FRAME_W);
  c.x1 = clip(b->x1, 0, MXH_FRAME_W);
  c.y0 = clip(b->y0, 0, MXH_FRAME_H);
  c.y1 = clip(b->y1, 0, MXH_FRAME_H);
  return c;
}

/* Clipped box with its ranking keys. */
typedef struct {
  mxh_raw_box_t b;
  uint8_t conf;
  uint32_t area;
} cand_t;

/* true when a ranks strictly before b (equal keys keep input order). */
static bool better(const cand_t *a, const cand_t *b) {
  if (a->conf != b->conf) return a->conf > b->conf;
  return a->area > b->area;
}

uint8_t mxh_pick_hands(const mxh_raw_box_t *in, size_t n, uint8_t hand_threshold, uint8_t max_hands,
                       mxh_raw_box_t out[MXH_MAX_HANDS]) {
  uint8_t limit = max_hands < 1 ? 1 : (max_hands > MXH_MAX_HANDS ? MXH_MAX_HANDS : max_hands);
  if (!in || !out) return 0;
  /* Top-k insertion: best[0..count) stays sorted. */
  cand_t best[MXH_MAX_HANDS];
  uint8_t count = 0;
  for (size_t i = 0; i < n; i++) {
    cand_t c;
    c.b = clip_box(&in[i]);
    if (c.b.x1 - c.b.x0 < MXH_MIN_HAND_PX || c.b.y1 - c.b.y0 < MXH_MIN_HAND_PX) continue;
    c.conf = mxh_confidence(in[i].score);
    if (c.conf < hand_threshold) continue;
    c.area = (uint32_t)(c.b.x1 - c.b.x0) * (uint32_t)(c.b.y1 - c.b.y0);
    uint8_t pos = count;
    while (pos > 0 && better(&c, &best[pos - 1])) pos--;
    if (pos >= limit) continue;
    uint8_t last = count < limit ? count : (uint8_t)(limit - 1);
    for (uint8_t j = last; j > pos; j--) best[j] = best[j - 1];
    best[pos] = c;
    if (count < limit) count++;
  }
  for (uint8_t i = 0; i < count; i++) out[i] = best[i].b;
  return count;
}

void mxh_to_result(const mxh_raw_box_t *hand, const char *cat_name, float gesture_score, uint8_t gesture_threshold,
                   mxp_result_t *out) {
  mxh_raw_box_t b = clip_box(hand);
  uint16_t w = b.x1 > b.x0 ? (uint16_t)(b.x1 - b.x0) : 0;
  uint16_t h = b.y1 > b.y0 ? (uint16_t)(b.y1 - b.y0) : 0;
  uint8_t id = mxh_gesture_id(cat_name);
  uint8_t gconf = mxh_confidence(gesture_score);
  if (id != MXP_GESTURE_UNKNOWN && gconf < gesture_threshold) id = MXP_GESTURE_UNKNOWN;
  memset(out, 0, sizeof(*out));
  out->id = id;
  out->x = (int16_t)(b.x0 + w / 2);
  out->y = (int16_t)(b.y0 + h / 2);
  out->w = w;
  out->h = h;
  out->confidence = id != MXP_GESTURE_UNKNOWN ? gconf : mxh_confidence(hand->score);
  out->extra_len = 0;
}

const char *mxh_ascii_name(uint16_t id) { return id <= MXP_GESTURE_MAX ? k_ascii[id] : k_ascii[0]; }

int mxh_label(uint16_t id, uint8_t confidence, char *buf, size_t cap) {
  return snprintf(buf, cap, "%s %u", mxh_ascii_name(id), (unsigned)confidence);
}
