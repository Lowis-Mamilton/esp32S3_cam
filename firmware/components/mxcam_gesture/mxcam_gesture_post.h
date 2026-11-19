#ifndef MXCAM_GESTURE_POST_H
#define MXCAM_GESTURE_POST_H

#include <stdbool.h>
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

/* ---- Detection skipping (ADR-0019 addendum, "reuse hand boxes") ----
 * Hand detection (~170 ms) costs about twice a classification (~89 ms per
 * hand). After a frame where hands were found, the next frames reuse those
 * boxes (grown by MXH_EXPAND_PCT around their centre, so a moving hand stays
 * in the crop) and only run the classifier. Detection runs again when:
 *   - nothing is tracked (no hands last time, or a reset),
 *   - MXH_DETECT_EVERY frames have passed since the last detection,
 *   - max_hands asks for more hands than are tracked,
 *   - the gesture config (any of the three fields) changed,
 *   - a reused crop is classified as no_hand (mxh_hand_lost): the caller
 *     then detects again on that same frame.
 * The caller resets the tracker on function switch, heap-guard skip and
 * stale frames. Records on reuse frames carry the boxes (and, for id 0, the
 * hand score) of the last detection: the box lags a moving hand by up to
 * MXH_DETECT_EVERY - 1 frames. */

/* Re-run hand detection at least every this many frames (1 = every frame). */
#define MXH_DETECT_EVERY 3
/* Reuse crop: each side of the last detected box grown by this percentage. */
#define MXH_EXPAND_PCT 25

typedef struct {
  mxh_raw_box_t hands[MXH_MAX_HANDS]; /* last detection's boxes (clipped), as reported */
  uint8_t n;                          /* hands tracked; 0 = nothing to reuse */
  uint8_t age;                        /* frames since that detection (saturates at 255) */
  /* Config of that detection (max_hands clamped to 1..MXH_MAX_HANDS). */
  uint8_t hand_threshold, gesture_threshold, max_hands;
} mxh_track_t;

/* Forget the tracked hands (next frame detects). NULL is a no-op. */
void mxh_track_reset(mxh_track_t *t);

/* true when this frame must run hand detection (rules above). NULL -> true;
 * every <= 1 -> always true. */
bool mxh_track_need_detect(const mxh_track_t *t, uint8_t hand_threshold, uint8_t gesture_threshold,
                           uint8_t max_hands, uint8_t every);

/* After a detection frame: remember hands[0..n) (at most MXH_MAX_HANDS) and
 * the config, age 0. n == 0, hands == NULL or any_lost (a crop on this frame
 * was classified no_hand) leaves nothing tracked, so the next frame detects
 * again (same cost as without skipping). NULL t is a no-op. */
void mxh_track_detected(mxh_track_t *t, const mxh_raw_box_t *hands, uint8_t n, bool any_lost,
                        uint8_t hand_threshold, uint8_t gesture_threshold, uint8_t max_hands);

/* After a frame that reused the boxes: age + 1 (saturating). NULL is a no-op. */
void mxh_track_reused(mxh_track_t *t);

/* Reuse crop for a tracked box: each side grown by MXH_EXPAND_PCT (rounded;
 * an odd growth puts the extra pixel right / bottom) around the same
 * centre, then clipped to the frame. score copied. false (out untouched)
 * when b or out is NULL, b is empty / inverted, or the clipped crop is
 * narrower or lower than MXH_MIN_HAND_PX. */
bool mxh_expand_box(const mxh_raw_box_t *b, mxh_raw_box_t *out);

/* true when the classifier's top-1 class says there is no hand in the crop
 * ("no_hand", or no result at all). "no_gesture" is still a hand. */
bool mxh_hand_lost(const char *cat_name);

/* TFT label name: "ONE".."DISLIKE" for 1..9; 0 and anything else -> "HAND". */
const char *mxh_ascii_name(uint16_t id);

/* "<name> <confidence>", e.g. "OK 92". snprintf semantics: returns the full
 * length, writes at most cap bytes and always NUL-terminates when cap > 0. */
int mxh_label(uint16_t id, uint8_t confidence, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
