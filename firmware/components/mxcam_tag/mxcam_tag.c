#include "mxcam_tag.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "apriltag.h"
#include "common/image_u8.h"
#include "common/timeprofile.h"
#include "common/zarray.h"
#include "tag36h11.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
/* The 19.2 KB half-res gray image is the hottest buffer: internal RAM. */
static void *alloc_hot(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}
static uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }
#else
#include <stdlib.h>
#include <time.h>
static void *alloc_hot(size_t n) { return malloc(n); }
/* Host only (timing is informational there): processor time, portable C99. */
static uint64_t now_us(void) { return (uint64_t)clock() * 1000000u / CLOCKS_PER_SEC; }
#endif

/* Memory: upstream mallocs freely (detector, per-frame zarrays, unionfind,
 * decimated image ...). On target every apriltag allocation goes to PSRAM via
 * the forced-include shim (apriltag/mxt_alloc_shim.h); without it the many
 * small per-frame blocks took ~130 KB of internal RAM at peak. Only our
 * 160x120 half-res gray image is internal. apriltag (and this file) are also
 * built single precision (apriltag/mxt_float_shim.h): the S3 FPU has no
 * double, and double math dominated quad fitting and decoding.
 *
 * Decode table: apriltag_detector_add_family() builds the hamming<=2 table,
 * which for tag36h11 is 587 * (1 + 36 + 36*35) * 3 entries * 16 B ~= 36 MB.
 * We build the hamming<=1 table instead (587 * 37 * 3 * 16 B ~= 1.0 MB, PSRAM)
 * and, consistently, report nothing that needed more than 1 corrected bit. */
#define MAX_HAMMING 1

static apriltag_detector_t *s_td;
static apriltag_family_t *s_tf;
static image_u8_t *s_gray;     /* 320x240 (full-res mode) */
static uint8_t *s_half;        /* 160x120 (half-res mode, allocated on first use) */
static float s_decimate = 2.0f;
/* min_white_black_diff 20 instead of upstream's 5: low-contrast components
 * (noise, texture) no longer become quad candidates; measured on the board
 * 107 -> 56 ms per frame with no tag in view (Task 6 report). A printed or
 * on-screen tag has far more black/white contrast. */
static mxt_tuning_t s_tune = { 0, 20, 10, 5, 1, 10 };

#define HALF_W (MXT_FRAME_W / 2)
#define HALF_H (MXT_FRAME_H / 2)

static void apply_tuning(void) {
  s_td->quad_decimate = s_tune.half_res ? 1.0f : s_decimate;
  s_td->refine_edges = s_tune.refine_edges != 0;
  s_td->qtp.min_white_black_diff = s_tune.min_white_black_diff;
  s_td->qtp.max_nmaxima = s_tune.max_nmaxima;
  s_td->qtp.min_cluster_pixels = s_tune.min_cluster_pixels;
  s_td->qtp.max_line_fit_mse = (float)s_tune.max_line_fit_mse;
}

void mxt_get_tuning(mxt_tuning_t *out) { *out = s_tune; }

bool mxt_set_tuning(const mxt_tuning_t *t) {
  if (t->max_nmaxima == 0 || t->max_line_fit_mse == 0) return false;
  s_tune = *t;
  s_tune.half_res = t->half_res ? 1 : 0;
  s_tune.refine_edges = t->refine_edges ? 1 : 0;
  if (s_td) apply_tuning();
  return true;
}
static mxt_profile_t s_prof;
static uint16_t s_lut_hi[256], s_lut_lo[256]; /* gray conversion, see to_gray() */
static bool s_lut_ready;
static void init_luts(void);

/* Upstream image_u8_decimate() only implements exactly 1.5 or an integer
 * factor (it truncates anything else), while quad corners are rescaled by the
 * raw value, so e.g. 2.5 would yield wrong coordinates. Only 1, 1.5, 2, 3, 4
 * are accepted (exact comparisons on purpose; NaN/inf fail them all). */
bool mxt_set_quad_decimate(float decimate) {
  if (!(decimate == 1.0f || decimate == 1.5f || decimate == 2.0f || decimate == 3.0f ||
        decimate == 4.0f))
    return false;
  s_decimate = decimate;
  if (s_td) apply_tuning();
  return true;
}

void mxt_deinit(void) {
  if (s_td) apriltag_detector_destroy(s_td); /* also frees the decode table */
  if (s_tf) tag36h11_destroy(s_tf);
  if (s_gray) image_u8_destroy(s_gray);
  free(s_half); /* heap_caps_malloc'd blocks are released by free() on ESP-IDF */
  s_half = NULL;
  s_td = NULL;
  s_tf = NULL;
  s_gray = NULL;
}

