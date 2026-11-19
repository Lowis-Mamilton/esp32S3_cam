#include "mxcam_track.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mxcam_gray.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "sdkconfig.h"
#if !CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#error "mxcam_track needs CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y (work buffers live in PSRAM BSS)"
#endif
#else
#define EXT_RAM_BSS_ATTR
#endif

#define FW (2 * MXG_HALF_W)
#define FH (2 * MXG_HALF_H)
#define HW MXG_HALF_W
#define HH MXG_HALF_H
#define II_N ((HW + 1) * (HH + 1))
#define WMAX 128 /* reduced search window side cap (search windows are ~3x the template) */
#define TN (MXK_TMPL_MAX * MXK_TMPL_MAX)
#define SCALE_MIN 0.5f
#define SCALE_MAX 2.0f
#define UPDATE_MIN 70 /* template update needs a score >= max(threshold, 70) */
#define MIN_WIN_STD 2 /* a flatter window scores 0 */

/* ---- Work buffers (static, no malloc) ----------------------------------
 *   gray  u8  [160*120]    19.2 KB  mxg_half output
 *   gii   u32 [161*121]    77.9 KB  integral of gray (area-average resampling)
 *   win   u8  [128*128]    16.4 KB  search window resampled to template scale
 *   ii    u32 [161*121]    77.9 KB  integral of win (stride wr+1), or of the
 *                                   back-projection (stride 161) when lost
 *   ii2   u32 [129*129]    66.6 KB  integral of win^2; during resampling,
 *                                   the integral at the cell corners (x256)
 *   patch u8  [32*32]       1 KB    best match of the best scale
 *   bx/by, fx/fy, rx       ~1.3 KB  resampling cell edges (index + 1/256 fraction)
 * ~260 KB in PSRAM on target (EXT_RAM_BSS_ATTR); not reentrant. Canaries sit
 * after each array so tests can detect overruns without ASan. */
#define CANARY 0xA5A5A5A5u
typedef struct {
  uint8_t gray[HW * HH];
  uint32_t c0;
  uint32_t gii[II_N];
  uint32_t c1;
  uint8_t win[WMAX * WMAX];
  uint32_t c2;
  uint32_t ii[II_N];
  uint32_t c3;
  uint32_t ii2[(WMAX + 1) * (WMAX + 1)];
  uint32_t c4;
  uint8_t patch[TN];
  uint32_t c5;
  uint8_t bx[WMAX + 1], by[WMAX + 1];
  uint16_t fx[WMAX + 1], fy[WMAX + 1];
  float rx[WMAX]; /* 256 / cell width (1/256 units) */
  uint32_t c6;
} work_t;
static EXT_RAM_BSS_ATTR work_t s_w;

