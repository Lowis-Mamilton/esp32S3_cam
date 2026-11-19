#include "mxcam_color.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "sdkconfig.h"
#if !CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#error "mxcam_color needs CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y (work buffer lives in PSRAM BSS)"
#endif
#else
#define EXT_RAM_BSS_ATTR
#endif

/* ---- Work buffer (static, no malloc) ----------------------------------
 * Subsampled grid is 160x120 = 19200 cells; with 4-connectivity a two-pass
 * labelling needs at most 9600 provisional labels (+1 for "none").
 *   cls    u8  [19200]  19.2 KB  learned index+1 per cell (0 = none)
 *   label  u16 [19200]  38.4 KB
 *   parent u16 [9601]   19.2 KB  union-find
 *   area   u16 [9601]   19.2 KB  (cells, <= 19200)
 *   bbox   u8 x4[9601]  38.4 KB
 *   lcls   u8  [9601]    9.6 KB  class per root
 * Total ~ 144 KB, so it goes to PSRAM on target (EXT_RAM_BSS_ATTR; needs
 * CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y) and is plain static on host.
 * Consequence: mxc_detect is not reentrant, and must not run while the flash
 * cache is disabled (PSRAM BSS is unreachable then).
 * Label bound: 4-connectivity bounds labels to GN/2 only for ONE class; with
 * several classes up to GN provisional labels could be requested. Rather than
 * size every array for GN+1 (+~110 KB PSRAM), label allocation is guarded:
 * once MAXLBL is exhausted further new components are left unlabelled (they
 * are dropped). Only pathological noise (thousands of isolated 1-cell blobs)
 * can hit this, and such blobs are below any sensible min_area. */
#define GW (MXC_FRAME_W / 2)
#define GH (MXC_FRAME_H / 2)
#define GN (GW * GH)
#define MAXLBL (GN / 2 + 1)

#define CANARY 0xA5A5A5A5u
typedef struct {
  uint8_t cls[GN];
  uint32_t c0;
  uint16_t label[GN];
  uint32_t c1;
  uint16_t parent[MAXLBL];
  uint32_t c2;
  uint16_t area[MAXLBL];
  uint32_t c3;
  uint8_t bx0[MAXLBL], by0[MAXLBL], bx1[MAXLBL], by1[MAXLBL];
  uint32_t c4;
  uint8_t lcls[MAXLBL];
  uint32_t c5;
} work_t;
/* Canaries sit after each array so tests can detect overruns without ASan. */
static EXT_RAM_BSS_ATTR work_t s_w;
#define s_cls s_w.cls
#define s_label s_w.label
#define s_parent s_w.parent
#define s_area s_w.area
#define s_bx0 s_w.bx0
#define s_by0 s_w.by0
#define s_bx1 s_w.bx1
#define s_by1 s_w.by1
#define s_lcls s_w.lcls

#ifdef MXC_TEST_HOOKS
void mxc_test_canary_arm(void) {
  s_w.c0 = s_w.c1 = s_w.c2 = s_w.c3 = s_w.c4 = s_w.c5 = CANARY;
}
bool mxc_test_canary_ok(void) {
  return s_w.c0 == CANARY && s_w.c1 == CANARY && s_w.c2 == CANARY && s_w.c3 == CANARY &&
         s_w.c4 == CANARY && s_w.c5 == CANARY;
}
#endif

typedef struct {
  float h, s, v;
} hsv_t;

static hsv_t rgb_to_hsv(uint8_t r8, uint8_t g8, uint8_t b8) {
  float r = r8 / 255.0f, g = g8 / 255.0f, b = b8 / 255.0f;
  float max = r > g ? (r > b ? r : b) : (g > b ? g : b);
  float min = r < g ? (r < b ? r : b) : (g < b ? g : b);
  float d = max - min;
  float h = 0.0f;
  if (d > 0.0f) {
    if (max == r) {
      h = (g - b) / d;
      if (h < 0.0f) h += 6.0f;
    } else if (max == g) {
      h = (b - r) / d + 2.0f;
    } else {
      h = (r - g) / d + 4.0f;
    }
    h *= 60.0f;
  }
  hsv_t o = {h, max == 0.0f ? 0.0f : d / max, max};
  return o;
}

