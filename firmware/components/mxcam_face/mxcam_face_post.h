#ifndef MXCAM_FACE_POST_H
#define MXCAM_FACE_POST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mxcam_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pure C post-processing for mxcam_face (host-tested): turns the detector's
 * boxes into protocol ResultRecords (protocol.md 7) in true-image (ADR-0010)
 * 320x240 coordinates. */

#define MXF_FRAME_W 320
#define MXF_FRAME_H 240

/* One detector box: [x0, y0] top-left, [x1, y1] bottom-right (x1 - x0 is the
 * width, as esp-dl's result_t::box), in the coordinates of the image the
 * detector saw (src_w x src_h); score is the model score 0..1. */
typedef struct {
  int32_t x0, y0, x1, y1;
  float score;
} mxf_raw_box_t;

/* round(score * 100) clamped to 0..100; NaN reads 0. */
uint8_t mxf_confidence(float score);

/* Scales the box from src_w x src_h to 320x240 (rounded), clips it to the
 * frame and fills a record: id 0, x/y = box centre (x0 + w/2, y0 + h/2), w/h,
 * confidence = mxf_confidence(score), extra_len 0. false (out untouched) when
 * src_w/src_h is 0 or the clipped box is empty. */
bool mxf_to_result(const mxf_raw_box_t *b, uint16_t src_w, uint16_t src_h, mxp_result_t *out);

/* Converts n boxes, drops empty ones and those with confidence < threshold,
 * sorts by confidence (descending; ties: larger area first, then detector
 * order) and writes at most min(max_results, cap, MXP_MAX_RESULTS) records.
 * Returns the number written. */
uint8_t mxf_select(const mxf_raw_box_t *boxes, size_t n, uint16_t src_w, uint16_t src_h, uint8_t threshold,
                   uint8_t max_results, mxp_result_t *out, uint8_t cap);

#ifdef __cplusplus
}
#endif

#endif
