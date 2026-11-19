#include "mxcam_gray.h"

/* With bytes hi (RRRRRGGG) and lo (GGGBBBBB) the expanded channels split into
 * per-byte fields: r8 = f(hi), b8 = f(lo), g8 = ((hi&7)<<5 | (hi&7)>>1) + ((lo>>5)<<2),
 * so 77r8 + 150g8 + 29b8 = LUT_HI[hi] + LUT_LO[lo] exactly (as in mxcam_tag).
 * The tables are built by the preprocessor: const, no init, no data race. */
#define R5_8(b) ((((b) >> 3) << 3) | ((b) >> 5))         /* r5 = b>>3; (r5<<3)|(r5>>2) */
#define GH_8(b) ((((b) & 7u) << 5) | (((b) & 7u) >> 1))  /* green high bits, expanded */
#define B5_8(b) ((((b) & 31u) << 3) | (((b) & 31u) >> 2))
#define HI(b) (uint16_t)(77u * R5_8(b) + 150u * GH_8(b))
#define LO(b) (uint16_t)(150u * (((b) >> 5) << 2) + 29u * B5_8(b))

#define X4(m, b) m(b), m((b) + 1u), m((b) + 2u), m((b) + 3u)
#define X16(m, b) X4(m, b), X4(m, (b) + 4u), X4(m, (b) + 8u), X4(m, (b) + 12u)
#define X64(m, b) X16(m, b), X16(m, (b) + 16u), X16(m, (b) + 32u), X16(m, (b) + 48u)
#define X256(m) X64(m, 0u), X64(m, 64u), X64(m, 128u), X64(m, 192u)

static const uint16_t LUT_HI[256] = {X256(HI)};
static const uint16_t LUT_LO[256] = {X256(LO)};

static inline unsigned luma2(uint8_t hi, uint8_t lo) { return (unsigned)(LUT_HI[hi] + LUT_LO[lo]) >> 8; }

uint8_t mxg_luma_be(uint16_t px) {
  const uint8_t *b = (const uint8_t *)&px; /* memory order: hi, lo */
  return (uint8_t)luma2(b[0], b[1]);
}

void mxg_half(const uint16_t *frame, uint8_t *out) {
  for (int y = 0; y < MXG_HALF_H; y++) {
    const uint8_t *a = (const uint8_t *)(frame + (2 * y) * (2 * MXG_HALF_W));
    const uint8_t *b = a + 2 * (2 * MXG_HALF_W); /* next row, in bytes */
    uint8_t *dst = out + y * MXG_HALF_W;
    for (int x = 0; x < MXG_HALF_W; x++, a += 4, b += 4) {
      unsigned sum = luma2(a[0], a[1]) + luma2(a[2], a[3]) + luma2(b[0], b[1]) + luma2(b[2], b[3]);
      dst[x] = (uint8_t)((sum + 2u) >> 2);
    }
  }
}
