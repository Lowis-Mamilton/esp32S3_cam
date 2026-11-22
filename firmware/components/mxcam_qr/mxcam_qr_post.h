#ifndef MXCAM_QR_POST_H
#define MXCAM_QR_POST_H

#include <stddef.h>
#include <stdint.h>

#include "mxcam_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pure C post-processing for mxcam_qr (host-tested, ADR-0020): turns decoded
 * QR codes into protocol ResultRecords (protocol.md section 11, function 6)
 * in true-image (ADR-0010) 320x240 coordinates, keeps the largest codes,
 * picks the code to learn and builds the TFT label. */

#define MXQ_FRAME_W 320
#define MXQ_FRAME_H 240
/* At most this many codes are reported per frame (largest first). */
#define MXQ_MAX_CODES 4
/* Payload characters shown in the TFT label. */
#define MXQ_LABEL_CHARS 8

typedef struct {
  int16_t x[4], y[4];   /* quirc corners: the code's own TL, TR, BR, BL (320x240, not clipped) */
  uint16_t total_len;   /* payload length as decoded */
  uint8_t len;          /* stored bytes = min(total_len, MXP_RESULT_DATA_MAX) */
  uint8_t bytes[MXP_RESULT_DATA_MAX];
} mxq_code_t;

/* Stores the payload: len = min(n, MXP_RESULT_DATA_MAX) first bytes,
 * total_len = n (saturating at 65535). p may be NULL when n is 0. */
void mxq_set_payload(mxq_code_t *c, const uint8_t *p, size_t n);

/* Twice the area of the corner quadrilateral (shoelace formula, absolute
 * value; saturates at INT32_MAX). */
int32_t mxq_area2(const mxq_code_t *c);

/* Sorts codes[0..n) by mxq_area2, largest first (equal areas keep their
 * order), and returns min(n, max): the caller keeps that many. */
uint8_t mxq_sort_limit(mxq_code_t *codes, uint8_t n, uint8_t max);

/* One ResultRecord (ADR-0020 decision 5):
 *   id          the learned id (1..8), or 0 when not learned
 *   x, y, w, h  bounding box of the four corners clipped to the frame:
 *               w = max - min + 1, centre = (min + max) / 2 (rounded down)
 *   confidence  255 (not applicable: a decoded code is certain)
 *   extra       18 bytes as AprilTag (mxp_pack_corners_extra): rotation_cdeg
 *               = atan2(TR.y - TL.y, TR.x - TL.x) in 0.01 degrees (upright 0,
 *               clockwise on screen positive), then the four corners TL, TR,
 *               BR, BL, not clipped. */
void mxq_to_result(const mxq_code_t *c, uint16_t id, mxp_result_t *out);

/* Index of the code to learn from region (x, y, w, h) (top-left + size,
 * 320x240, may reach past the frame): among the codes whose record centre
 * (mxq_to_result's x, y) lies in [x, x + w) x [y, y + h), the largest
 * (mxq_area2; ties: the first). -1 when none. */
int mxq_pick_for_learn(const mxq_code_t *codes, uint8_t n, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/* TFT label: "QR<id> <text>" ("QR <text>" for id 0), text = the first
 * MXQ_LABEL_CHARS payload bytes with every byte outside printable ASCII
 * (0x20..0x7E) shown as '?'. An empty payload gives just "QR<id>" / "QR".
 * snprintf semantics: returns the full length, writes at most cap bytes and
 * always NUL-terminates when cap > 0. */
int mxq_label(uint16_t id, const mxq_code_t *c, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
