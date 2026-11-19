#include "mxcam_face_post.h"

#include <string.h>

uint8_t mxf_confidence(float score) {
  if (!(score > 0.0f)) return 0; /* also NaN */
  float c = score * 100.0f + 0.5f;
  if (c >= 100.0f) return 100;
  return (uint8_t)c;
}

/* v * dst / src rounded to nearest (halves away from zero). */
static int64_t scale(int32_t v, int32_t dst, int32_t src) {
  int64_t num = (int64_t)v * dst;
  if (num >= 0) return (num + src / 2) / src;
  return -((-num + src / 2) / src);
}

static int64_t clip(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool mxf_to_result(const mxf_raw_box_t *b, uint16_t src_w, uint16_t src_h, mxp_result_t *out) {
  if (src_w == 0 || src_h == 0) return false;
  int64_t x0 = clip(scale(b->x0, MXF_FRAME_W, src_w), 0, MXF_FRAME_W);
  int64_t x1 = clip(scale(b->x1, MXF_FRAME_W, src_w), 0, MXF_FRAME_W);
  int64_t y0 = clip(scale(b->y0, MXF_FRAME_H, src_h), 0, MXF_FRAME_H);
  int64_t y1 = clip(scale(b->y1, MXF_FRAME_H, src_h), 0, MXF_FRAME_H);
  if (x1 <= x0 || y1 <= y0) return false;
  uint16_t w = (uint16_t)(x1 - x0), h = (uint16_t)(y1 - y0);
  memset(out, 0, sizeof(*out));
  out->id = 0;
  out->w = w;
  out->h = h;
  out->x = (int16_t)(x0 + w / 2);
  out->y = (int16_t)(y0 + h / 2);
  out->confidence = mxf_confidence(b->score);
  out->extra_len = 0;
  return true;
}

/* true when a ranks strictly before b. */
static bool better(const mxp_result_t *a, const mxp_result_t *b) {
  if (a->confidence != b->confidence) return a->confidence > b->confidence;
  return (uint32_t)a->w * a->h > (uint32_t)b->w * b->h;
}

uint8_t mxf_select(const mxf_raw_box_t *boxes, size_t n, uint16_t src_w, uint16_t src_h, uint8_t threshold,
                   uint8_t max_results, mxp_result_t *out, uint8_t cap) {
  uint8_t limit = max_results;
  if (cap < limit) limit = cap;
  if (limit > MXP_MAX_RESULTS) limit = MXP_MAX_RESULTS;
  if (limit == 0) return 0;

  /* Top-k insertion: out[0..count) stays sorted; equal keys keep detector
   * order (a new record goes after everything it does not beat). */
  uint8_t count = 0;
  for (size_t i = 0; i < n; i++) {
    mxp_result_t r;
    if (!mxf_to_result(&boxes[i], src_w, src_h, &r)) continue;
    if (r.confidence < threshold) continue;
    uint8_t pos = count;
    while (pos > 0 && better(&r, &out[pos - 1])) pos--;
    if (pos >= limit) continue;
    uint8_t last = count < limit ? count : (uint8_t)(limit - 1);
    for (uint8_t j = last; j > pos; j--) out[j] = out[j - 1];
    out[pos] = r;
    if (count < limit) count++;
  }
  return count;
}
