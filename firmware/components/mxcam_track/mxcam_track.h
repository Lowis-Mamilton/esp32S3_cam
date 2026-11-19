#ifndef MXCAM_TRACK_H
#define MXCAM_TRACK_H

#include <stdbool.h>
#include <stdint.h>

#include "mxcam_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Object tracking (function 5, ADR-0018) on a 320x240 RGB565 big-endian frame.
 * One target, id 1. LEARN keeps a grey template (the box on the 160x120
 * mxg_half image, area-averaged down to a long side <= 32, short side >= 8), a
 * 32-bin colour histogram and the learned box / mean colour. Each frame runs
 * NCC around the last position (window = box centre +- (box/2 + long side)) at
 * scales 0.9 / 1.0 / 1.1 (1.0 only when cfg->scale is 0), coarse step 2 then
 * +-1 refinement; confidence = round(max(0, NCC) * 100); below the threshold
 * the target is lost and there is no record. While lost the last position is
 * searched first; if that fails and reacquire = 1, a histogram back-projection
 * over the whole frame proposes a position that NCC must confirm. The live template adapts by adapt% when the score is >=
 * max(threshold, 70) and the patch still matches the learned template; it
 * returns to the learned template when the target is lost. All coordinates
 * are 320x240 real-image coordinates (ADR-0010). */

#define MXK_TMPL_MAX 32
#define MXK_TMPL_MIN 8
#define MXK_HIST_BINS 32 /* 12 hues x 2 saturations (S 0.2-0.5, >= 0.5) + 8 brightness bins (S < 0.2) */
#define MXK_MIN_BOX 16   /* 320 px */
#define MXK_MIN_STD 6    /* grey standard deviation lower bound */
/* Per-frame NCC multiply-accumulate budget (coarse grid + refine; a lost +
 * reacquire frame splits it between its two searches). ~26 ms at ~5.3
 * cycles/MAC on ESP32-S3 @ 240 MHz from the generated inner loop, but that
 * excludes the per-position ncc_score (int64, sqrtf, divide), resampling and
 * cache misses: a review estimates 33-38 ms per frame for 48-120 px boxes.
 * Task 5 measures and tunes this. Above it the plan's fallbacks apply in
 * order: (1) only one extra scale per frame (0.9 / 1.1 alternating), (2)
 * coarse step 3. */
#define MXK_MAC_BUDGET 1200000u

typedef struct {
  uint8_t tw, th;                            /* template size 8..32 */
  uint16_t box_w, box_h;                     /* learned box (320 px) */
  int16_t cx, cy;                            /* learned centre (320 px) */
  uint8_t rgb[3];                            /* mean colour in the box (GET_LEARNED) */
  uint16_t hist[MXK_HIST_BINS];              /* normalised to a sum of 65535 */
  uint8_t tmpl[MXK_TMPL_MAX * MXK_TMPL_MAX]; /* first tw*th bytes used, rest 0 */
} mxk_model_t;

typedef struct { /* owned by the caller; tracker state, never saved */
  uint8_t live[MXK_TMPL_MAX * MXK_TMPL_MAX];
  int16_t cx, cy;
  uint16_t w, h; /* current box (320 px) */
  uint8_t lost;
  uint16_t lost_frames;
  uint8_t tick; /* alternates the extra scale under fallback (1) */
} mxk_state_t;

/* Region is (x, y) top-left + size in 320 px, clipped to the frame. Returns
 * false and leaves *out untouched when the clipped box is smaller than
 * MXK_MIN_BOX on either side or the template has no texture (grey std <
 * MXK_MIN_STD). Not reentrant (static PSRAM work buffers). */
bool mxk_learn(const uint16_t *frame, uint16_t x, uint16_t y, uint16_t w, uint16_t h, mxk_model_t *out);
/* tw/th in range, box >= MXK_MIN_BOX and within the frame, cx/cy inside the
 * frame (for the store's deserialiser). */
bool mxk_model_valid(const mxk_model_t *m);
/* live = learned template, box = learned box, not lost. */
void mxk_reset(mxk_state_t *st, const mxk_model_t *m);
/* Returns 0 or 1 record (id 1, centre + box, confidence, extra_len 0); with 0
 * out is untouched. Not reentrant (static PSRAM work buffers). */
uint8_t mxk_track(const uint16_t *frame, const mxk_model_t *m, const mxp_track_config_t *cfg,
                  mxk_state_t *st, mxp_result_t *out);

#ifdef __cplusplus
}
#endif

#endif
