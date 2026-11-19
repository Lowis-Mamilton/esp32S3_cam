#ifndef MXCAM_FACE_H
#define MXCAM_FACE_H

#include <stdbool.h>
#include <stdint.h>

#include "mxcam_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Face detection (function 3) on a 320x240 RGB565 big-endian true-image frame
 * (ADR-0010; same layout as mxcam_color / mxcam_tag). Wraps Espressif's
 * human_face_detect (MSR + MNP s8 models, esp-dl), models in flash rodata,
 * tensors in PSRAM.
 *
 * Result record (protocol.md 7):
 *   id          0
 *   x, y        box centre (x0 + w/2, y0 + h/2)
 *   w, h        face box, clipped to the frame
 *   confidence  model score x 100, rounded, 0..100
 *   extra_len   0
 * Records below threshold are dropped; the rest are sorted by confidence
 * (descending; ties: larger box first). */

/* Loads both models and builds the detector (~80 ms). Idempotent. false
 * (nothing left allocated) when there is not enough PSRAM / internal RAM or a
 * model fails to build; the rest of the firmware keeps working without face
 * detection. Call it at boot, before the other tasks start: while the models
 * load, plain malloc is steered to PSRAM for every task. */
bool mxf_init(void);

/* Returns the number of records written to out
 * (<= min(max_results, cap, MXP_MAX_RESULTS)). threshold is 0..100.
 * 0 at once when mxf_init has not succeeded, frame/out is NULL or
 * max_results / cap is 0. elapsed_us (optional) receives the time spent in
 * detection and post-processing (44-62 ms measured on the board for 0-2 faces).
 *
 * Heap guard: esp-dl allocates while it runs (internal RAM < 1 KB, PSRAM
 * blocks for its tensors) and aborts on a failed allocation (no C++
 * exceptions). So the detection is skipped (0 results, a warning logged at
 * most once per second) when internal free < MXF_GUARD_INTERNAL_FREE or the
 * largest free PSRAM block < MXF_GUARD_PSRAM_BLOCK. Residual risk: the check
 * is not atomic with the run, so another task allocating in between, or heap
 * fragmentation the check does not see (e.g. no internal block large enough
 * although enough is free), can still make esp-dl abort; the margins are far
 * above what one run needs.
 *
 * Pipeline task only; not reentrant. */
#define MXF_GUARD_INTERNAL_FREE (32u * 1024u)
#define MXF_GUARD_PSRAM_BLOCK (256u * 1024u)
uint8_t mxf_detect(const uint16_t *frame, uint8_t threshold, uint8_t max_results, mxp_result_t *out, uint8_t cap,
                   uint32_t *elapsed_us);

/* Frees the detector. Safe to call when not initialised. */
void mxf_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
