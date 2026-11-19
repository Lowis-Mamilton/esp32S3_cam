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
 *               = the tag's top-left, top-right, bottom-right, bottom-left
 *               corners (clockwise on screen; rounded, unclipped)
 * "Top" follows the official AprilRobotics tag image (apriltag-imgs PNG). The
 * marker OpenCV/ArUco generates for DICT_APRILTAG_36h11 is that image turned
 * 180 degrees, so OpenCV's corners/angle for the same tag differ by 180.
 * rotation_cdeg = atan2(TR.y - TL.y, TR.x - TL.x) in image coordinates (y down),
 * in centidegrees -18000..18000: an upright tag reads 0 and a tag turned
 * clockwise on screen reads positive.
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

/* Detector tuning (speed vs range). Defaults: half_res 0,
 * min_white_black_diff 20 (upstream 5), max_nmaxima 10, min_cluster_pixels 5,
 * refine_edges 1, max_line_fit_mse 10. */
typedef struct {
  /* 1: convert to a 160x120 gray image (2x2 mean) and detect on it without
   * upstream decimation (quad_decimate is ignored); decode and edge refinement
   * also run at 160x120, coordinates are scaled back to 320x240. Faster and
   * smaller, but tags need ~2x the size in pixels to decode. */
  uint8_t half_res;
  uint8_t min_white_black_diff; /* threshold contrast to keep a component */
  uint8_t max_nmaxima;          /* corner candidates per cluster */
  uint8_t min_cluster_pixels;   /* smaller clusters are not fitted */
  uint8_t refine_edges;         /* 0/1 */
  uint8_t max_line_fit_mse;     /* line fit error limit (upstream float, default 10) */
} mxt_tuning_t;
void mxt_get_tuning(mxt_tuning_t *out);
/* Applies now if initialised, otherwise at mxt_init. false on 0 for
 * max_nmaxima or max_line_fit_mse. */
bool mxt_set_tuning(const mxt_tuning_t *t);

/* The gray value the detector uses for one RGB565 big-endian pixel (as stored
 * in the frame buffer): (77R + 150G + 29B) >> 8 on 8-bit expanded channels. */
uint8_t mxt_luma_be(uint16_t px);

/* Where the last mxt_detect spent its time (from upstream's timeprofile
 * stamps), for the 1 Hz log. Stages that did not run read 0. */
typedef struct {
  uint32_t gray_us;      /* RGB565 -> gray conversion (ours) */
  uint32_t decimate_us;  /* quad_decimate */
  uint32_t threshold_us; /* adaptive threshold */
  uint32_t unionfind_us; /* connected components */
  uint32_t clusters_us;  /* gradient clusters */
  uint32_t fitquads_us;  /* quad fitting */
  uint32_t decode_us;    /* decode + edge refinement */
  uint32_t other_us;     /* everything else (init, reconcile, cleanup) */
  uint32_t total_us;
  uint32_t nquads;       /* candidate quads that reached decoding */
} mxt_profile_t;
void mxt_last_profile(mxt_profile_t *out);

#ifdef __cplusplus
}
#endif

#endif