bool mxt_init(void) {
  if (s_td) return true;
  if (!s_lut_ready) init_luts();
  s_tf = tag36h11_create();
  s_td = apriltag_detector_create();
  s_gray = image_u8_create(MXT_FRAME_W, MXT_FRAME_H);
  if (!s_tf || !s_td || !s_gray) {
    mxt_deinit();
    return false;
  }
  apriltag_detector_add_family_bits(s_td, s_tf, MAX_HAMMING);
  if (s_tf->impl == NULL) { /* decode table allocation failed */
    mxt_deinit();
    return false;
  }
  s_td->quad_sigma = 0.0f;
  s_td->decode_sharpening = 0.25;
  s_td->nthreads = 1;
  apply_tuning();
  return true;
}

/* RGB565 big-endian -> 8-bit luma, Y = (77R + 150G + 29B) >> 8 on channels
 * expanded to 8 bits (weights sum to 256, so white stays 255).
 * Fast path: with bytes hi (RRRRRGGG) and lo (GGGBBBBB), the expanded
 * channels split into non-overlapping bit fields of one byte each:
 *   r8 = f(hi), b8 = f(lo), g8 = ((hi&7)<<5 | (hi&7)>>1) + ((lo>>5)<<2)
 * so 77r8 + 150g8 + 29b8 = s_lut_hi[hi] + s_lut_lo[lo] exactly. */
static void init_luts(void) {
  for (unsigned b = 0; b < 256; b++) {
    unsigned r5 = b >> 3, gh = b & 7u;
    s_lut_hi[b] = (uint16_t)(77u * ((r5 << 3) | (r5 >> 2)) + 150u * ((gh << 5) | (gh >> 1)));
    unsigned b5 = b & 0x1Fu, gl = b >> 5;
    s_lut_lo[b] = (uint16_t)(150u * (gl << 2) + 29u * ((b5 << 3) | (b5 >> 2)));
  }
  s_lut_ready = true;
}

static inline unsigned luma2(uint8_t hi, uint8_t lo) { return (s_lut_hi[hi] + s_lut_lo[lo]) >> 8; }

uint8_t mxt_luma_be(uint16_t px) {
  if (!s_lut_ready) init_luts();
  const uint8_t *b = (const uint8_t *)&px; /* memory order: hi, lo (big-endian pixel) */
  return (uint8_t)luma2(b[0], b[1]);
}

static void to_gray(const uint16_t *frame) {
  const uint32_t *src = (const uint32_t *)frame; /* 2 pixels per 32-bit read */
  uint8_t *dst = s_gray->buf;
  for (int y = 0; y < MXT_FRAME_H; y++, dst += s_gray->stride) {
    for (int x = 0; x < MXT_FRAME_W; x += 2) {
      uint32_t w = *src++; /* little-endian load of bytes hi0 lo0 hi1 lo1 */
      dst[x] = (uint8_t)luma2((uint8_t)w, (uint8_t)(w >> 8));
      dst[x + 1] = (uint8_t)luma2((uint8_t)(w >> 16), (uint8_t)(w >> 24));
    }
  }
}

/* Same luma, 2x2 mean -> 160x120. */
static void to_gray_half(const uint16_t *frame) {
  for (int y = 0; y < HALF_H; y++) {
    const uint32_t *a = (const uint32_t *)(frame + (2 * y) * MXT_FRAME_W), *b = a + MXT_FRAME_W / 2;
    uint8_t *dst = s_half + y * HALF_W;
    for (int x = 0; x < HALF_W; x++) {
      uint32_t wa = a[x], wb = b[x];
      unsigned sum = luma2((uint8_t)wa, (uint8_t)(wa >> 8)) + luma2((uint8_t)(wa >> 16), (uint8_t)(wa >> 24)) +
                     luma2((uint8_t)wb, (uint8_t)(wb >> 8)) + luma2((uint8_t)(wb >> 16), (uint8_t)(wb >> 24));
      dst[x] = (uint8_t)((sum + 2) >> 2);
    }
  }
}

static int cmp_margin_desc(const void *a, const void *b) {
  const apriltag_detection_t *da = *(apriltag_detection_t *const *)a;
  const apriltag_detection_t *db = *(apriltag_detection_t *const *)b;
  if (da->decision_margin > db->decision_margin) return -1;
  if (da->decision_margin < db->decision_margin) return 1;
  return da->id - db->id;
}

static int16_t to_i16(double v) {
  if (v > 32767.0) return 32767;
  if (v < -32768.0) return -32768;
  return (int16_t)lround(v);
}

