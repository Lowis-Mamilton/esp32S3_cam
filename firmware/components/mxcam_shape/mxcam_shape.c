#include "mxcam_shape.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mxcam_gray.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "sdkconfig.h"
#if !CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#error "mxcam_shape needs CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y (work buffers live in PSRAM BSS)"
#endif
#else
#define EXT_RAM_BSS_ATTR
#endif

#define HW MXG_HALF_W
#define HH MXG_HALF_H
#define HN (HW * HH)
#define PI_F 3.14159265f
#define NMAX MXSH_MAX_CONTOUR
/* Douglas-Peucker stops (not a reportable shape) once it keeps more points
 * than this: a triangle or quadrilateral needs a handful, and the cap bounds a
 * ragged region's cost to ~DP_MAX_KEEP * n distance evaluations. */
#define DP_MAX_KEEP 12

/* mask values */
#define BG 0
#define FG 1
#define SEEN 2

/* ---- Work buffers (static, no malloc) ----------------------------------
 *   gray   u8  [160*120]   19.2 KB  mxg_half output
 *   mask   u8  [160*120]   19.2 KB  0 background, 1 shape pixel, 2 visited
 *   stack  u16 [160*120]   38.4 KB  flood fill (each pixel pushed at most once)
 *   pts    i16 [1024][2]    4 KB    outer contour of the current region
 *   keep   u8  [1025]       1 KB    Douglas-Peucker: point kept as a vertex
 *   seg    u16 [1024][2]    4 KB    Douglas-Peucker stack
 *   vtx    u16 [1024]       2 KB    vertex offsets (contour order)
 *   hist   u32 [256]        1 KB    Otsu histogram
 * ~89 KB in PSRAM on target (EXT_RAM_BSS_ATTR); not reentrant. Canaries sit
 * after each array so tests can detect overruns without ASan. */
#define CANARY 0xA5A5A5A5u
typedef struct {
  uint8_t gray[HN];
  uint32_t c0;
  uint8_t mask[HN];
  uint32_t c1;
  uint16_t stack[HN];
  uint32_t c2;
  int16_t pts[NMAX][2];
  uint32_t c3;
  uint8_t keep[NMAX + 1];
  uint32_t c4;
  uint16_t seg[NMAX][2];
  uint32_t c5;
  uint16_t vtx[NMAX];
  uint32_t c6;
  uint32_t hist[256];
  uint32_t c7;
} work_t;
static EXT_RAM_BSS_ATTR work_t s_w;

#ifdef MXSH_TEST_HOOKS
void mxsh_test_canary_arm(void);
bool mxsh_test_canary_ok(void);
void mxsh_test_canary_arm(void) {
  s_w.c0 = s_w.c1 = s_w.c2 = s_w.c3 = s_w.c4 = s_w.c5 = s_w.c6 = s_w.c7 = CANARY;
}
bool mxsh_test_canary_ok(void) {
  return s_w.c0 == CANARY && s_w.c1 == CANARY && s_w.c2 == CANARY && s_w.c3 == CANARY && s_w.c4 == CANARY &&
         s_w.c5 == CANARY && s_w.c6 == CANARY && s_w.c7 == CANARY;
}
#endif

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- Classification ------------------------------------------------------ */

/* The closed contour seen from an anchor: offset k is point (a + k) % n, so
 * offset n is the anchor again. */
typedef struct {
  const int16_t (*p)[2];
  int n, a;
} ring_t;
static float rx(const ring_t *r, int k) { return (float)r->p[(r->a + k) % r->n][0]; }
static float ry(const ring_t *r, int k) { return (float)r->p[(r->a + k) % r->n][1]; }

/* Distance from point k to the line through points s and e (to point s when
 * they coincide). */
static float line_dist(const ring_t *r, int s, int e, int k) {
  float x0 = rx(r, s), y0 = ry(r, s), dx = rx(r, e) - x0, dy = ry(r, e) - y0;
  float px = rx(r, k) - x0, py = ry(r, k) - y0, len = sqrtf(dx * dx + dy * dy);
  if (len < 1e-3f) return sqrtf(px * px + py * py);
  return fabsf(dx * py - dy * px) / len;
}

