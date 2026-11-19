#ifndef MXCAM_COLOR_H
#define MXCAM_COLOR_H

#include <stdbool.h>
#include <stdint.h>

#include "mxcam_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Colour detection on a 320x240 RGB565 big-endian frame (same layout as
 * mxcam_overlay: bytes hi,lo in memory; read a pixel as (p>>8)|(p<<8) on the
 * little-endian CPU). Matching rules mirror the PC demo (recognize.ts):
 *   hue window   = +-(tolerance * 0.6) degrees
 *   S and V each differ by <= 0.25 + tolerance/200
 *   S < 0.15 never matches. Nearest hue wins among learned colours. */

#define MXC_FRAME_W 320
#define MXC_FRAME_H 240

typedef struct {
  uint8_t id;
  uint8_t rgb[3];
} mxc_learned_t;

typedef struct {
  uint8_t tolerance; /* 0..100 */
  uint16_t min_area; /* in 320x240 pixels */
  uint8_t max_results;
} mxc_params_t;

/* Finds 4-connected blobs of learned colours on a 2x2-subsampled (top-left
 * pixel of each block) 160x120 grid. Results: id = learned id, x/y = bbox
 * centre, w/h = bbox size (320x240 units, clipped), confidence 255, extra_len 0,
 * sorted by area descending, at most min(max_results, cap, MXP_MAX_RESULTS).
 * Colours not learned are never reported. Returns the result count.
 * Not reentrant: uses a static work buffer (see RAM cost in mxcam_color.c). */
uint8_t mxc_detect(const uint16_t *frame, const mxc_learned_t *learned, uint8_t n_learned,
                   const mxc_params_t *p, mxp_result_t *out, uint8_t cap);

/* Average RGB of the saturated (S>=0.15, V>=0.15) pixels in the region
 * (320x240 top-left coords, clamped to the frame). false if fewer than 25% of
 * the region's pixels qualify. */
bool mxc_sample(const uint16_t *frame, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                uint8_t rgb_out[3]);

/* Same tolerance window as detection; false when either colour is grey. */
bool mxc_same_color(const uint8_t a[3], const uint8_t b[3], uint8_t tolerance);

/* One frame pixel (RGB565 big-endian, as in the frame) against a reference
 * colour: exactly mxc_same_color(expanded pixel, rgb, tolerance). Grey pixels
 * are rejected with integer math before any float work (used per pixel by the
 * line function's learned-colour mode). Stateless and reentrant. */
bool mxc_match_px(uint16_t px_be, const uint8_t rgb[3], uint8_t tolerance);

#ifdef __cplusplus
}
#endif

#endif