static void put_i16(uint8_t *p, int16_t v) {
  p[0] = (uint8_t)((uint16_t)v & 0xFFu);
  p[1] = (uint8_t)((uint16_t)v >> 8);
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* scale: 2 in half-res mode (detections are in 160x120 coordinates). */
static void fill_record(const apriltag_detection_t *dd, double scale, mxp_result_t *r) {
  apriltag_detection_t s = *dd, *d = &s;
  for (int k = 0; k < 4; k++) {
    d->p[k][0] *= scale;
    d->p[k][1] *= scale;
  }
  d->c[0] *= scale;
  d->c[1] *= scale;
  double minx = d->p[0][0], maxx = minx, miny = d->p[0][1], maxy = miny;
  for (int k = 1; k < 4; k++) {
    minx = fmin(minx, d->p[k][0]);
    maxx = fmax(maxx, d->p[k][0]);
    miny = fmin(miny, d->p[k][1]);
    maxy = fmax(maxy, d->p[k][1]);
  }
  long x0 = lround(clampd(minx, 0, MXT_FRAME_W)), x1 = lround(clampd(maxx, 0, MXT_FRAME_W));
  long y0 = lround(clampd(miny, 0, MXT_FRAME_H)), y1 = lround(clampd(maxy, 0, MXT_FRAME_H));

  r->id = (uint16_t)d->id;
  r->x = to_i16(d->c[0]);
  r->y = to_i16(d->c[1]);
  r->w = (uint16_t)(x1 - x0);
  r->h = (uint16_t)(y1 - y0);
  r->confidence = (uint8_t)lround(clampd(d->decision_margin, 0.0, 100.0));

  /* Output corners in the OpenCV/ArUco order TL, TR, BR, BL (clockwise on
   * screen from the tag's top-left). OpenCV's DICT_APRILTAG_36h11 marker is
   * the AprilRobotics image turned 180 degrees, and upstream's p[] runs
   * BL, BR, TR, TL of the AprilRobotics image; in OpenCV terms that is
   * TR, TL, BL, BR, so OpenCV's TL, TR, BR, BL = upstream p[1], p[0], p[3], p[2]
   * (confirmed on the board against OpenCV on the same frames). */
  static const int order[4] = { 1, 0, 3, 2 };
  const double *tl = d->p[order[0]], *tr = d->p[order[1]];
  /* Rotation: direction of the top edge TL -> TR; upright reads 0, turned
   * clockwise on screen reads positive. */
  double deg = atan2(tr[1] - tl[1], tr[0] - tl[0]) * (180.0 / 3.14159265358979323846);
  r->extra_len = 18;
  put_i16(r->extra, to_i16(clampd(deg * 100.0, -18000.0, 18000.0)));
  for (int k = 0; k < 4; k++) {
    put_i16(r->extra + 2 + 4 * k, to_i16(d->p[order[k]][0]));
    put_i16(r->extra + 4 + 4 * k, to_i16(d->p[order[k]][1]));
  }
}

static int name_is(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* Stage durations from upstream's timeprofile (stamps are cumulative). */
static void collect_profile(void) {
  timeprofile_t *tp = s_td->tp;
  int64_t last = tp->utime;
  for (int i = 0; i < zarray_size(tp->stamps); i++) {
    struct timeprofile_entry *e;
    zarray_get_volatile(tp->stamps, i, &e);
    uint32_t d = (uint32_t)(e->utime - last);
    last = e->utime;
    if (name_is(e->name, "decimate")) s_prof.decimate_us += d;
    else if (name_is(e->name, "threshold")) s_prof.threshold_us += d;
    else if (name_is(e->name, "unionfind")) s_prof.unionfind_us += d;
    else if (name_is(e->name, "make clusters")) s_prof.clusters_us += d;
    else if (name_is(e->name, "fit quads to clusters")) s_prof.fitquads_us += d;
    else if (name_is(e->name, "decode+refinement")) s_prof.decode_us += d;
    else s_prof.other_us += d;
  }
  s_prof.nquads = s_td->nquads;
}

void mxt_last_profile(mxt_profile_t *out) { *out = s_prof; }

uint8_t mxt_detect(const uint16_t *frame, mxp_result_t *out, uint8_t cap, uint32_t *elapsed_us) {
  uint64_t t0 = now_us();
  uint8_t n = 0;
  bool half = false;
  memset(&s_prof, 0, sizeof(s_prof));
  if (cap > MXP_MAX_RESULTS) cap = MXP_MAX_RESULTS;
  if (s_td && frame && out && cap > 0) {
    zarray_t *dets;
    if (s_tune.half_res && !s_half) s_half = alloc_hot((size_t)HALF_W * HALF_H); /* first use */
    /* Without the buffer: full resolution (quad_decimate is 1 then: slower, correct). */
    half = s_tune.half_res && s_half;
    if (half) {
      to_gray_half(frame);
      s_prof.gray_us = (uint32_t)(now_us() - t0);
      image_u8_t half = { HALF_W, HALF_H, HALF_W, s_half };
      dets = apriltag_detector_detect(s_td, &half);
    } else {
      to_gray(frame);
      s_prof.gray_us = (uint32_t)(now_us() - t0);
      dets = apriltag_detector_detect(s_td, s_gray);
    }
    collect_profile();
    if (dets) {
      zarray_sort(dets, cmp_margin_desc);
      for (int i = 0; i < zarray_size(dets) && n < cap; i++) {
        apriltag_detection_t *d;
        zarray_get(dets, i, &d);
        if (d->hamming > MAX_HAMMING) continue;
        fill_record(d, half ? 2.0 : 1.0, &out[n++]);
      }
      apriltag_detections_destroy(dets);
    }
  }
  s_prof.total_us = (uint32_t)(now_us() - t0);
  if (elapsed_us) *elapsed_us = s_prof.total_us;
  return n;
}
