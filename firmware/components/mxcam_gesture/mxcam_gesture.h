#ifndef MXCAM_GESTURE_H
#define MXCAM_GESTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "mxcam_gesture_post.h"
#include "mxcam_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Gesture recognition (function 8) on a 320x240 RGB565 big-endian true-image
 * frame (ADR-0010; same layout as mxcam_face). Wraps Espressif's hand_detect
 * (espdet_pico 224x224, finds hands) and hand_gesture_recognition's
 * HandGestureCls (MobileNetV2 0.5, classifies each hand's crop) on esp-dl;
 * models in flash rodata, copied to PSRAM at load (ADR-0019).
 *
 * One record per hand (mxh_to_result in mxcam_gesture_post.h): id = gesture
 * id (MXP_GESTURE_*, 0 = hand with no recognised gesture), box centre / size,
 * confidence = gesture score (id >= 1) or hand score (id 0), extra_len 0.
 * Hands below hand_threshold or smaller than MXH_MIN_HAND_PX are dropped; only
 * the best max_hands (1..2) are classified (time bound = 1 detection +
 * max_hands classifications). With a tracker (mxh_detect_tracked) most
 * frames skip the detection and only classify the last boxes. */

typedef struct {
  uint32_t total_us, detect_us, classify_us;
  uint8_t raw_hands;                /* boxes the detector returned (before filtering) */
  uint8_t hand_conf[MXH_MAX_HANDS]; /* hand detection score x 100 of each output record (benchmark / stats) */
  uint8_t detected;                 /* 1: hand detection ran on this frame; 0: boxes reused */
  uint8_t lost;                     /* 1: a reused crop said no_hand, so this frame detected again */
} mxh_stats_t;

#define MXH_SKIPPED (-1)

/* Heap guard (same as mxcam_face): esp-dl allocates while it runs and aborts
 * on a failed allocation (no C++ exceptions), so mxh_detect skips the frame
 * (MXH_SKIPPED, a warning logged at most once per second) when internal free
 * < MXH_GUARD_INTERNAL_FREE or the largest free PSRAM block <
 * MXH_GUARD_PSRAM_BLOCK. Residual risk as in mxcam_face.h: the check is not
 * atomic with the run. */
#define MXH_GUARD_INTERNAL_FREE (32u * 1024u)
#define MXH_GUARD_PSRAM_BLOCK (256u * 1024u)

/* Loads both models (lazy_load off) and builds the detector and classifier.
 * Idempotent. false (nothing left allocated) when there is not enough PSRAM /
 * internal RAM or a model fails to build; the rest of the firmware keeps
 * working without function 8. Call it at boot, right after mxf_init and
 * before the other tasks start: while the models load, plain malloc is
 * steered to PSRAM for every task (ADR-0011, ADR-0019). */
bool mxh_init(void);

/* Returns the number of records written to out
 * (<= min(max_hands, cap, MXP_MAX_RESULTS)), or MXH_SKIPPED when mxh_init has
 * not succeeded, frame/out is NULL or the heap guard tripped (the caller must
 * then treat the frame as "no new result", not as "no hands"). 0 when cap is
 * 0. hand_threshold / gesture_threshold are 0..100 (x 100 of the scores);
 * max_hands is clamped to 1..MXH_MAX_HANDS. stats may be NULL.
 * Pipeline task only; not reentrant. */
int mxh_detect(const uint16_t *frame, uint8_t hand_threshold, uint8_t gesture_threshold, uint8_t max_hands,
               mxp_result_t *out, uint8_t cap, mxh_stats_t *stats);

/* As mxh_detect, but with detection skipping (mxcam_gesture_post.h,
 * "Detection skipping"): when trk says so, the frame only classifies the
 * tracked boxes (grown by MXH_EXPAND_PCT) and reports them with the last
 * detection's boxes; a crop classified no_hand makes this same frame detect
 * again. trk is updated (reset on MXH_SKIPPED). trk NULL = mxh_detect. The
 * caller resets trk (mxh_track_reset) on function switch and stale frames;
 * config changes are noticed here. */
int mxh_detect_tracked(const uint16_t *frame, uint8_t hand_threshold, uint8_t gesture_threshold, uint8_t max_hands,
                       mxh_track_t *trk, mxp_result_t *out, uint8_t cap, mxh_stats_t *stats);

/* Frees the detector and classifier. Safe to call when not initialised. */
void mxh_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
