#ifndef MXCAM_MOTION_H
#define MXCAM_MOTION_H

#include <stdint.h>

#include "mxcam_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Motion detection (function 12, ADR-0021) on a 320x240 RGB565 big-endian
 * frame, for a camera that does not move. The mxg_half grey image is averaged
 * again 2x2 into 80x60 cells; the background is an exponential moving
 * average per cell (u16, x16 fixed point, bg += (cur*16 - bg) >> MXM_BG_SHIFT
 * every frame, ~0.8 s at 10 fps). Exposure compensation: the median of
 * (cur - bg) over all cells is subtracted before comparing a cell's
 * difference with cfg->threshold. More than MXM_GLOBAL_PCT % of the cells
 * changed (light switched, camera moved) -> the background is reset to this
 * frame, which and the next 2 report nothing. Changed cells are dilated 3x3
 * and grouped into 8-connected regions; a region is reported when the long
 * side of the bounding box of its changed cells (x4 = 320 px) is at least
 * cfg->min_size. Objects that stop blend into the background in ~1-2 s. */

#define MXM_W 80
#define MXM_H 60
#define MXM_BG_SHIFT 3
#define MXM_WARMUP 3       /* frames after mxm_reset that only build the background */
#define MXM_GLOBAL_PCT 50  /* more changed cells than this -> global change */
#define MXM_GLOBAL_SKIP 2  /* frames after a global change that only update the background */

typedef struct {
  uint8_t global_reset;   /* this frame was a global change (background reset) */
  uint16_t changed_cells; /* changed 80x60 cells before dilation (0 in warm-up frames) */
} mxm_stats_t;

/* The next frame rebuilds the background and the warm-up starts again. Call
 * when the function changes to 12 (the state at boot is the same as after a
 * reset). */
void mxm_reset(void);
/* Writes 0..min(cap, cfg->max_results) records, largest bounding box first:
 * id 0, x/y/w/h = bounding box of the region's changed cells (320x240),
 * confidence = round(100 * changed cells / cells in that box), extra_len 0.
 * The first MXM_WARMUP frames after a reset return 0. stats may be NULL. Not
 * reentrant (the background and work buffers are static PSRAM, ~55 KB). */
uint8_t mxm_detect(const uint16_t *frame, const mxp_motion_config_t *cfg, mxp_result_t *out, uint8_t cap,
                   mxm_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
