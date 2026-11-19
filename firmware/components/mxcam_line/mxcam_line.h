#ifndef MXCAM_LINE_H
#define MXCAM_LINE_H

#include <stdint.h>

#include "mxcam_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Line tracking (function 4, ADR-0018) on a 320x240 RGB565 big-endian frame.
 * Works on the 160x120 mxg_half grey image: the ROI is the bottom
 * 120 * roi_pct / 100 rows, cut into `bands` equal horizontal bands (band 0 at
 * the bottom; leftover rows at the top of the ROI are dropped). A cell is
 * "line" when gray < T (mode 0, dark line), gray > T (mode 1, light line) or
 * it matches the learned colour (mode 2, mxc_match_px on the top-left pixel of
 * its 2x2 block). T = threshold, or Otsu over the ROI when threshold is 0
 * (class means closer than MXL_MIN_CONTRAST -> no line). In each band a column
 * is "line" when at least half its rows are; runs of such columns are
 * segments, valid when their width (x2 = 320 px) is within
 * [min_width, max_width]; wider runs set CROSS. Bands are walked bottom-up
 * picking the valid segment nearest a prediction: the first pick has no
 * distance limit and empty bands before it are passed over; after it, a
 * segment farther than max(8, 2 * band height) half-res px is skipped and two
 * skipped bands in a row stop the walk. A least-squares fit over the picked
 * band centres gives one main line. All output coordinates are 320x240
 * real-image coordinates (ADR-0010). */

#define MXL_MAX_BANDS 12
#define MXL_MIN_CONTRAST 40 /* Otsu: minimum difference between the two class means */

/* Owned by the caller; zero it when the function changes. last_x0 is the
 * bottom end of the last reported line in 160x120 units. */
typedef struct {
  uint8_t has_last;
  int16_t last_x0;
} mxl_state_t;

/* For the TFT overlay (320x240 units): pts = centre (x, y) of each picked
 * band's segment, bottom-up; roi_top = first ROI row. */
typedef struct {
  uint8_t n;
  int16_t pts[MXL_MAX_BANDS][2];
  int16_t roi_top;
} mxl_trace_t;

/* Returns 0 or 1 record. With 1, out is the line ResultRecord (protocol.md:
 * id 1, x/y midpoint, w/h end-point bounding box, confidence = picked bands *
 * 100 / bands, 14-byte line extra) and st->last_x0 is updated; with 0 (fewer
 * than 2 bands picked, no contrast) st->has_last is cleared and out is
 * untouched. line_rgb = NULL means nothing learned: mode 2 then runs as mode
 * 0. trace may be NULL. Not reentrant (static PSRAM work buffers). */
uint8_t mxl_detect(const uint16_t *frame, const mxp_line_config_t *cfg, const uint8_t *line_rgb,
                   mxl_state_t *st, mxp_result_t *out, mxl_trace_t *trace);

#ifdef __cplusplus
}
#endif

#endif
