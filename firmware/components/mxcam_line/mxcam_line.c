#include "mxcam_line.h"

#include <math.h>
#include <string.h>

#include "mxcam_color.h"
#include "mxcam_gray.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "sdkconfig.h"
#if !CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#error "mxcam_line needs CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y (work buffers live in PSRAM BSS)"
#endif
#else
#define EXT_RAM_BSS_ATTR
#endif

#define FW (2 * MXG_HALF_W)
#define HW MXG_HALF_W
#define HH MXG_HALF_H
#define MAX_SEGS (HW / 2) /* runs in 160 columns: at most 80 */
#define PI_F 3.14159265f
#define CENTRE_X 79.5f /* 160x120 x of the image centre (x = 160 in 320 units) */

/* ---- Work buffers (static, no malloc) ----------------------------------
 *   gray  u8  [160*120]   19.2 KB  mxg_half output
 *   mask  u8  [160*120]   19.2 KB  1 = line cell (ROI rows only)
 *   hist  u32 [256]        1 KB    Otsu histogram
 *   segs  i16 [12][80]     1.9 KB  valid segment centres per band (x2, i.e. start+end)
 * ~41 KB in PSRAM on target (EXT_RAM_BSS_ATTR); mxl_detect is not reentrant. */
typedef struct {
  uint8_t gray[HW * HH];
  uint8_t mask[HW * HH];
  uint32_t hist[256];
  int16_t segs[MXL_MAX_BANDS][MAX_SEGS];
  uint8_t nsegs[MXL_MAX_BANDS];
} work_t;
static EXT_RAM_BSS_ATTR work_t s_w;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Otsu over the ROI rows of s_w.gray. Returns false when there is no split or
 * the class means differ by less than MXL_MIN_CONTRAST; *t is the last value
 * of the dark class (dark = gray <= t). */
static bool otsu(int row0, int *t_out) {
  memset(s_w.hist, 0, sizeof s_w.hist);
  const uint8_t *g = s_w.gray + row0 * HW, *end = s_w.gray + HW * HH;
  for (; g < end; g++) s_w.hist[*g]++;
  uint32_t n = (uint32_t)(HH - row0) * HW;
  float sum = 0.0f;
  for (int i = 0; i < 256; i++) sum += (float)i * (float)s_w.hist[i];
  float best = -1.0f, diff = 0.0f, sum0 = 0.0f;
  uint32_t w0 = 0;
  int bt = -1;
  for (int t = 0; t < 255; t++) {
    w0 += s_w.hist[t];
    sum0 += (float)t * (float)s_w.hist[t];
    if (w0 == 0) continue;
    uint32_t w1 = n - w0;
    if (w1 == 0) break;
    float m0 = sum0 / (float)w0, m1 = (sum - sum0) / (float)w1, d = m1 - m0;
    float var = (float)w0 * (float)w1 * d * d;
    if (var > best) {
      best = var;
      bt = t;
      diff = d;
    }
  }
  if (bt < 0 || diff < (float)MXL_MIN_CONTRAST) return false;
  *t_out = bt;
  return true;
}

