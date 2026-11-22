#ifndef MXCAM_QR_H
#define MXCAM_QR_H

#include <stdbool.h>
#include <stdint.h>

#include "mxcam_qr_post.h"

#ifdef __cplusplus
extern "C" {
#endif

/* QR code detection + decoding (function 6, ADR-0020) on a 320x240 RGB565
 * big-endian true-image frame (ADR-0010; same layout as mxcam_face). Wraps
 * Espressif's quirc port: full resolution, every frame, no quirc_flip retry.
 * Records come from mxq_to_result (mxcam_qr_post.h); the pipeline maps the
 * payload to a learned id. */

#define MXQ_SKIPPED (-1)
/* quirc may find up to 64 grids; at most this many are extracted + decoded
 * per frame (in quirc's order) to bound the time (~3 ms per decode). */
#define MXQ_MAX_GRIDS 8

typedef struct {
  uint32_t total_us, gray_us, identify_us, decode_us;
  uint8_t grids;   /* grids quirc found (quirc_count, saturating at 255) */
  uint8_t decoded; /* grids that decoded (before the MXQ_MAX_CODES limit) */
  uint8_t failed;  /* grids that were tried and failed to decode */
} mxq_stats_t;

/* Allocates the quirc decoder for 320x240 (~92 KB: 76.8 KB grey image,
 * ~12 KB struct quirc, ~2.5 KB flood-fill work area) with malloc steered to
 * PSRAM (heap_caps_malloc_extmem_enable(0), as ADR-0011) and keeps it for
 * good. Idempotent. Logs "qr ready (internal A -> B, PSRAM C -> D)". Call it
 * at boot, after mxh_init and before the other tasks are created (the
 * malloc switch is global). false (nothing left allocated): function 6 must
 * not be listed in GET_INFO. */
bool mxq_init(void);

/* Detects and decodes the QR codes of one frame and writes the decoded ones,
 * largest first (mxq_sort_limit), to out. Returns their number
 * (0..MXQ_MAX_CODES), or MXQ_SKIPPED when mxq_init has not succeeded or
 * frame / out is NULL. Grids that fail to decode are not reported. stats
 * may be NULL.
 * Stack: quirc_decode keeps an ~8.9 KB work struct on the caller's stack
 * (quirc's README: ~10 KiB for a small code), so call it with >= 12 KB of
 * free stack. Pipeline task only; not reentrant. */
int mxq_detect(const uint16_t *frame, mxq_code_t out[MXQ_MAX_CODES], mxq_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