/* Douglas-Peucker over the closed contour, split at the anchor (offset 0, the
 * farthest point from the centroid) and offset b (the farthest point from the
 * anchor); then, while more than 3 vertices remain, the vertex closest to the
 * line through its two neighbours is dropped if that distance is below eps
 * (so an anchor that is not a real corner goes away). Writes the vertex
 * offsets (ascending) to s_w.vtx and returns their count, or 0 when more than
 * DP_MAX_KEEP points were kept. */
static int simplify(const ring_t *r, int b, float eps) {
  int n = r->n;
  memset(s_w.keep, 0, (size_t)n + 1);
  s_w.keep[0] = s_w.keep[b] = 1;
  int sp = 0, kept = 2;
  s_w.seg[sp][0] = 0;
  s_w.seg[sp++][1] = (uint16_t)b;
  s_w.seg[sp][0] = (uint16_t)b;
  s_w.seg[sp++][1] = (uint16_t)n;
  while (sp > 0) {
    sp--;
    int s = s_w.seg[sp][0], e = s_w.seg[sp][1];
    if (e - s < 2) continue;
    float dmax = -1.0f;
    int m = s;
    for (int k = s + 1; k < e; k++) {
      float d = line_dist(r, s, e, k);
      if (d > dmax) {
        dmax = d;
        m = k;
      }
    }
    /* disjoint open intervals: the stack never holds more than n entries */
    if (dmax <= eps || sp + 2 > NMAX) continue;
    if (++kept > DP_MAX_KEEP) return 0;
    s_w.keep[m] = 1;
    s_w.seg[sp][0] = (uint16_t)s;
    s_w.seg[sp++][1] = (uint16_t)m;
    s_w.seg[sp][0] = (uint16_t)m;
    s_w.seg[sp++][1] = (uint16_t)e;
  }
  int nv = 0;
  for (int k = 0; k < n; k++)
    if (s_w.keep[k]) s_w.vtx[nv++] = (uint16_t)k;
  while (nv > 3) {
    float best = eps;
    int bi = -1;
    for (int i = 0; i < nv; i++) {
      float d = line_dist(r, s_w.vtx[(i + nv - 1) % nv], s_w.vtx[(i + 1) % nv], s_w.vtx[i]);
      if (d < best) {
        best = d;
        bi = i;
      }
    }
    if (bi < 0) break;
    memmove(&s_w.vtx[bi], &s_w.vtx[bi + 1], (size_t)(nv - bi - 1) * sizeof s_w.vtx[0]);
    nv--;
  }
  return nv;
}

static uint8_t ratio_conf(float a, float b) {
  if (a <= 0.0f || b <= 0.0f) return 0;
  float q = a < b ? a / b : b / a;
  return (uint8_t)lroundf(q * 100.0f);
}

/* The contour area A' against the polygon's area + half its perimeter (the
 * same half-pixel band as A'). */
static uint8_t poly_conf(const ring_t *r, int nv, float area) {
  float a2 = 0.0f, per = 0.0f;
  for (int i = 0; i < nv; i++) {
    int k0 = s_w.vtx[i], k1 = s_w.vtx[(i + 1) % nv];
    float x0 = rx(r, k0), y0 = ry(r, k0), x1 = rx(r, k1), y1 = ry(r, k1);
    a2 += x0 * y1 - x1 * y0;
    per += sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
  }
  return ratio_conf(area, fabsf(a2) * 0.5f + per * 0.5f);
}

/* Quadrilateral: convex, every interior angle within 90 +- tol -> square or
 * rectangle by the mean opposite side lengths; else 0. */