uint8_t mxl_detect(const uint16_t *frame, const mxp_line_config_t *cfg, const uint8_t *line_rgb,
                   mxl_state_t *st, mxp_result_t *out, mxl_trace_t *trace) {
  /* Defensive clamps (mxp_unpack_line_config already clamps protocol input). */
  int bands = clampi(cfg->bands, 4, MXL_MAX_BANDS);
  int roi_pct = clampi(cfg->roi_pct, 20, 100);
  int min_w = cfg->min_width < 1 ? 1 : cfg->min_width;
  int max_w = cfg->max_width < min_w ? min_w : cfg->max_width;
  int mode = cfg->mode == MXP_LINE_MODE_LIGHT ? 1 : (cfg->mode == MXP_LINE_MODE_LEARNED && line_rgb ? 2 : 0);

  int roi_h = HH * roi_pct / 100;
  int band_h = roi_h / bands;
  int row0 = HH - bands * band_h; /* first row of the top band (leftover rows dropped) */
  if (trace) {
    trace->n = 0;
    trace->roi_top = (int16_t)((HH - roi_h) * 2);
  }

  /* 1. Line mask over the ROI. */
  if (mode == 2) {
    for (int y = row0; y < HH; y++) {
      const uint16_t *src = frame + (2 * y) * FW;
      uint8_t *m = s_w.mask + y * HW;
      for (int x = 0; x < HW; x++) m[x] = mxc_match_px(src[2 * x], line_rgb, cfg->color_tolerance) ? 1 : 0;
    }
  } else {
    mxg_half(frame, s_w.gray);
    int lt, gt; /* line when gray < lt (dark) or gray > gt (light) */
    if (cfg->threshold) {
      lt = gt = cfg->threshold;
    } else {
      int t;
      if (!otsu(row0, &t)) {
        st->has_last = 0;
        return 0;
      }
      lt = t + 1; /* dark class: gray <= t */
      gt = t;     /* light class: gray > t */
    }
    for (int i = row0 * HW; i < HW * HH; i++) {
      uint8_t g = s_w.gray[i];
      s_w.mask[i] = mode == 1 ? (g > gt) : (g < lt);
    }
  }

  /* 2. Segments per band; BRANCH / CROSS flags over the whole ROI. */
  uint8_t flags = 0;
  for (int k = 0; k < bands; k++) {
    int r1 = HH - k * band_h, r0 = r1 - band_h;
    uint8_t cnt[HW];
    memset(cnt, 0, sizeof cnt);
    for (int y = r0; y < r1; y++) {
      const uint8_t *m = s_w.mask + y * HW;
      for (int x = 0; x < HW; x++) cnt[x] = (uint8_t)(cnt[x] + m[x]);
    }
    uint8_t ns = 0;
    for (int x = 0; x < HW;) {
      if (cnt[x] * 2 < band_h) {
        x++;
        continue;
      }
      int s = x;
      while (x < HW && cnt[x] * 2 >= band_h) x++;
      int w320 = (x - s) * 2;
      if (w320 > max_w) {
        flags |= MXP_LINE_FLAG_CROSS;
      } else if (w320 >= min_w) {
        s_w.segs[k][ns++] = (int16_t)(s + x - 1); /* centre x2 */
      }
    }
    s_w.nsegs[k] = ns;
    if (ns >= 2) flags |= MXP_LINE_FLAG_BRANCH;
  }

  /* 3. Walk the bands bottom-up, picking the segment nearest the prediction
   * (before the first pick: last_x0 or the centre; one pick: that x; then
   * extrapolated from the last two picks). */
  float sel_x[MXL_MAX_BANDS], sel_y[MXL_MAX_BANDS];
  int nsel = 0, skips = 0;
  float pred0 = st->has_last ? (float)st->last_x0 : CENTRE_X;
  float limit = (float)(2 * band_h > 8 ? 2 * band_h : 8);
  for (int k = 0; k < bands; k++) {
    int r1 = HH - k * band_h;
    float yc = (float)(2 * r1 - band_h - 1) * 0.5f; /* centre row of [r1 - band_h, r1) */
    float pred = pred0;
    if (nsel >= 2) {
      float ya = sel_y[nsel - 2], xa = sel_x[nsel - 2], yb = sel_y[nsel - 1], xb = sel_x[nsel - 1];
      pred = xb + (xb - xa) * (yc - yb) / (yb - ya);
    } else if (nsel == 1) {
      pred = sel_x[0];
    }
    int best = -1;
    float bd = 0.0f, bc = 0.0f;
    for (int i = 0; i < s_w.nsegs[k]; i++) {
      float cx = (float)s_w.segs[k][i] * 0.5f;
      float d = fabsf(cx - pred), dc = fabsf(cx - CENTRE_X);
      if (best < 0 || d < bd || (d == bd && dc < bc)) {
        best = i;
        bd = d;
        bc = dc;
      }
    }
    /* Acquisition: until the first band is picked there is no distance limit
     * and empty bands are not skips (the line may start above an empty band 0).
     * After that, a segment farther than `limit` is skipped, and two skipped
     * bands in a row end the walk. */
    if (best >= 0 && (nsel == 0 || bd <= limit)) {
      sel_x[nsel] = (float)s_w.segs[k][best] * 0.5f;
      sel_y[nsel] = yc;
      nsel++;
      skips = 0;
    } else if (nsel > 0 && ++skips >= 2) {
      break;
    }
  }

  if (trace) {
    trace->n = (uint8_t)nsel;
    for (int i = 0; i < nsel; i++) {
      trace->pts[i][0] = (int16_t)lroundf(sel_x[i] * 2.0f + 1.0f);
      trace->pts[i][1] = (int16_t)lroundf(sel_y[i] * 2.0f + 1.0f);
    }
  }
  if (nsel < 2) {
    st->has_last = 0;
    return 0;
  }

  /* 4. Least squares x = a*y + b over the picked band centres. */
  float my = 0.0f, mx = 0.0f;
  for (int i = 0; i < nsel; i++) {
    my += sel_y[i];
    mx += sel_x[i];
  }
  my /= (float)nsel;
  mx /= (float)nsel;
  float sxy = 0.0f, syy = 0.0f;
  for (int i = 0; i < nsel; i++) {
    float dy = sel_y[i] - my;
    sxy += dy * (sel_x[i] - mx);
    syy += dy * dy;
  }
  float a = sxy / syy; /* syy > 0: picked bands have distinct rows */
  float b = mx - a * my;
  float yb = sel_y[0], yt = sel_y[nsel - 1];
  float xb = a * yb + b, xt = a * yt + b;

  mxp_line_extra_t e;
  e.x0 = (int16_t)clampi((int)lroundf(xb * 2.0f + 1.0f), 0, FW - 1);
  e.y0 = (int16_t)lroundf(yb * 2.0f + 1.0f);
  e.x1 = (int16_t)clampi((int)lroundf(xt * 2.0f + 1.0f), 0, FW - 1);
  e.y1 = (int16_t)lroundf(yt * 2.0f + 1.0f);
  e.angle_cdeg = (int16_t)lroundf(atan2f((float)(e.x1 - e.x0), (float)(e.y0 - e.y1)) * 18000.0f / PI_F);
  e.offset_x = (int16_t)(e.x0 - FW / 2);
  e.flags = flags;

  memset(out, 0, sizeof *out);
  out->id = 1;
  out->x = (int16_t)((e.x0 + e.x1) / 2);
  out->y = (int16_t)((e.y0 + e.y1) / 2);
  int w = (e.x1 > e.x0 ? e.x1 - e.x0 : e.x0 - e.x1) + 1, h = e.y0 - e.y1 + 1;
  out->w = (uint16_t)(w < 2 ? 2 : w);
  out->h = (uint16_t)(h < 2 ? 2 : h);
  out->confidence = (uint8_t)(nsel * 100 / bands);
  mxp_pack_line_extra(&e, out);

  st->has_last = 1;
  st->last_x0 = (int16_t)clampi((int)lroundf(xb), 0, HW - 1);
  return 1;
}
