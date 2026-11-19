#ifndef MXCAM_GRAY_H
#define MXCAM_GRAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RGB565 big-endian (camera byte order: bytes hi,lo in memory) -> 8-bit luma,
 * Y = (77R + 150G + 29B) >> 8 on channels expanded to 8 bits. Same formula and
 * values as mxt_luma_be (mxcam_tag); white stays 255. Shared by the line and
 * object tracking functions (ADR-0018). Stateless and reentrant. */

#define MXG_HALF_W 160
#define MXG_HALF_H 120

uint8_t mxg_luma_be(uint16_t px);
/* 320x240 RGB565BE frame -> 160x120 luma, each cell the rounded mean
 * ((sum + 2) >> 2) of its 2x2 block. out must hold 160*120 bytes. */
void mxg_half(const uint16_t *frame, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif
