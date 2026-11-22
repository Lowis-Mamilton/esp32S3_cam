#include "mxcam_motion.h"

#include <stdbool.h>
#include <string.h>

#include "mxcam_gray.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "sdkconfig.h"
#if !CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#error "mxcam_motion needs CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y (work buffers live in PSRAM BSS)"
#endif
#else
#define EXT_RAM_BSS_ATTR
#endif

#define HW MXG_HALF_W
#define HH MXG_HALF_H
#define MW MXM_W
#define MH MXM_H
#define MN (MW * MH)
#define CELL 4 /* 320 px per 80 cells */
#define DMAX 255

/* ---- Work buffers and background (static, no malloc) --------------------
 *   half   u8  [160*120]   19.2 KB  mxg_half output
 *   cur    u8  [80*60]      4.8 KB  this frame, 2x2 mean of half
 *   bg     u16 [80*60]      9.6 KB  background x16 (kept between frames)
 *   chg    u8  [80*60]      4.8 KB  1 = changed cell
 *   dil    u8  [80*60]      4.8 KB  3x3 dilation of chg; 2 = visited
 *   stack  u16 [80*60]      9.6 KB  region fill (each cell pushed at most once)
 *   hist   u16 [511]        1 KB    (cur - bg) histogram for the median
 * ~54 KB in PSRAM on target (EXT_RAM_BSS_ATTR); not reentrant. Canaries sit
 * after each array so tests can detect overruns without ASan. */
#define CANARY 0xA5A5A5A5u
typedef struct {
  uint8_t half[HW * HH];
  uint32_t c0;
  uint8_t cur[MN];
  uint32_t c1;
  uint16_t bg[MN];
  uint32_t c2;
  uint8_t chg[MN];
  uint32_t c3;
  uint8_t dil[MN];
  uint32_t c4;
  uint16_t stack[MN];
  uint32_t c5;
  uint16_t hist[2 * DMAX + 1];
  uint32_t c6;
} work_t;
static EXT_RAM_BSS_ATTR work_t s_w;

/* Internal RAM, so the boot state is "reset" without an init call. */
static bool s_need_bg = true;       /* next frame becomes the background */
static uint8_t s_skip = MXM_WARMUP; /* frames that only update the background */

#ifdef MXM_TEST_HOOKS
void mxm_test_canary_arm(void);
bool mxm_test_canary_ok(void);
void mxm_test_canary_arm(void) { s_w.c0 = s_w.c1 = s_w.c2 = s_w.c3 = s_w.c4 = s_w.c5 = s_w.c6 = CANARY; }
bool mxm_test_canary_ok(void) {
  return s_w.c0 == CANARY && s_w.c1 == CANARY && s_w.c2 == CANARY && s_w.c3 == CANARY && s_w.c4 == CANARY &&
         s_w.c5 == CANARY && s_w.c6 == CANARY;
}
#endif

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void mxm_reset(void) {
  s_need_bg = true;
  s_skip = MXM_WARMUP;
}

static void set_bg(void) {
  for (int i = 0; i < MN; i++) s_w.bg[i] = (uint16_t)(s_w.cur[i] << 4);
}

/* bg += (cur*16 - bg) >> MXM_BG_SHIFT, rounding toward -infinity on both
 * signs (no implementation-defined right shift of a negative value). */
static void update_bg(void) {
  for (int i = 0; i < MN; i++) {
    int d = ((int)s_w.cur[i] << 4) - (int)s_w.bg[i];
    int step = d >= 0 ? d >> MXM_BG_SHIFT : -((-d + (1 << MXM_BG_SHIFT) - 1) >> MXM_BG_SHIFT);
    s_w.bg[i] = (uint16_t)((int)s_w.bg[i] + step);
  }
}

/* Median of (cur - bg) in whole grey levels: exposure compensation that a
 * moving object covering less than half the frame cannot drag. */
static int median_offset(void) {
  memset(s_w.hist, 0, sizeof s_w.hist);
  for (int i = 0; i < MN; i++) {
    int d16 = ((int)s_w.cur[i] << 4) - (int)s_w.bg[i];
    int d = d16 >= 0 ? (d16 + 8) >> 4 : -((-d16 + 8) >> 4);
    s_w.hist[clampi(d, -DMAX, DMAX) + DMAX]++;
  }
  int acc = 0;
  for (int b = 0; b <= 2 * DMAX; b++) {
    acc += s_w.hist[b];
    if (acc * 2 >= MN) return b - DMAX;
  }
  return 0;
}

typedef struct {
  int x0, y0, x1, y1; /* bounding box of the changed cells (80x60) */
  int changed;
} region_t;

/* 8-connected region of dilated cells containing start (explicit stack);
 * marks it visited and returns the box and count of its changed cells. */
static region_t fill_region(int start) {
  region_t g = {MW, MH, -1, -1, 0};
  int sp = 0;
  s_w.dil[start] = 2;
  s_w.stack[sp++] = (uint16_t)start;
  while (sp > 0) {
    int i = s_w.stack[--sp], x = i % MW, y = i / MW;
    if (s_w.chg[i]) {
      g.changed++;
      if (x < g.x0) g.x0 = x;
      if (x > g.x1) g.x1 = x;
      if (y < g.y0) g.y0 = y;
      if (y > g.y1) g.y1 = y;
    }
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        int nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= MW || ny >= MH) continue;
        int j = ny * MW + nx;
        if (s_w.dil[j] == 1) {
          s_w.dil[j] = 2;
          s_w.stack[sp++] = (uint16_t)j; /* each cell is pushed once: sp <= MN */
        }
      }
  }
  return g;
}

uint8_t mxm_detect(const uint16_t *frame, const mxp_motion_config_t *cfg, mxp_result_t *out, uint8_t cap,
                   mxm_stats_t *stats) {
  mxm_stats_t dummy;
  if (!stats) stats = &dummy;
  stats->global_reset = 0;
  stats->changed_cells = 0;
  /* Defensive clamps (mxp_unpack_motion_config already clamps protocol input). */
  int thr16 = clampi(cfg->threshold, 5, 100) << 4;
  int min_size = clampi(cfg->min_size, 8, 240);
  int maxr = clampi(cfg->max_results, 1, MXP_MAX_RESULTS);
  if (maxr > cap) maxr = cap;

  /* 1. 80x60 grey cells. */
  mxg_half(frame, s_w.half);
  for (int y = 0; y < MH; y++) {
    const uint8_t *a = s_w.half + (2 * y) * HW, *b = a + HW;
    uint8_t *dst = s_w.cur + y * MW;
    for (int x = 0; x < MW; x++, a += 2, b += 2) dst[x] = (uint8_t)(((unsigned)a[0] + a[1] + b[0] + b[1] + 2u) >> 2);
  }

  /* 2. Warm-up (after a reset) and the frames after a global change. */
  if (s_need_bg) {
    set_bg();
    s_need_bg = false;
  } else if (s_skip) {
    update_bg();
  }
  if (s_skip) {
    s_skip--;
    return 0;
  }

  /* 3. Changed cells, exposure compensated; a global change resets. */
  int off16 = median_offset() << 4, changed = 0;
  for (int i = 0; i < MN; i++) {
    int d = ((int)s_w.cur[i] << 4) - (int)s_w.bg[i] - off16;
    uint8_t c = (uint8_t)(d > thr16 || -d > thr16);
    s_w.chg[i] = c;
    changed += c;
  }
  stats->changed_cells = (uint16_t)changed;
  update_bg();
  if (changed * 100 > MXM_GLOBAL_PCT * MN) {
    set_bg();
    s_skip = MXM_GLOBAL_SKIP;
    stats->global_reset = 1;
    return 0;
  }
  if (changed == 0 || maxr <= 0) return 0;

  /* 4. 3x3 dilation joins the fragments of one moving thing. */
  for (int y = 0; y < MH; y++)
    for (int x = 0; x < MW; x++) {
      uint8_t v = 0;
      for (int dy = -1; dy <= 1 && !v; dy++) {
        int ny = y + dy;
        if (ny < 0 || ny >= MH) continue;
        for (int dx = -1; dx <= 1; dx++) {
          int nx = x + dx;
          if (nx >= 0 && nx < MW && s_w.chg[ny * MW + nx]) {
            v = 1;
            break;
          }
        }
      }
      s_w.dil[y * MW + x] = v;
    }

  /* 5. Regions, largest box first. */
  int count = 0;
  int areas[MXP_MAX_RESULTS];
  for (int i = 0; i < MN; i++) {
    if (s_w.dil[i] != 1) continue;
    region_t g = fill_region(i);
    if (g.changed == 0) continue;
    int bw = g.x1 - g.x0 + 1, bh = g.y1 - g.y0 + 1;
    if ((bw > bh ? bw : bh) * CELL < min_size) continue;
    int area = bw * bh;
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
    int X0 = g.x0 * CELL, X1 = g.x1 * CELL + CELL - 1, Y0 = g.y0 * CELL, Y1 = g.y1 * CELL + CELL - 1;
    r->id = 0;
    r->x = (int16_t)((X0 + X1) / 2);
    r->y = (int16_t)((Y0 + Y1) / 2);
    r->w = (uint16_t)(X1 - X0 + 1);
    r->h = (uint16_t)(Y1 - Y0 + 1);
    r->confidence = (uint8_t)((g.changed * 100 + area / 2) / area);
    r->extra_len = 0;
    areas[pos] = area;
  }
  return (uint8_t)count;
}