static uint16_t quad_id(const ring_t *r) {
  float ex[4], ey[4], len[4];
  for (int i = 0; i < 4; i++) {
    int k0 = s_w.vtx[i], k1 = s_w.vtx[(i + 1) % 4];
    ex[i] = rx(r, k1) - rx(r, k0);
    ey[i] = ry(r, k1) - ry(r, k0);
    len[i] = sqrtf(ex[i] * ex[i] + ey[i] * ey[i]);
    if (len[i] < 1e-3f) return 0;
  }
  const float cos_tol = sinf((float)MXSH_QUAD_ANGLE_TOL * PI_F / 180.0f); /* |cos(90 +- tol)| */
  int sign = 0;
  for (int i = 0; i < 4; i++) {
    int j = (i + 1) % 4;
    int s = ex[i] * ey[j] - ey[i] * ex[j] > 0.0f ? 1 : -1;
    if (sign && s != sign) return 0;
    sign = s;
    /* interior angle between -e_i and e_j */
    float c = -(ex[i] * ex[j] + ey[i] * ey[j]) / (len[i] * len[j]);
    if (fabsf(c) > cos_tol) return 0;
  }
  float a = (len[0] + len[2]) * 0.5f, b = (len[1] + len[3]) * 0.5f;
  float q = a > b ? a / b : b / a;
  return q <= MXSH_SQUARE_RATIO ? MXP_SHAPE_SQUARE : MXP_SHAPE_RECT;
}

/* Full classification; *area_out = A' (used for sorting). */
static uint16_t classify(const int16_t (*p)[2], int n, uint8_t *conf_out, float *area_out) {
  *conf_out = 0;
  *area_out = 0.0f;
  if (n < 3 || n > NMAX) return 0;
  /* perimeter, shoelace area and centroid (relative to the first point) */
  float ox = (float)p[0][0], oy = (float)p[0][1];
  float per = 0.0f, a2 = 0.0f, cx = 0.0f, cy = 0.0f;
  for (int i = 0; i < n; i++) {
    int j = i + 1 < n ? i + 1 : 0;
    float x0 = (float)p[i][0] - ox, y0 = (float)p[i][1] - oy, x1 = (float)p[j][0] - ox, y1 = (float)p[j][1] - oy;
    per += sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    float cr = x0 * y1 - x1 * y0;
    a2 += cr;
    cx += (x0 + x1) * cr;
    cy += (y0 + y1) * cr;
  }
  if (fabsf(a2) < 2.0f || per <= 0.0f) return 0; /* shoelace area below 1: a line */
  cx = cx / (3.0f * a2) + ox;
  cy = cy / (3.0f * a2) + oy;
  float area = fabsf(a2) * 0.5f + per * 0.5f; /* A': pixel-centre polygon + half-pixel band */
  *area_out = area;

  /* centroid distances: mean, coefficient of variation, farthest point */
  float sum = 0.0f, sum2 = 0.0f, far = -1.0f;
  int a = 0;
  for (int i = 0; i < n; i++) {
    float dx = (float)p[i][0] - cx, dy = (float)p[i][1] - cy, d2 = dx * dx + dy * dy;
    sum += sqrtf(d2);
    sum2 += d2;
    if (d2 > far) {
      far = d2;
      a = i;
    }
  }
  float mean = sum / (float)n, var = sum2 / (float)n - mean * mean;
  float cv = mean > 0.0f ? sqrtf(var > 0.0f ? var : 0.0f) / mean : 1.0f;
  float circ = 4.0f * PI_F * area / (per * per);
  if (circ >= MXSH_CIRC_MIN && cv <= MXSH_CV_MAX) {
    *conf_out = ratio_conf(area, PI_F * mean * mean + PI_F * mean); /* circle area + half its perimeter */
    return MXP_SHAPE_CIRCLE;
  }

  ring_t r = {p, n, a};
  float best = -1.0f;
  int b = 1;
  for (int k = 1; k < n; k++) {
    float dx = rx(&r, k) - rx(&r, 0), dy = ry(&r, k) - ry(&r, 0), d2 = dx * dx + dy * dy;
    if (d2 > best) {
      best = d2;
      b = k;
    }
  }
  float eps = per * (float)MXSH_DP_EPS_PCT / 100.0f;
  if (eps < 1.0f) eps = 1.0f;
  int nv = simplify(&r, b, eps);
  uint16_t id = 0;
  if (nv == 3) id = MXP_SHAPE_TRIANGLE;
  else if (nv == 4) id = quad_id(&r);
  if (id) *conf_out = poly_conf(&r, nv, area);
  return id;
}

