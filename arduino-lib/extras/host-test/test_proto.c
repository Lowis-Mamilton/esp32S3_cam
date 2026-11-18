#include <stdio.h>
#include <string.h>

#include "mxcam_proto.h"
#include "vectors.h"

static int failures = 0;
#define CHECK(cond, ...)                          \
  do {                                            \
    if (!(cond)) {                                \
      failures++;                                 \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
    }                                             \
  } while (0)

static size_t unhex(const char *s, uint8_t *out, size_t cap) {
  size_t n = strlen(s) / 2;
  if (n > cap) n = cap;
  for (size_t i = 0; i < n; i++) {
    unsigned v = 0;
    sscanf(s + 2 * i, "%2x", &v);
    out[i] = (uint8_t)v;
  }
  return n;
}

static void test_crc(void) {
  for (int i = 0; i < VEC_COUNT(VEC_CRC); i++) {
    uint8_t b[64];
    size_t n = unhex(VEC_CRC[i].hex, b, sizeof b);
    CHECK(mxp_crc16(b, n) == VEC_CRC[i].crc, "crc vector %d", i);
  }
}

static void test_encode(void) {
  for (int i = 0; i < VEC_COUNT(VEC_FRAMES); i++) {
    const vec_frame_t *f = &VEC_FRAMES[i];
    uint8_t pl[64], exp[128], out[128];
    size_t pn = unhex(f->payload_hex, pl, sizeof pl);
    size_t en = unhex(f->encoded_hex, exp, sizeof exp);
    size_t n = mxp_encode(f->cmd, f->seq, pl, (uint16_t)pn, out, sizeof out);
    CHECK(n == en && memcmp(out, exp, en) == 0, "encode %s", f->name);
  }
}

static void test_decoder_vectors(void) {
  for (int i = 0; i < VEC_COUNT(VEC_DECODER); i++) {
    const vec_decoder_t *v = &VEC_DECODER[i];
    uint8_t buf[MXP_MAX_REQUEST_PAYLOAD];
    mxp_decoder_t d;
    mxp_decoder_init(&d, buf, sizeof buf);
    int got = 0;
    for (int c = 0; c < v->nchunks; c++) {
      uint8_t bytes[128];
      size_t n = unhex(v->chunks[c], bytes, sizeof bytes);
      for (size_t k = 0; k < n; k++) {
        mxp_frame_t f;
        if (mxp_decoder_feed(&d, bytes[k], &f)) {
          if (got < v->nexpect) {
            uint8_t ep[64];
            size_t en = unhex(v->expect[got].payload_hex, ep, sizeof ep);
            CHECK(f.cmd == v->expect[got].cmd && f.seq == v->expect[got].seq &&
                      f.len == en && memcmp(f.payload, ep, en) == 0,
                  "decoder %s frame %d mismatch", v->name, got);
          }
          got++;
        }
      }
    }
    CHECK(got == v->nexpect, "decoder %s: got %d frames, expected %d", v->name, got,
          v->nexpect);
  }
}

static void test_info_roundtrip(void) {
  for (int i = 0; i < VEC_COUNT(VEC_FRAMES); i++) {
    if (strcmp(VEC_FRAMES[i].name, "get_info_response_m1") != 0) continue;
    uint8_t pl[64], out[64];
    size_t pn = unhex(VEC_FRAMES[i].payload_hex, pl, sizeof pl);
    mxp_info_t info;
    CHECK(mxp_unpack_info(pl, (uint16_t)pn, &info), "unpack_info");
    CHECK(info.frame_width == 320 && info.frame_height == 240 && info.max_results == 8 &&
              info.function_count == 0 && memcmp(info.fw_git, "a1b2c3d", 7) == 0,
          "info fields");
    size_t n = mxp_pack_info(&info, out, sizeof out);
    CHECK(n == pn && memcmp(out, pl, pn) == 0, "info roundtrip");
    CHECK(mxp_pack_info(&info, out, pn - 1) == 0, "pack_info small buffer");
    CHECK(!mxp_unpack_info(pl, (uint16_t)(pn - 1), &info), "unpack_info short");
  }
}

static void test_oversize_dropped(void) {
  /* cap 1024; len = 2000 must be dropped without swallowing the next PING. */
  uint8_t buf[MXP_MAX_REQUEST_PAYLOAD];
  uint8_t hdr[7] = {0x4D, 0x58, 1, 0x20, 9, 2000 & 0xFF, 2000 >> 8};
  uint8_t ping[16];
  size_t pn = mxp_encode(MXP_CMD_PING, 3, NULL, 0, ping, sizeof ping);
  mxp_decoder_t d;
  mxp_decoder_init(&d, buf, sizeof buf);
  mxp_frame_t f;
  int got = 0;
  for (int i = 0; i < 7; i++) got += mxp_decoder_feed(&d, hdr[i], &f);
  for (size_t i = 0; i < pn; i++) {
    if (mxp_decoder_feed(&d, ping[i], &f)) {
      got++;
      CHECK(f.cmd == MXP_CMD_PING && f.seq == 3 && f.len == 0, "ping after oversize");
    }
  }
  CHECK(got == 1, "oversize: expected exactly the PING, got %d", got);
}

static void test_small_out(void) {
  uint8_t out[16], pl[3] = {1, 2, 3};
  CHECK(mxp_encode(MXP_CMD_PING, 1, NULL, 0, out, MXP_OVERHEAD - 1) == 0, "cap 8 for empty");
  CHECK(mxp_encode(MXP_CMD_PING, 1, NULL, 0, out, MXP_OVERHEAD) == MXP_OVERHEAD, "cap 9 ok");
  CHECK(mxp_encode(0x10, 1, pl, 3, out, 11) == 0, "cap 11 for len 3");
  CHECK(mxp_encode(0x10, 1, pl, 3, out, 12) == 12, "cap 12 for len 3");
  CHECK(mxp_encode_error(1, 0x7E, MXP_ERR_UNKNOWN_CMD, out, 10) == 0, "error small");
  CHECK(mxp_encode_error(7, 0x7E, MXP_ERR_UNKNOWN_CMD, out, sizeof out) == 11, "error ok");
}

static void test_misc(void) {
  mxp_stream_start_t s;
  uint8_t p[3] = {1, 10, 0};
  CHECK(mxp_unpack_stream_start(p, 3, &s) && s.flags == 1 && s.max_fps == 10 &&
            s.jpeg_quality == 0,
        "stream_start");
  CHECK(!mxp_unpack_stream_start(p, 2, &s), "stream_start short");
  mxp_frame_header_t h = {1, 1000, 320, 240};
  uint8_t out[MXP_FRAME_HEADER_SIZE], exp[12];
  unhex("01000000e80300004001f000", exp, 12);
  mxp_pack_frame_header(&h, out);
  CHECK(memcmp(out, exp, 12) == 0, "frame header");
}

int main(void) {
  test_crc();
  test_encode();
  test_decoder_vectors();
  test_info_roundtrip();
  test_oversize_dropped();
  test_small_out();
  test_misc();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
