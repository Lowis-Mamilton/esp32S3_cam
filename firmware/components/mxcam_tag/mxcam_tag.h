#ifndef MXCAM_TAG_H
#define MXCAM_TAG_H

#include <stdbool.h>
#include <stdint.h>

#include "mxcam_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AprilTag tag36h11 detection on a 320x240 RGB565 big-endian frame (same
 * layout as mxcam_color / mxcam_overlay). Wraps the vendored AprilRobotics
 * library (components/apriltag, ADR-0008).
 *
 * Result record (protocol.md 7):
 *   id          tag ID (0..586)
 *   x, y        tag centre (det->c, rounded)
 *   w, h        axis-aligned bounding box of the 4 corners, clipped to the frame
 *   confidence  decision_margin rounded and clamped to 0..100
 *   extra       18 bytes, little-endian i16: rotation_cdeg, then x0,y0..x3,y3
 *               (corners in upstream order, rounded, unclipped)
 * rotation_cdeg = atan2(y1 - y0, x1 - x0) in image coordinates (y down), in
 * centidegrees -18000..18000. Corner 0 -> corner 1 is the tag's bottom edge
 * running left to right, so an upright tag reads 0 and a tag turned clockwise
 * on screen reads positive.
 * Records are sorted by decision_margin (descending); detections that needed
 * more than 1 corrected bit are discarded (the decoder only builds the
 * hamming<=1 table anyway, see mxcam_tag.c). */

#define MXT_FRAME_W 320
#define MXT_FRAME_H 240

/* Creates the detector (tag36h11, quad_decimate 2, quad_sigma 0, refine_edges,
 * decode_sharpening 0.25, nthreads 1) and the grayscale work image. Idempotent.
 * false on allocation failure (nothing is left allocated). */
bool mxt_init(void);

/* Returns the number of records written to out (<= min(cap, MXP_MAX_RESULTS)).
 * 0 when mxt_init has not succeeded. elapsed_us (optional) receives the time
 * spent including the grayscale conversion. Not reentrant. */
uint8_t mxt_detect(const uint16_t *frame, mxp_result_t *out, uint8_t cap, uint32_t *elapsed_us);

/* Frees everything mxt_init allocated. Safe to call when not initialised. */
void mxt_deinit(void);

/* Changes quad_decimate for subsequent detections. Only 1, 1.5, 2, 3 and 4 are
 * accepted (upstream decimates by exactly 1.5 or an integer factor); anything
 * else, including NaN/inf, returns false and keeps the current value. Allowed
 * before mxt_init; the value survives deinit/init. Default 2. */
bool mxt_set_quad_decimate(float decimate);

#ifdef __cplusplus
}
#endif

#endif
