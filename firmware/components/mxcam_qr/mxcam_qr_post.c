#include "mxcam_qr_post.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void mxq_set_payload(mxq_code_t *c, const uint8_t *p, size_t n) {
  size_t keep = n < MXP_RESULT_DATA_MAX ? n : MXP_RESULT_DATA_MAX;
  c->total_len = (uint16_t)(n > 65535u ? 65535u : n);
  c->len = (uint8_t)keep;
  if (keep > 0 && p) memcpy(c->bytes, p, keep);
  else c->len = 0;
}

int32_t mxq_area2(const mxq_code_t *c) {
  int64_t s = 0;
  for (int k = 0; k < 4; k++) {
    int j = (k + 1) & 3;
    s += (int64_t)c->x[k] * c->y[j] - (int64_t)c->x[j] * c->y[k];
  }
  if (s < 0) s = -s;
  return s > INT32_MAX ? INT32_MAX : (int32_t)s;
}

uint8_t mxq_sort_limit(mxq_code_t *codes, uint8_t n, uint8_t max) {
  /* Insertion sort: stable, n <= quirc's 8 grids. */
  for (uint8_t i = 1; i < n; i++) {
    mxq_code_t t = codes[i];
    int32_t a = mxq_area2(&t);
    int j = i;
    while (j > 0 && mxq_area2(&codes[j - 1]) < a) {
      codes[j] = codes[j - 1];
      j--;
    }
    codes[j] = t;
  }
  return n < max ? n : max;
}

/* Clipped bounding box of the corners: [x0, x1] x [y0, y1] inclusive. */
static void bbox(const mxq_code_t *c, int *x0, int *y0, int *x1, int *y1) {
  int minx = c->x[0], maxx = minx, miny = c->y[0], maxy = miny;
  for (int k = 1; k < 4; k++) {
    if (c->x[k] < minx) minx = c->x[k];
    if (c->x[k] > maxx) maxx = c->x[k];
    if (c->y[k] < miny) miny = c->y[k];
    if (c->y[k] > maxy) maxy = c->y[k];
  }
  *x0 = clampi(minx, 0, MXQ_FRAME_W - 1);
  *x1 = clampi(maxx, 0, MXQ_FRAME_W - 1);
  *y0 = clampi(miny, 0, MXQ_FRAME_H - 1);
  *y1 = clampi(maxy, 0, MXQ_FRAME_H - 1);
}

static void centre(const mxq_code_t *c, int *cx, int *cy) {
  int x0, y0, x1, y1;
  bbox(c, &x0, &y0, &x1, &y1);
  *cx = (x0 + x1) / 2; /* non-negative after clipping: rounds down */
  *cy = (y0 + y1) / 2;
}

void mxq_to_result(const mxq_code_t *c, uint16_t id, mxp_result_t *out) {
  int x0, y0, x1, y1;
  bbox(c, &x0, &y0, &x1, &y1);
  memset(out, 0, sizeof(*out));
  out->id = id;
  out->x = (int16_t)((x0 + x1) / 2);
  out->y = (int16_t)((y0 + y1) / 2);
  out->w = (uint16_t)(x1 - x0 + 1);
  out->h = (uint16_t)(y1 - y0 + 1);
  out->confidence = 255;

  mxp_corners_extra_t e;
  double deg = atan2((double)c->y[1] - c->y[0], (double)c->x[1] - c->x[0]) * (180.0 / 3.14159265358979323846);
  long cdeg = lround(deg * 100.0);
  e.rotation_cdeg = (int16_t)(cdeg > 18000 ? 18000 : (cdeg < -18000 ? -18000 : cdeg));
  for (int k = 0; k < 4; k++) {
    e.x[k] = c->x[k];
    e.y[k] = c->y[k];
  }
  mxp_pack_corners_extra(&e, out);
}

int mxq_pick_for_learn(const mxq_code_t *codes, uint8_t n, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  int best = -1;
  int32_t best_area = -1;
  for (uint8_t i = 0; i < n; i++) {
    int cx, cy;
    centre(&codes[i], &cx, &cy);
    if ((uint32_t)cx < x || (uint32_t)cx >= (uint32_t)x + w) continue;
    if ((uint32_t)cy < y || (uint32_t)cy >= (uint32_t)y + h) continue;
    int32_t a = mxq_area2(&codes[i]);
    if (a > best_area) {
      best = i;
      best_area = a;
    }
  }
  return best;
}

int mxq_label(uint16_t id, const mxq_code_t *c, char *buf, size_t cap) {
  char text[MXQ_LABEL_CHARS + 1];
  size_t n = c->len < MXQ_LABEL_CHARS ? c->len : MXQ_LABEL_CHARS;
  for (size_t i = 0; i < n; i++) {
    uint8_t b = c->bytes[i];
    text[i] = (b >= 0x20 && b <= 0x7E) ? (char)b : '?';
  }
  text[n] = 0;
  char idtxt[8] = "";
  if (id != 0) snprintf(idtxt, sizeof(idtxt), "%u", (unsigned)id);
  return snprintf(buf, cap, "QR%s%s%s", idtxt, n ? " " : "", text);
}
