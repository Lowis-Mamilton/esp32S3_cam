#ifndef MXCAM_GESTURE_POST_H
#define MXCAM_GESTURE_POST_H

#include <stddef.h>
#include <stdint.h>

#include "mxcam_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pure C post-processing for mxcam_gesture (host-tested): picks the hands to
 * classify from the hand detector's boxes and turns a hand plus its gesture
 * class into a protocol ResultRecord (protocol.md, function 8) in true-image
 * (ADR-0010) 320x240 coordinates. */

#define MXH_FRAME_W 320
#define MXH_FRAME_H 240
#define MXH_MAX_HANDS 2
/* Hands narrower or lower than this (after clipping) are dropped: too small to
 * classify reliably, and it keeps esp-dl from cropping an empty region. */
#define MXH_MIN_HAND_PX 16
/* Upper bound on the boxes taken from the detector per frame (it keeps at
 * most 10 after NMS). */
#define MXH_MAX_RAW 16

/* One detector box: [x0, y0] top-left, [x1, y1] bottom-right (x1 - x0 is the
 * width, as esp-dl's result_t::box), in 320x240 coordinates; score 0..1. */
typedef struct {
  int32_t x0, y0, x1, y1;
  float score;
} mxh_raw_box_t;

/* Gesture id (MXP_GESTURE_*) of a hand_gesture_recognition class name, by
 * exact (case-sensitive) string match: "one".."five" -> 1..5, "like" 6,
 * "ok" 7, "call" 8, "dislike" 9. NULL, "no_gesture", "no_hand" and any other
 * name -> 0 (MXP_GESTURE_UNKNOWN). */
uint8_t mxh_gesture_id(const char *cat_name);

/* round(score * 100) clamped to 0..100; NaN reads 0 (same as mxf_confidence). */
uint8_t mxh_confidence(float score);

/* Clips each box to the frame, drops those whose clipped width or height is
 * < MXH_MIN_HAND_PX and those with mxh_confidence(score) < hand_threshold,
 * ranks the rest by confidence (descending; ties: larger clipped area first,
 * then input order) and writes at most clamp(max_hands, 1, MXH_MAX_HANDS)
 * clipped boxes to out. Returns the number written. */
uint8_t mxh_pick_hands(const mxh_raw_box_t *in, size_t n, uint8_t hand_threshold, uint8_t max_hands,
                       mxh_raw_box_t out[MXH_MAX_HANDS]);

/* One record for a hand (from mxh_pick_hands, i.e. already clipped; clipped
 * again here anyway) and its top-1 gesture class:
 *   id          mxh_gesture_id(cat_name) when that is >= 1 and
 *               mxh_confidence(gesture_score) >= gesture_threshold, else 0
 *   x, y        box centre (x0 + w/2, y0 + h/2); w, h the box
 *   confidence  id >= 1: mxh_confidence(gesture_score);
 *               id 0: mxh_confidence(hand->score)
 *   extra_len   0 */
void mxh_to_result(const mxh_raw_box_t *hand, const char *cat_name, float gesture_score, uint8_t gesture_threshold,
                   mxp_result_t *out);

/* TFT label name: "ONE".."DISLIKE" for 1..9; 0 and anything else -> "HAND". */
const char *mxh_ascii_name(uint16_t id);

/* "<name> <confidence>", e.g. "OK 92". snprintf semantics: returns the full
 * length, writes at most cap bytes and always NUL-terminates when cap > 0. */
int mxh_label(uint16_t id, uint8_t confidence, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