#ifdef MXK_TEST_HOOKS
static uint32_t s_macs;
#define COUNT_MACS(n) (s_macs += (uint32_t)(n))
void mxk_test_canary_arm(void) { s_w.c0 = s_w.c1 = s_w.c2 = s_w.c3 = s_w.c4 = s_w.c5 = s_w.c6 = CANARY; }
bool mxk_test_canary_ok(void) {
  return s_w.c0 == CANARY && s_w.c1 == CANARY && s_w.c2 == CANARY && s_w.c3 == CANARY && s_w.c4 == CANARY &&
         s_w.c5 == CANARY && s_w.c6 == CANARY;
}
uint32_t mxk_test_macs(void) { return s_macs; }
#else
#define COUNT_MACS(n) ((void)0)
#endif

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- Colour histogram (ADR-0009 HSV: v = max, s = (max-min)/max, hue as in
 * mxcam_color's rgb_to_hsv), integer only -------------------------------- */

/* Big-endian RGB565 in a uint16 on a little-endian CPU -> RGB888. */
static void unpack(uint16_t p, int *r, int *g, int *b) {
  uint16_t v = (uint16_t)((p >> 8) | (p << 8));
  int r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
  *r = (r5 << 3) | (r5 >> 2);
  *g = (g6 << 2) | (g6 >> 4);
  *b = (b5 << 3) | (b5 >> 2);
}

/* bins 0..23: hue (12 x 30 deg) * 2 + (S >= 0.5); bins 24..31: V / 32 when S < 0.2 */
static int color_bin(uint16_t p) {
  int r, g, b;
  unpack(p, &r, &g, &b);
  int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  int d = mx - mn;
  if (d == 0 || 5 * d < mx) return 24 + (mx >> 5);
  int sat = 2 * d < mx ? 0 : 1;
  int t, base;
  if (mx == r) {
    t = 2 * (g - b);
    base = 0;
  } else if (mx == g) {
    t = 2 * (b - r);
    base = 4;
  } else {
    t = 2 * (r - g);
    base = 8;
  }
  /* hue / 30 = base + t / d with t in [-2d, 2d]: floor division */
  int q = t >= 0 ? t / d : -((-t + d - 1) / d);
  return ((base + q + 12) % 12) * 2 + sat;
}

/* ---- Grey image, integral images, area-average resampling -------------- */

static void build_gray(const uint16_t *frame) {
  mxg_half(frame, s_w.gray);
  const int st = HW + 1;
  memset(s_w.gii, 0, (size_t)st * sizeof s_w.gii[0]);
  for (int y = 0; y < HH; y++) {
    uint32_t row = 0;
    uint32_t *dst = s_w.gii + (y + 1) * st, *up = s_w.gii + y * st;
    const uint8_t *src = s_w.gray + y * HW;
    dst[0] = 0;
    for (int x = 0; x < HW; x++) {
      row += src[x];
      dst[x + 1] = up[x + 1] + row;
    }
  }
}

static uint32_t box_sum(const uint32_t *ii, int stride, int x0, int y0, int x1, int y1) {
  return ii[y1 * stride + x1] - ii[y0 * stride + x1] - ii[y1 * stride + x0] + ii[y0 * stride + x0];
}

/* Edge k of an n-cell grid sits at o + k*p (clamped to [0, lim]): index
 * idx[k] <= lim - 1 plus a fraction frac[k] in 1/256 (0..256). */
static void cell_edges(float o, float p, int n, int lim, uint8_t *idx, uint16_t *frac) {
  for (int k = 0; k <= n; k++) {
    float e = clampf(o + (float)k * p, 0.0f, (float)lim);
    int i = (int)e;
    if (i > lim - 1) i = lim - 1;
    idx[k] = (uint8_t)i;
    frac[k] = (uint16_t)clampi((int)((e - (float)i) * 256.0f + 0.5f), 0, 256);
  }
}

/* Exact area average of the grey image over an n x m grid with origin
 * (ox, oy) and cell size (px, py) in 160x120 units (fractional cell edges).
 * The integral image of a piecewise-constant image is bilinear inside each
 * pixel, so interpolating it at the cell corners gives exact area sums. */
static void resample(float ox, float oy, float px, float py, int n, int m, uint8_t *dst) {
  const int st = HW + 1, js = n + 1;
  uint32_t *jj = s_w.ii2; /* corner integrals x256, (n+1) x (m+1) */
  cell_edges(ox, px, n, HW, s_w.bx, s_w.fx);
  cell_edges(oy, py, m, HH, s_w.by, s_w.fy);
  for (int j = 0; j <= m; j++) {
    const uint32_t *r0 = s_w.gii + s_w.by[j] * st, *r1 = r0 + st;
    const uint64_t wy = s_w.fy[j];
    for (int i = 0; i <= n; i++) {
      const int x = s_w.bx[i];
      const uint32_t wx = s_w.fx[i];
      /* each row interpolated in x (x256 fits u32: <= 256 * 160*120*255) */
      uint64_t a = (uint64_t)(256u - wx) * r0[x] + (uint64_t)wx * r0[x + 1];
      uint64_t b = (uint64_t)(256u - wx) * r1[x] + (uint64_t)wx * r1[x + 1];
      jj[j * js + i] = (uint32_t)(((256u - wy) * a + wy * b + 128u) >> 8); /* x256, <= 1.26e9 */
    }
  }
  for (int i = 0; i < n; i++) {
    const int ax = (s_w.bx[i + 1] - s_w.bx[i]) * 256 + s_w.fx[i + 1] - s_w.fx[i];
    s_w.rx[i] = ax > 0 ? 256.0f / (float)ax : 0.0f;
  }
  for (int j = 0; j < m; j++) {
    const int ay = (s_w.by[j + 1] - s_w.by[j]) * 256 + s_w.fy[j + 1] - s_w.fy[j];
    const float ry = ay > 0 ? 1.0f / (float)ay : 0.0f;
    const uint32_t *j0 = jj + j * js, *j1 = j0 + js;
    for (int i = 0; i < n; i++) {
      uint32_t sum = j1[i + 1] - j0[i + 1] - j1[i] + j0[i]; /* x256; exact in u32 arithmetic */
      int v = (int)((float)sum * s_w.rx[i] * ry + 0.5f);
      dst[j * n + i] = (uint8_t)(v > 255 ? 255 : v);
    }
  }
}

/* ---- NCC ---------------------------------------------------------------- */

typedef struct {
  const uint8_t *t;
  int tw, th, n;
  int64_t st, vt; /* sum T, n * sum T^2 - (sum T)^2 */
} tmpl_t;

static void tmpl_init(tmpl_t *t, const uint8_t *data, int tw, int th) {
  t->t = data;
  t->tw = tw;
  t->th = th;
  t->n = tw * th;
  int64_t s = 0, s2 = 0;
  for (int i = 0; i < t->n; i++) {
    s += data[i];
    s2 += (int64_t)data[i] * data[i];
  }
  t->st = s;
  t->vt = (int64_t)t->n * s2 - s * s;
}

static float ncc_score(const tmpl_t *t, uint32_t p, uint32_t s, uint32_t s2) {
  int64_t n = t->n;
  int64_t vw = n * (int64_t)s2 - (int64_t)s * s;
  if (t->vt <= 0 || vw < n * n * (MIN_WIN_STD * MIN_WIN_STD)) return 0.0f;
  int64_t num = n * (int64_t)p - t->st * (int64_t)s;
  return (float)num / sqrtf((float)t->vt * (float)vw);
}

/* Sum of T * W over a tw x th template (contiguous) against window rows of
 * stride wr starting at a. Unrolled x4: on Xtensa the inner loop is 18
 * instructions per 4 MACs (zero-overhead loop, mul16u) plus ~27 per row. */
static uint32_t dot(const uint8_t *tp, const uint8_t *a, int tw, int th, int wr) {
  uint32_t acc = 0;
  const int n4 = tw >> 2, rem = tw & 3;
  for (int j = 0; j < th; j++, a += wr) {
    const uint8_t *r = a;
    for (int k = 0; k < n4; k++) {
      acc += (uint32_t)r[0] * tp[0] + (uint32_t)r[1] * tp[1] + (uint32_t)r[2] * tp[2] + (uint32_t)r[3] * tp[3];
      r += 4;
      tp += 4;
    }
    for (int k = 0; k < rem; k++) acc += (uint32_t)r[k] * *tp++;
  }
  COUNT_MACS(tw * th);
  return acc;
}

/* NCC of two equally sized templates. */
static float ncc_pair(const tmpl_t *a, const uint8_t *b) {
  uint32_t p = 0, s = 0, s2 = 0;
  for (int i = 0; i < a->n; i++) {
    p += (uint32_t)a->t[i] * b[i];
    s += b[i];
    s2 += (uint32_t)b[i] * b[i];
  }
  return ncc_score(a, p, s, s2);
}

/* Search window at one scale: grid origin (ox, oy) and cell size (px, py) in
 * 160x120 units, wr x hr cells. */
typedef struct {
  float ox, oy, px, py;
  int wr, hr;
} geom_t;

/* Window = box centre +- (box/2 + long side) (320 px), clipped to the frame,
 * resampled so the target at scale cs is template-sized. Returns false when
 * the window cannot hold the template. */
static bool window_geom(const mxk_model_t *m, int cx, int cy, int bw, int bh, float cs, geom_t *g) {
  int lng = bw > bh ? bw : bh;
  float xc = (float)cx * 0.5f, yc = (float)cy * 0.5f;
  float ex = ((float)bw * 0.5f + (float)lng) * 0.5f, ey = ((float)bh * 0.5f + (float)lng) * 0.5f;
  g->px = cs * ((float)m->box_w * 0.5f) / (float)m->tw;
  g->py = cs * ((float)m->box_h * 0.5f) / (float)m->th;
  float x0 = clampf(xc - ex, 0.0f, (float)HW), x1 = clampf(xc + ex, 0.0f, (float)HW);
  float y0 = clampf(yc - ey, 0.0f, (float)HH), y1 = clampf(yc + ey, 0.0f, (float)HH);
  g->wr = (int)((x1 - x0) / g->px);
  g->hr = (int)((y1 - y0) / g->py);
  if (g->wr > WMAX) {
    g->wr = WMAX;
    float span = (float)WMAX * g->px;
    x0 = clampf(xc - span * 0.5f, 0.0f, (float)HW - span);
  }
  if (g->hr > WMAX) {
    g->hr = WMAX;
    float span = (float)WMAX * g->py;
    y0 = clampf(yc - span * 0.5f, 0.0f, (float)HH - span);
  }
  g->ox = x0;
  g->oy = y0;
  return g->wr >= m->tw && g->hr >= m->th;
}

/* NCC multiply-accumulates of one scale: coarse grid + the 8 refine points. */
static uint32_t scale_macs(const geom_t *g, const mxk_model_t *m, int step) {
  uint32_t nu = (uint32_t)((g->wr - m->tw) / step + 1), nv = (uint32_t)((g->hr - m->th) / step + 1);
  return (nu * nv + 8u) * (uint32_t)m->tw * m->th;
}

typedef struct {
  float score, cs;
  int cx, cy; /* 320 px */
} match_t;

/* NCC search over one window; on a better score than *best, updates it and
 * copies the matched patch into s_w.patch. */
static void search_scale(const tmpl_t *t, const geom_t *g, float cs, int step, match_t *best) {
  const int wr = g->wr, hr = g->hr, st = wr + 1;
  resample(g->ox, g->oy, g->px, g->py, wr, hr, s_w.win);
  memset(s_w.ii, 0, (size_t)st * sizeof s_w.ii[0]);
  memset(s_w.ii2, 0, (size_t)st * sizeof s_w.ii2[0]);
  for (int y = 0; y < hr; y++) {
    uint32_t r1 = 0, r2 = 0;
    const uint8_t *src = s_w.win + y * wr;
    s_w.ii[(y + 1) * st] = 0;
    s_w.ii2[(y + 1) * st] = 0;
    for (int x = 0; x < wr; x++) {
      r1 += src[x];
      r2 += (uint32_t)src[x] * src[x];
      s_w.ii[(y + 1) * st + x + 1] = s_w.ii[y * st + x + 1] + r1;
      s_w.ii2[(y + 1) * st + x + 1] = s_w.ii2[y * st + x + 1] + r2;
    }
  }
  const int umax = wr - t->tw, vmax = hr - t->th;
#define SCORE(u, v)                                                                                     \
  ncc_score(t, dot(t->t, s_w.win + (v) * wr + (u), t->tw, t->th, wr), box_sum(s_w.ii, st, (u), (v), (u) + t->tw, (v) + t->th), \
            box_sum(s_w.ii2, st, (u), (v), (u) + t->tw, (v) + t->th))
  float bs = -2.0f;
  int bu = 0, bv = 0;
  for (int v = 0; v <= vmax; v += step)
    for (int u = 0; u <= umax; u += step) {
      float s = SCORE(u, v);
      if (s > bs) {
        bs = s;
        bu = u;
        bv = v;
      }
    }
  const int cu = bu, cv = bv;
  for (int dv = -1; dv <= 1; dv++)
    for (int du = -1; du <= 1; du++) {
      int u = cu + du, v = cv + dv;
      if ((du == 0 && dv == 0) || u < 0 || v < 0 || u > umax || v > vmax) continue;
      float s = SCORE(u, v);
      if (s > bs) {
        bs = s;
        bu = u;
        bv = v;
      }
    }
#undef SCORE
  if (bs <= best->score) return;
  best->score = bs;
  best->cs = cs;
  best->cx = (int)lroundf(2.0f * (g->ox + ((float)bu + (float)t->tw * 0.5f) * g->px));
  best->cy = (int)lroundf(2.0f * (g->oy + ((float)bv + (float)t->th * 0.5f) * g->py));
  for (int j = 0; j < t->th; j++) memcpy(s_w.patch + j * t->tw, s_w.win + (bv + j) * wr + bu, (size_t)t->tw);
}

/* Multi-scale search around (cx, cy) with the current box. st->tick (toggled
 * once per frame by mxk_track) picks the extra scale under fallback (1). */
static match_t search(const mxk_model_t *m, const mxp_track_config_t *cfg, const mxk_state_t *st,
                      const uint8_t *tdata, int cx, int cy, uint32_t budget) {
  match_t best = {-2.0f, 1.0f, cx, cy};
  tmpl_t t;
  tmpl_init(&t, tdata, m->tw, m->th);
  float c = m->box_w >= m->box_h ? (float)st->w / (float)m->box_w : (float)st->h / (float)m->box_h;
  c = clampf(c, SCALE_MIN, SCALE_MAX);
  float scales[3] = {c, clampf(c * 0.9f, SCALE_MIN, SCALE_MAX), clampf(c * 1.1f, SCALE_MIN, SCALE_MAX)};
  geom_t g[3];
  bool ok[3] = {false, false, false};
  int nsc = cfg->scale ? 3 : 1;
  for (int i = 0; i < nsc; i++) {
    ok[i] = window_geom(m, cx, cy, st->w, st->h, scales[i], &g[i]);
    if (i > 0 && scales[i] == c) ok[i] = false; /* clamped onto the base scale */
  }
  /* Budget fallbacks, in the plan's order: (1) one extra scale per frame,
   * alternating 0.9 / 1.1; (2) coarse step 3; then (only reachable with the
   * half budget of a lost + reacquire frame) the base scale alone. */
  int step = 2;
  bool use[3] = {ok[0], nsc > 1 && ok[1], nsc > 1 && ok[2]};
  uint32_t cost = 0;
  for (int i = 0; i < 3; i++)
    if (use[i]) cost += scale_macs(&g[i], m, step);
  if (cost > budget && nsc > 1) {
    if (st->tick) use[2] = false; else use[1] = false;
    cost = 0;
    for (int i = 0; i < 3; i++)
      if (use[i]) cost += scale_macs(&g[i], m, step);
  }
  if (cost > budget) {
    step = 3;
    cost = 0;
    for (int i = 0; i < 3; i++)
      if (use[i]) cost += scale_macs(&g[i], m, step);
  }
  if (cost > budget) use[1] = use[2] = false;
  for (int i = 0; i < 3; i++)
    if (use[i]) search_scale(&t, &g[i], scales[i], step, &best);
  return best;
}

/* Back-projection of the learned histogram over the whole frame (weight of a
 * cell = hist[bin] of the top-left pixel of its 2x2 block); the centre of the
 * bw x bh (320 px) window with the largest weight. */
static bool backproject(const uint16_t *frame, const mxk_model_t *m, int bw, int bh, int *cx, int *cy) {
  const int st = HW + 1;
  uint32_t *ii = s_w.ii;
  memset(ii, 0, (size_t)st * sizeof ii[0]);
  for (int y = 0; y < HH; y++) {
    uint32_t row = 0;
    const uint16_t *src = frame + (2 * y) * FW;
    ii[(y + 1) * st] = 0;
    for (int x = 0; x < HW; x++) {
      row += m->hist[color_bin(src[2 * x])];
      ii[(y + 1) * st + x + 1] = ii[y * st + x + 1] + row;
    }
  }
  int hw = clampi(bw / 2, 1, HW), hh = clampi(bh / 2, 1, HH);
  uint32_t best = 0;
  int bx = -1, by = -1;
  for (int y = 0; y + hh <= HH; y++)
    for (int x = 0; x + hw <= HW; x++) {
      uint32_t s = box_sum(ii, st, x, y, x + hw, y + hh);
      if (s > best) {
        best = s;
        bx = x;
        by = y;
      }
    }
  if (bx < 0) return false;
  *cx = 2 * bx + hw;
  *cy = 2 * by + hh;
  return true;
}

/* ---- Public API ---------------------------------------------------------- */

bool mxk_learn(const uint16_t *frame, uint16_t x, uint16_t y, uint16_t w, uint16_t h, mxk_model_t *out) {
  if (x >= FW || y >= FH) return false;
  int x0 = x, y0 = y, x1 = x0 + w, y1 = y0 + h;
  if (x1 > FW) x1 = FW;
  if (y1 > FH) y1 = FH;
  int bw = x1 - x0, bh = y1 - y0;
  if (bw < MXK_MIN_BOX || bh < MXK_MIN_BOX) return false;

  build_gray(frame);
  /* The template covers exactly the box: box_w/2 x box_h/2 half-pixels from
   * (x0/2, y0/2), the same geometry mxk_track assumes (no bias for odd
   * coordinates or sizes). hw/hh only size it. */
  int hx0 = x0 / 2, hy0 = y0 / 2, hx1 = (x1 + 1) / 2, hy1 = (y1 + 1) / 2;
  int hw = (bw + 1) / 2, hh = (bh + 1) / 2, lng = hw > hh ? hw : hh;
  int tw = hw, th = hh;
  if (lng > MXK_TMPL_MAX) {
    tw = (hw * MXK_TMPL_MAX + lng / 2) / lng;
    th = (hh * MXK_TMPL_MAX + lng / 2) / lng;
  }
  tw = clampi(tw, MXK_TMPL_MIN, MXK_TMPL_MAX);
  th = clampi(th, MXK_TMPL_MIN, MXK_TMPL_MAX);
  uint8_t *tmpl = s_w.patch; /* scratch: keeps 1 KB off the caller's stack */
  memset(tmpl, 0, TN);
  resample((float)x0 * 0.5f, (float)y0 * 0.5f, (float)bw * 0.5f / (float)tw, (float)bh * 0.5f / (float)th, tw, th,
           tmpl);
  tmpl_t t;
  tmpl_init(&t, tmpl, tw, th);
  if (t.vt < (int64_t)t.n * t.n * (MXK_MIN_STD * MXK_MIN_STD)) return false; /* no texture */

  /* histogram on the same 2x2-block sampling grid as the back-projection */
  uint32_t cnt[MXK_HIST_BINS] = {0}, n = 0;
  for (int yy = hy0; yy < hy1; yy++)
    for (int xx = hx0; xx < hx1; xx++) {
      cnt[color_bin(frame[(2 * yy) * FW + 2 * xx])]++;
      n++;
    }
  /* mean colour over every pixel of the box */
  uint32_t sr = 0, sg = 0, sb = 0;
  for (int yy = y0; yy < y1; yy++)
    for (int xx = x0; xx < x1; xx++) {
      int r, g, b;
      unpack(frame[yy * FW + xx], &r, &g, &b);
      sr += (uint32_t)r;
      sg += (uint32_t)g;
      sb += (uint32_t)b;
    }
  uint32_t np = (uint32_t)(bw * bh);

  memset(out, 0, sizeof *out);
  out->tw = (uint8_t)tw;
  out->th = (uint8_t)th;
  out->box_w = (uint16_t)bw;
  out->box_h = (uint16_t)bh;
  out->cx = (int16_t)(x0 + bw / 2);
  out->cy = (int16_t)(y0 + bh / 2);
  out->rgb[0] = (uint8_t)((sr + np / 2) / np);
  out->rgb[1] = (uint8_t)((sg + np / 2) / np);
  out->rgb[2] = (uint8_t)((sb + np / 2) / np);
  uint32_t sum = 0;
  int top = 0;
  for (int i = 0; i < MXK_HIST_BINS; i++) {
    out->hist[i] = (uint16_t)((cnt[i] * 65535u + n / 2) / n);
    sum += out->hist[i];
    if (cnt[i] > cnt[top]) top = i;
  }
  out->hist[top] = (uint16_t)((int32_t)out->hist[top] + (65535 - (int32_t)sum)); /* exact sum */
  memcpy(out->tmpl, tmpl, TN);
  return true;
}

bool mxk_model_valid(const mxk_model_t *m) {
  return m->tw >= MXK_TMPL_MIN && m->tw <= MXK_TMPL_MAX && m->th >= MXK_TMPL_MIN && m->th <= MXK_TMPL_MAX &&
         m->box_w >= MXK_MIN_BOX && m->box_w <= FW && m->box_h >= MXK_MIN_BOX && m->box_h <= FH && m->cx >= 0 &&
         m->cx < FW && m->cy >= 0 && m->cy < FH;
}

void mxk_reset(mxk_state_t *st, const mxk_model_t *m) {
  memset(st, 0, sizeof *st);
  memcpy(st->live, m->tmpl, sizeof st->live);
  st->cx = m->cx;
  st->cy = m->cy;
  st->w = m->box_w;
  st->h = m->box_h;
}

static void go_lost(mxk_state_t *st, const mxk_model_t *m) {
  if (!st->lost) memcpy(st->live, m->tmpl, sizeof st->live);
  st->lost = 1;
  if (st->lost_frames < 0xFFFF) st->lost_frames++;
}

uint8_t mxk_track(const uint16_t *frame, const mxk_model_t *m, const mxp_track_config_t *cfg, mxk_state_t *st,
                  mxp_result_t *out) {
#ifdef MXK_TEST_HOOKS
  s_macs = 0;
#endif
  if (!mxk_model_valid(m)) return 0;
  int thr = clampi(cfg->threshold, 10, 95), adapt = clampi(cfg->adapt, 0, 50);
  build_gray(frame);

  st->tick ^= 1;

  /* Always the last position first (also while lost: a target that comes back
   * where it was lost is found even if a bigger same-colour decoy wins the
   * back-projection). */
  /* a lost + reacquire frame may run two searches: each gets half the budget */
  const bool reacq = st->lost && cfg->reacquire;
  const uint32_t budget = reacq ? MXK_MAC_BUDGET / 2 : MXK_MAC_BUDGET;
  match_t b = search(m, cfg, st, st->live, st->cx, st->cy, budget);
  if (b.score * 100.0f < (float)thr && reacq) {
    int cx, cy, lng = st->w > st->h ? st->w : st->h;
    /* a peak within one long side was already inside the first window */
    if (backproject(frame, m, st->w, st->h, &cx, &cy) && (abs(cx - st->cx) > lng || abs(cy - st->cy) > lng)) {
      match_t b2 = search(m, cfg, st, st->live, cx, cy, budget);
      if (b2.score > b.score) b = b2;
    }
  }
  int conf = b.score > 0.0f ? (int)lroundf(b.score * 100.0f) : 0;
  if (conf > 100) conf = 100;
  if (conf < thr) {
    go_lost(st, m);
    return 0;
  }

  /* found */
  st->lost = 0;
  st->lost_frames = 0;
  st->cx = (int16_t)clampi(b.cx, 0, FW - 1);
  st->cy = (int16_t)clampi(b.cy, 0, FH - 1);
  st->w = (uint16_t)lroundf((float)m->box_w * b.cs);
  st->h = (uint16_t)lroundf((float)m->box_h * b.cs);
  int upd = thr > UPDATE_MIN ? thr : UPDATE_MIN;
  if (adapt > 0 && conf >= upd) {
    tmpl_t learned;
    tmpl_init(&learned, m->tmpl, m->tw, m->th);
    if (ncc_pair(&learned, s_w.patch) * 100.0f >= (float)thr) {
      for (int i = 0; i < m->tw * m->th; i++) {
        int d = (s_w.patch[i] - st->live[i]) * adapt;
        st->live[i] = (uint8_t)(st->live[i] + (d >= 0 ? d + 50 : d - 50) / 100);
      }
    }
  }
  memset(out, 0, sizeof *out);
  out->id = 1;
  out->x = st->cx;
  out->y = st->cy;
  out->w = st->w;
  out->h = st->h;
  out->confidence = (uint8_t)conf;
  out->extra_len = 0;
  return 1;
}
