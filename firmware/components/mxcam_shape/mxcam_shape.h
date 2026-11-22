#ifndef MXCAM_SHAPE_H
#define MXCAM_SHAPE_H

#include <stddef.h>
#include <stdint.h>

#include "mxcam_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Geometric shapes (function 11, ADR-0021) on a 320x240 RGB565 big-endian
 * frame. Works on the 160x120 mxg_half grey image: binarise (mode 0 dark
 * shapes on light, mode 1 light on dark; threshold 0 = Otsu, class means
 * closer than MXSH_MIN_CONTRAST -> no shapes, 1-255 fixed: mode 0 gray < T,
 * mode 1 gray > T), 8-connected regions (explicit stack), drop regions that
 * touch the frame edge, whose bounding box long side (x2 = 320 px) is below
 * min_size, or whose bounding box covers more than MXSH_MAX_BG_PCT % of the
 * frame (paper / background). Each remaining region's outer contour (Moore
 * tracing, at most MXSH_MAX_CONTOUR points, longer contours are skipped) goes
 * through mxsh_classify. Ids are MXP_SHAPE_*; output coordinates are 320x240
 * real-image coordinates (ADR-0010). */

#define MXSH_MIN_CONTRAST 40    /* Otsu: minimum difference between the two class means */
#define MXSH_MAX_CONTOUR 1024   /* contour points; longer contours are skipped */
#define MXSH_CIRC_MIN 0.80f     /* circle: circularity 4*pi*A'/P^2 at least this */
#define MXSH_CV_MAX 0.055f      /* circle: coefficient of variation of the centroid distance at most this
                                   (regular pentagon ~0.064, hexagon ~0.046, 0.9 ellipse ~0.037) */
#define MXSH_DP_EPS_PCT 3       /* Douglas-Peucker epsilon = max(1, 3 % of the perimeter) */
#define MXSH_QUAD_ANGLE_TOL 20  /* square / rectangle: every interior angle within 90 +- 20 degrees */
#define MXSH_SQUARE_RATIO 1.25f /* square: mean opposite side lengths differ by at most this ratio */
#define MXSH_MAX_BG_PCT 60      /* regions whose bounding box covers more of the frame are background */

/* Closed contour points (any coordinate unit, in order, last point not
 * repeated) -> shape id (0 = not reported) and confidence 0-100 (the
 * contour area A' = shoelace + P/2 against the fitted shape's area expanded
 * the same way: the Douglas-Peucker polygon, or the circle of mean radius).
 * P is the sum of the step lengths. Classification, in order: circularity >=
 * MXSH_CIRC_MIN and CV <= MXSH_CV_MAX -> circle; 3 vertices -> triangle; 4
 * convex vertices with every angle within 90 +- MXSH_QUAD_ANGLE_TOL -> square
 * when the mean opposite side lengths differ by at most MXSH_SQUARE_RATIO,
 * else rectangle; anything else -> 0. n must be 3..MXSH_MAX_CONTOUR (else 0).
 * confidence may be NULL. Not reentrant (static work buffers). */
uint16_t mxsh_classify(const int16_t (*pts)[2], int n, uint8_t *confidence);

/* Writes 0..min(cap, cfg->max_results) records, largest contour area first:
 * id = shape id, x/y/w/h = bounding box (320x240), confidence as above,
 * extra_len 0. Not reentrant (static PSRAM work buffers, ~90 KB). */
uint8_t mxsh_detect(const uint16_t *frame, const mxp_shape_config_t *cfg, mxp_result_t *out, uint8_t cap);

/* "CIRCLE" "TRIANGLE" "SQUARE" "RECT"; other ids "?". */
const char *mxsh_ascii_name(uint16_t id);
/* "SQUARE 90" (TFT label); snprintf semantics (returns the untruncated length). */
int mxsh_label(uint16_t id, uint8_t conf, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