uint16_t mxsh_classify(const int16_t (*pts)[2], int n, uint8_t *confidence) {
  uint8_t conf;
  float area;
  uint16_t id = classify(pts, n, &conf, &area);
  if (confidence) *confidence = conf;
  return id;
}

/* ---- Detection ----------------------------------------------------------- */

/* Otsu over s_w.gray (as mxcam_line). Returns false when there is no split or
 * the class means differ by less than MXSH_MIN_CONTRAST; *t_out is the last
 * value of the dark class (dark = gray <= t). */
static bool otsu(int *t_out) {
  memset(s_w.hist, 0, sizeof s_w.hist);
  for (int i = 0; i < HN; i++) s_w.hist[s_w.gray[i]]++;
  float sum = 0.0f;
  for (int i = 0; i < 256; i++) sum += (float)i * (float)s_w.hist[i];
  float best = -1.0f, diff = 0.0f, sum0 = 0.0f;
  uint32_t w0 = 0;
  int bt = -1;
  for (int t = 0; t < 255; t++) {
    w0 += s_w.hist[t];
    sum0 += (float)t * (float)s_w.hist[t];
    if (w0 == 0) continue;
    uint32_t w1 = HN - w0;
    if (w1 == 0) break;
    float m0 = sum0 / (float)w0, m1 = (sum - sum0) / (float)w1, d = m1 - m0;
    float var = (float)w0 * (float)w1 * d * d;
    if (var > best) {
      best = var;
      bt = t;
      diff = d;
    }
  }
  if (bt < 0 || diff < (float)MXSH_MIN_CONTRAST) return false;
  *t_out = bt;
  return true;
}

/* 8 neighbours, clockwise on screen (y down) starting east. */
static const int8_t DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
static const int8_t DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
static int dir_of(int dx, int dy) {
  static const int8_t T[3][3] = {{5, 6, 7}, {4, -1, 0}, {3, 2, 1}}; /* [dy + 1][dx + 1] */
  return T[dy + 1][dx + 1];
}

/* Moore neighbour tracing of the outer contour from (x0, y0), the region's
 * first pixel in raster order (so its west neighbour is background). The
 * region does not touch the frame edge, so every neighbour looked at is
 * inside the image, and every non-background neighbour belongs to the region.
 * Stops when the first move (start -> second point) would repeat. Returns the
 * point count in s_w.pts, or -1 when the contour is longer than NMAX. */
static int trace(int x0, int y0) {
  int n = 0, cx = x0, cy = y0, bdir = 4; /* backtrack neighbour: west */
  int p1x = 0, p1y = 0;
  s_w.pts[n][0] = (int16_t)x0;
  s_w.pts[n++][1] = (int16_t)y0;
  for (int step = 0;; step++) {
    int d = -1;
    for (int i = 1; i <= 8; i++) {
      int k = (bdir + i) & 7;
      if (s_w.mask[(cy + DY[k]) * HW + cx + DX[k]] != BG) {
        d = k;
        break;
      }
    }
    if (d < 0) return n; /* isolated pixel */
    int nx = cx + DX[d], ny = cy + DY[d];
    int pd = (d + 7) & 7; /* the last background neighbour checked */
    int bx = cx + DX[pd], by = cy + DY[pd];
    if (step == 0) {
      p1x = nx;
      p1y = ny;
    } else if (cx == x0 && cy == y0 && nx == p1x && ny == p1y) {
      return n - 1; /* the start point was appended again on arrival */
    }
    if (n >= NMAX) return -1;
    s_w.pts[n][0] = (int16_t)nx;
    s_w.pts[n++][1] = (int16_t)ny;
    bdir = dir_of(bx - nx, by - ny);
    cx = nx;
    cy = ny;
  }
}

typedef struct {
  int x0, y0, x1, y1;
  bool edge;
} region_t;

/* Flood fill (8-connected, explicit stack) of the region containing start;
 * marks it SEEN and returns its bounding box. */
