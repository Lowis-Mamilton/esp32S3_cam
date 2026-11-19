#include "mxcam_tag.h"

#include <math.h>
#include <stddef.h>

#include "apriltag.h"
#include "common/image_u8.h"
#include "common/zarray.h"
#include "tag36h11.h"

#ifdef ESP_PLATFORM
#include "esp_timer.h"
static uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }
#else
#include <time.h>
/* Host only (timing is informational there): processor time, portable C99. */
static uint64_t now_us(void) { return (uint64_t)clock() * 1000000u / CLOCKS_PER_SEC; }
#endif

/* Memory: upstream mallocs freely (detector, per-frame zarrays, unionfind,
 * decimated image ...). On target, plain malloc goes to PSRAM for anything
 * above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (sdkconfig.defaults), so the big
 * buffers land in PSRAM without touching upstream code.
 *
 * Decode table: apriltag_detector_add_family() builds the hamming<=2 table,
 * which for tag36h11 is 587 * (1 + 36 + 36*35) * 3 entries * 16 B ~= 36 MB.
 * We build the hamming<=1 table instead (587 * 37 * 3 * 16 B ~= 1.0 MB, PSRAM)
 * and, consistently, report nothing that needed more than 1 corrected bit. */
#define MAX_HAMMING 1

static apriltag_detector_t *s_td;
static apriltag_family_t *s_tf;
static image_u8_t *s_gray;
static float s_decimate = 2.0f;

void mxt_set_quad_decimate(float decimate) {
  s_decimate = decimate >= 1.0f ? decimate : 1.0f; /* also maps NaN to 1 */
  if (s_td) s_td->quad_decimate = s_decimate;
}

void mxt_deinit(void) {
  if (s_td) apriltag_detector_destroy(s_td); /* also frees the decode table */
  if (s_tf) tag36h11_destroy(s_tf);
  if (s_gray) image_u8_destroy(s_gray);
  s_td = NULL;
  s_tf = NULL;
  s_gray = NULL;
}

bool mxt_init(void) {
  if (s_td) return true;
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
  s_td->quad_decimate = s_decimate;
  s_td->quad_sigma = 0.0f;
  s_td->refine_edges = true;
  s_td->decode_sharpening = 0.25;
  s_td->nthreads = 1;
  return true;
}

/* RGB565 big-endian -> 8-bit luma, Y = (77R + 150G + 29B) >> 8 on channels
 * expanded to 8 bits (weights sum to 256, so white stays 255). */
static void to_gray(const uint16_t *frame) {
  for (int y = 0; y < MXT_FRAME_H; y++) {
    const uint16_t *src = frame + y * MXT_FRAME_W;
    uint8_t *dst = s_gray->buf + y * s_gray->stride;
    for (int x = 0; x < MXT_FRAME_W; x++) {
      uint16_t v = (uint16_t)((src[x] >> 8) | (src[x] << 8));
      unsigned r5 = v >> 11, g6 = (v >> 5) & 0x3Fu, b5 = v & 0x1Fu;
      unsigned r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
      dst[x] = (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
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

static void fill_record(const apriltag_detection_t *d, mxp_result_t *r) {
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

  /* Corner 0 -> corner 1: the tag's bottom edge, left to right when upright. */
  double deg = atan2(d->p[1][1] - d->p[0][1], d->p[1][0] - d->p[0][0]) *
               (180.0 / 3.14159265358979323846);
  r->extra_len = 18;
  put_i16(r->extra, to_i16(clampd(deg * 100.0, -18000.0, 18000.0)));
  for (int k = 0; k < 4; k++) {
    put_i16(r->extra + 2 + 4 * k, to_i16(d->p[k][0]));
    put_i16(r->extra + 4 + 4 * k, to_i16(d->p[k][1]));
  }
}

uint8_t mxt_detect(const uint16_t *frame, mxp_result_t *out, uint8_t cap, uint32_t *elapsed_us) {
  uint64_t t0 = now_us();
  uint8_t n = 0;
  if (cap > MXP_MAX_RESULTS) cap = MXP_MAX_RESULTS;
  if (s_td && frame && out && cap > 0) {
    to_gray(frame);
    zarray_t *dets = apriltag_detector_detect(s_td, s_gray);
    if (dets) {
      zarray_sort(dets, cmp_margin_desc);
      for (int i = 0; i < zarray_size(dets) && n < cap; i++) {
        apriltag_detection_t *d;
        zarray_get(dets, i, &d);
        if (d->hamming > MAX_HAMMING) continue;
        fill_record(d, &out[n++]);
      }
      apriltag_detections_destroy(dets);
    }
  }
  if (elapsed_us) *elapsed_us = (uint32_t)(now_us() - t0);
  return n;
}