static float hue_diff(float a, float b) {
  float d = a > b ? a - b : b - a;
  return d < 360.0f - d ? d : 360.0f - d;
}

static float absf(float x) { return x < 0.0f ? -x : x; }

#define MIN_SAT 0.15f

static bool hsv_match(hsv_t a, hsv_t b, uint8_t tol) {
  if (a.s < MIN_SAT || b.s < MIN_SAT) return false;
  float sv = 0.25f + (float)tol / 200.0f;
  return hue_diff(a.h, b.h) <= (float)tol * 0.6f && absf(a.s - b.s) <= sv && absf(a.v - b.v) <= sv;
}

/* Big-endian RGB565 in a uint16 on a little-endian CPU -> RGB888. */
static void unpack(uint16_t p, uint8_t *r, uint8_t *g, uint8_t *b) {
  uint16_t v = (uint16_t)((p >> 8) | (p << 8));
  uint8_t r5 = (uint8_t)(v >> 11), g6 = (uint8_t)((v >> 5) & 63), b5 = (uint8_t)(v & 31);
  *r = (uint8_t)((r5 << 3) | (r5 >> 2));
  *g = (uint8_t)((g6 << 2) | (g6 >> 4));
  *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static uint16_t find(uint16_t x) {
  while (s_parent[x] != x) {
    s_parent[x] = s_parent[s_parent[x]];
    x = s_parent[x];
  }
  return x;
}

static void unite(uint16_t a, uint16_t b) {
  a = find(a);
  b = find(b);
  if (a == b) return;
  if (a < b) s_parent[b] = a; else s_parent[a] = b;
}

bool mxc_same_color(const uint8_t a[3], const uint8_t b[3], uint8_t tolerance) {
  return hsv_match(rgb_to_hsv(a[0], a[1], a[2]), rgb_to_hsv(b[0], b[1], b[2]), tolerance);
}

bool mxc_sample(const uint16_t *frame, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                uint8_t rgb_out[3]) {
  uint32_t x1 = (uint32_t)x + w, y1 = (uint32_t)y + h;
  if (x1 > MXC_FRAME_W) x1 = MXC_FRAME_W;
  if (y1 > MXC_FRAME_H) y1 = MXC_FRAME_H;
  if (x >= x1 || y >= y1) return false;
  uint32_t total = (x1 - x) * (y1 - y), n = 0, sr = 0, sg = 0, sb = 0;
  for (uint32_t j = y; j < y1; j++) {
    for (uint32_t i = x; i < x1; i++) {
      uint8_t r, g, b;
      unpack(frame[j * MXC_FRAME_W + i], &r, &g, &b);
      hsv_t c = rgb_to_hsv(r, g, b);
      if (c.s < MIN_SAT || c.v < MIN_SAT) continue;
      n++;
      sr += r;
      sg += g;
      sb += b;
    }
  }
  if (n == 0 || n * 4 < total) return false;
  rgb_out[0] = (uint8_t)((sr + n / 2) / n);
  rgb_out[1] = (uint8_t)((sg + n / 2) / n);
  rgb_out[2] = (uint8_t)((sb + n / 2) / n);
  return true;
}

uint8_t mxc_detect(const uint16_t *frame, const mxc_learned_t *learned, uint8_t n_learned,
                   const mxc_params_t *p, mxp_result_t *out, uint8_t cap) {
  uint8_t limit = p->max_results < cap ? p->max_results : cap;
  if (limit > MXP_MAX_RESULTS) limit = MXP_MAX_RESULTS;
  if (limit == 0 || n_learned == 0) return 0;

  hsv_t lh[255];
  for (uint8_t k = 0; k < n_learned; k++)
    lh[k] = rgb_to_hsv(learned[k].rgb[0], learned[k].rgb[1], learned[k].rgb[2]);

  /* Pass 0: classify each subsampled cell (top-left pixel of each 2x2 block). */
  for (int gy = 0; gy < GH; gy++) {
    for (int gx = 0; gx < GW; gx++) {
      uint8_t r, g, b;
      unpack(frame[(gy * 2) * MXC_FRAME_W + gx * 2], &r, &g, &b);
      hsv_t c = rgb_to_hsv(r, g, b);
      uint8_t best = 0;
      if (c.s >= MIN_SAT) {
        float bd = 1e9f;
        for (uint8_t k = 0; k < n_learned; k++) {
          if (!hsv_match(c, lh[k], p->tolerance)) continue;
          float d = hue_diff(c.h, lh[k].h);
          if (d < bd) {
            bd = d;
            best = (uint8_t)(k + 1);
          }
        }
      }
      s_cls[gy * GW + gx] = best;
    }
  }

  /* Pass 1: provisional labels + union of equal-class 4-neighbours. */
  uint16_t next = 1;
  s_parent[0] = 0;
  for (int gy = 0; gy < GH; gy++) {
    for (int gx = 0; gx < GW; gx++) {
      int i = gy * GW + gx;
      uint8_t c = s_cls[i];
      if (!c) {
        s_label[i] = 0;
        continue;
      }
      uint16_t left = (gx > 0 && s_cls[i - 1] == c) ? s_label[i - 1] : 0;
      uint16_t up = (gy > 0 && s_cls[i - GW] == c) ? s_label[i - GW] : 0;
      if (!left && !up) {
        if (next >= MAXLBL) {
          s_label[i] = 0; /* label budget exhausted: drop this cell */
          continue;
        }
        s_parent[next] = next;
        s_label[i] = next++;
      } else if (left && up) {
        unite(left, up);
        s_label[i] = left;
      } else {
        s_label[i] = left ? left : up;
      }
    }
  }

  /* Pass 2: accumulate area, bbox and class per root. */
  for (uint16_t l = 1; l < next; l++) {
    s_area[l] = 0;
    s_bx0[l] = s_by0[l] = 255;
    s_bx1[l] = s_by1[l] = 0;
  }
  for (int gy = 0; gy < GH; gy++) {
    for (int gx = 0; gx < GW; gx++) {
      uint16_t l = s_label[gy * GW + gx];
      if (!l) continue;
      l = find(l);
      s_lcls[l] = s_cls[gy * GW + gx];
      s_area[l]++;
      if (gx < s_bx0[l]) s_bx0[l] = (uint8_t)gx;
      if (gx > s_bx1[l]) s_bx1[l] = (uint8_t)gx;
      if (gy < s_by0[l]) s_by0[l] = (uint8_t)gy;
      if (gy > s_by1[l]) s_by1[l] = (uint8_t)gy;
    }
  }

  /* Keep the top `limit` components by area (insertion into a small list). */
  uint32_t areas[MXP_MAX_RESULTS];
  uint8_t count = 0;
  for (uint16_t l = 1; l < next; l++) {
    if (s_parent[l] != l) continue;
    uint32_t area = (uint32_t)s_area[l] * 4u;
    if (area < p->min_area) continue;
    if (count == limit && area <= areas[count - 1]) continue;
    uint8_t pos = count < limit ? count : (uint8_t)(limit - 1);
    while (pos > 0 && areas[pos - 1] < area) {
      areas[pos] = areas[pos - 1];
      out[pos] = out[pos - 1];
      pos--;
    }
    int x0 = s_bx0[l] * 2, y0 = s_by0[l] * 2;
    int x1 = (s_bx1[l] + 1) * 2, y1 = (s_by1[l] + 1) * 2;
    if (x1 > MXC_FRAME_W) x1 = MXC_FRAME_W;
    if (y1 > MXC_FRAME_H) y1 = MXC_FRAME_H;
    mxp_result_t r;
    memset(&r, 0, sizeof r);
    r.id = learned[s_lcls[l] - 1].id;
    r.x = (int16_t)((x0 + x1) / 2);
    r.y = (int16_t)((y0 + y1) / 2);
    r.w = (uint16_t)(x1 - x0);
    r.h = (uint16_t)(y1 - y0);
    r.confidence = 255;
    r.extra_len = 0;
    out[pos] = r;
    areas[pos] = area;
    if (count < limit) count++;
  }
  return count;
}