static region_t flood(int start) {
  region_t g = {HW, HH, -1, -1, false};
  int sp = 0;
  s_w.mask[start] = SEEN;
  s_w.stack[sp++] = (uint16_t)start;
  while (sp > 0) {
    int i = s_w.stack[--sp], x = i % HW, y = i / HW;
    if (x < g.x0) g.x0 = x;
    if (x > g.x1) g.x1 = x;
    if (y < g.y0) g.y0 = y;
    if (y > g.y1) g.y1 = y;
    if (x == 0 || y == 0 || x == HW - 1 || y == HH - 1) g.edge = true;
    for (int k = 0; k < 8; k++) {
      int nx = x + DX[k], ny = y + DY[k];
      if (nx < 0 || ny < 0 || nx >= HW || ny >= HH) continue;
      int j = ny * HW + nx;
      if (s_w.mask[j] == FG) {
        s_w.mask[j] = SEEN;
        s_w.stack[sp++] = (uint16_t)j; /* each pixel is pushed once: sp <= HN */
      }
    }
  }
  return g;
}

uint8_t mxsh_detect(const uint16_t *frame, const mxp_shape_config_t *cfg, mxp_result_t *out, uint8_t cap) {
  /* Defensive clamps (mxp_unpack_shape_config already clamps protocol input). */
  int maxr = clampi(cfg->max_results, 1, MXP_MAX_RESULTS);
  if (maxr > cap) maxr = cap;
  if (maxr <= 0) return 0;
  int min_size = clampi(cfg->min_size, 16, 240);
  bool light = cfg->mode == 1;

  mxg_half(frame, s_w.gray);
  int lt, gt; /* shape pixel when gray < lt (dark) or gray > gt (light) */
  if (cfg->threshold) {
    lt = gt = cfg->threshold;
  } else {
    int t;
    if (!otsu(&t)) return 0;
    lt = t + 1; /* dark class: gray <= t */
    gt = t;
  }
  for (int i = 0; i < HN; i++) {
    uint8_t g = s_w.gray[i];
    s_w.mask[i] = (uint8_t)(light ? (g > gt) : (g < lt));
  }

  float areas[MXP_MAX_RESULTS];
  int count = 0;
  for (int i = 0; i < HN; i++) {
    if (s_w.mask[i] != FG) continue;
    region_t g = flood(i);
    int bw = g.x1 - g.x0 + 1, bh = g.y1 - g.y0 + 1;
    if (g.edge || (bw > bh ? bw : bh) * 2 < min_size) continue;
    if ((long)bw * bh * 100 > (long)MXSH_MAX_BG_PCT * HN) continue;
    int n = trace(i % HW, i / HW);
    if (n < 0) continue;
    uint8_t conf;
    float area;
    uint16_t id = classify((const int16_t(*)[2])s_w.pts, n, &conf, &area);
    if (!id) continue;
    /* insert, largest area first */
    int pos = count;
    while (pos > 0 && areas[pos - 1] < area) pos--;
    if (pos >= maxr) continue;
    int last = count < maxr ? count : maxr - 1;
    for (int k = last; k > pos; k--) {
      out[k] = out[k - 1];
      areas[k] = areas[k - 1];
    }
    if (count < maxr) count++;
    mxp_result_t *r = &out[pos];
    memset(r, 0, sizeof *r);
    int X0 = g.x0 * 2, X1 = g.x1 * 2 + 1, Y0 = g.y0 * 2, Y1 = g.y1 * 2 + 1;
    r->id = id;
    r->x = (int16_t)((X0 + X1) / 2);
    r->y = (int16_t)((Y0 + Y1) / 2);
    r->w = (uint16_t)(X1 - X0 + 1);
    r->h = (uint16_t)(Y1 - Y0 + 1);
    r->confidence = conf;
    r->extra_len = 0;
    areas[pos] = area;
  }
  return (uint8_t)count;
}

const char *mxsh_ascii_name(uint16_t id) {
  switch (id) {
    case MXP_SHAPE_CIRCLE:
      return "CIRCLE";
    case MXP_SHAPE_TRIANGLE:
      return "TRIANGLE";
    case MXP_SHAPE_SQUARE:
      return "SQUARE";
    case MXP_SHAPE_RECT:
      return "RECT";
    default:
      return "?";
  }
}

int mxsh_label(uint16_t id, uint8_t conf, char *buf, size_t cap) {
  return snprintf(buf, cap, "%s %u", mxsh_ascii_name(id), (unsigned)conf);
}
