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

static size_t vec_payload(const char *name, uint8_t *out, size_t cap) {
  for (int i = 0; i < VEC_COUNT(VEC_FRAMES); i++)
    if (strcmp(VEC_FRAMES[i].name, name) == 0) return unhex(VEC_FRAMES[i].payload_hex, out, cap);
  CHECK(0, "no vector %s", name);
  return 0;
}

static void check_results_equal(const mxp_results_t *a, const mxp_results_t *b, const char *name) {
  CHECK(a->frame_id == b->frame_id && a->function_id == b->function_id && a->count == b->count,
        "%s header", name);
  for (int i = 0; i < a->count && i < b->count; i++) {
    const mxp_result_t *x = &a->r[i], *y = &b->r[i];
    CHECK(x->id == y->id && x->x == y->x && x->y == y->y && x->w == y->w && x->h == y->h &&
              x->confidence == y->confidence && x->extra_len == y->extra_len &&
              memcmp(x->extra, y->extra, x->extra_len) == 0,
          "%s record %d", name, i);
  }
}

static void test_results(void) {
  static const char *names[] = {"results_color_two", "results_tag_one"};
  const mxp_results_t *exp[] = {&VEC_P_RESULTS_COLOR_TWO, &VEC_P_RESULTS_TAG_ONE};
  for (int i = 0; i < 2; i++) {
    uint8_t pl[128], out[128];
    size_t pn = vec_payload(names[i], pl, sizeof pl);
    mxp_results_t r;
    CHECK(mxp_unpack_results(pl, (uint16_t)pn, &r), "unpack %s", names[i]);
    check_results_equal(&r, exp[i], names[i]);
    size_t n = mxp_pack_results(exp[i], out, sizeof out);
    CHECK(n == pn && memcmp(out, pl, pn) == 0, "pack %s", names[i]);
    CHECK(mxp_pack_results(exp[i], out, pn - 1) == 0, "pack %s small buffer", names[i]);
    CHECK(!mxp_unpack_results(pl, (uint16_t)(pn - 1), &r), "unpack %s truncated", names[i]);
    CHECK(!mxp_unpack_results(pl, (uint16_t)(pn + 1), &r), "unpack %s trailing byte", names[i]);
  }
  /* count 0 */
  mxp_results_t z;
  memset(&z, 0, sizeof z);
  z.frame_id = 7;
  z.function_id = 1;
  uint8_t out[16];
  CHECK(mxp_pack_results(&z, out, sizeof out) == 6, "empty results is 6 bytes");
  mxp_results_t back;
  CHECK(mxp_unpack_results(out, 6, &back) && back.count == 0 && back.frame_id == 7, "empty unpack");
  /* count > 8 */
  uint8_t bad[6] = {0, 0, 0, 0, 1, 9};
  CHECK(!mxp_unpack_results(bad, 6, &back), "count 9 rejected");
  z.count = 9;
  CHECK(mxp_pack_results(&z, out, sizeof out) == 0, "pack count 9 rejected");
  /* extra_len mismatch: claims 5 extra bytes but gives none; extra_len > 18 */
  uint8_t pl[128];
  size_t pn = vec_payload("results_color_two", pl, sizeof pl);
  pl[6 + 11] = 5;
  CHECK(!mxp_unpack_results(pl, (uint16_t)pn, &back), "extra_len overruns");
  pn = vec_payload("results_tag_one", pl, sizeof pl);
  pl[6 + 11] = 19;
  CHECK(!mxp_unpack_results(pl, (uint16_t)pn, &back), "extra_len 19 rejected");
  mxp_results_t t = VEC_P_RESULTS_TAG_ONE;
  t.r[0].extra_len = 19;
  CHECK(mxp_pack_results(&t, pl, sizeof pl) == 0, "pack extra_len 19 rejected");
  CHECK(!mxp_unpack_results(pl, 5, &back), "too short");
}

static void test_config(void) {
  uint8_t pl[32], out[18];
  size_t pn = vec_payload("config_default", pl, sizeof pl);
  CHECK(pn == 18, "config 18 bytes");
  mxp_config_t c;
  CHECK(mxp_unpack_config(pl, 18, &c), "unpack config");
  const mxp_config_t *e = &VEC_P_CONFIG_DEFAULT;
  CHECK(c.tolerance == e->tolerance && c.min_area == e->min_area && c.color_max == e->color_max &&
            c.color_names == e->color_names && c.tag_names == e->tag_names &&
            c.face_threshold == e->face_threshold && c.face_max == e->face_max &&
            c.uart_baud == e->uart_baud && c.i2c_address == e->i2c_address && c.led == e->led &&
            c.dirty == e->dirty,
        "config fields");
  CHECK(mxp_pack_config(e, out) == 18 && memcmp(out, pl, 18) == 0, "pack config");
  CHECK(!mxp_unpack_config(pl, 17, &c) && !mxp_unpack_config(pl, 19, &c), "config length");
  /* dirty is ignored on unpack and written on pack */
  uint8_t q[18];
  memcpy(q, pl, 18);
  q[15] = 1;
  CHECK(mxp_unpack_config(q, 18, &c) && c.dirty == 0, "dirty ignored on unpack");
  mxp_config_t d = *e;
  d.dirty = 1;
  mxp_pack_config(&d, out);
  CHECK(out[15] == 1, "dirty packed");
  /* wrong version */
  memcpy(q, pl, 18);
  q[0] = 2;
  CHECK(!mxp_unpack_config(q, 18, &c), "config_version 2 rejected");
  /* clamping */
  memcpy(q, pl, 18);
  q[1] = 200; /* tolerance */
  q[4] = 0;   /* color max_results */
  q[7] = 150; /* face threshold */
  q[8] = 9;   /* face max_results */
  q[9] = 0x80; q[10] = 0x25; q[11] = 0; q[12] = 0; /* baud 9600 is valid */
  q[13] = 0x80; /* i2c out of range */
  q[14] = 101;  /* led */
  CHECK(mxp_unpack_config(q, 18, &c), "unpack clamp");
  CHECK(c.tolerance == 100 && c.color_max == 1 && c.face_threshold == 100 && c.face_max == 8 &&
            c.uart_baud == 9600 && c.i2c_address == 0x5A && c.led == 100,
        "clamped values");
  q[9] = 0x01; q[10] = 0; q[11] = 0; q[12] = 0; /* baud 1 -> 115200 */
  q[4] = 200; q[13] = 0x07;
  CHECK(mxp_unpack_config(q, 18, &c) && c.uart_baud == 115200 && c.color_max == 8 &&
            c.i2c_address == 0x5A,
        "baud/i2c/max fallback");
  q[13] = 0x08;
  CHECK(mxp_unpack_config(q, 18, &c) && c.i2c_address == 0x08, "i2c low bound");
  q[13] = 0x77;
  CHECK(mxp_unpack_config(q, 18, &c) && c.i2c_address == 0x77, "i2c high bound");
  q[13] = 0x78;
  CHECK(mxp_unpack_config(q, 18, &c) && c.i2c_address == 0x5A, "i2c 0x78 out");
}

static void test_learn(void) {
  uint8_t pl[16];
  size_t pn = vec_payload("learn_request", pl, sizeof pl);
  mxp_learn_req_t l;
  CHECK(pn == 9 && mxp_unpack_learn(pl, 9, &l), "unpack learn");
  const mxp_learn_req_t *e = &VEC_P_LEARN_REQUEST;
  CHECK(l.function_id == e->function_id && l.x == e->x && l.y == e->y && l.w == e->w &&
            l.h == e->h,
        "learn fields");
  CHECK(!mxp_unpack_learn(pl, 8, &l) && !mxp_unpack_learn(pl, 10, &l), "learn length");
}

static void test_set_name(void) {
  uint8_t pl[80];
  size_t pn = vec_payload("set_name_utf8", pl, sizeof pl);
  mxp_set_name_t s;
  CHECK(mxp_unpack_set_name(pl, (uint16_t)pn, &s), "unpack set_name");
  const mxp_set_name_t *e = &VEC_P_SET_NAME_UTF8;
  CHECK(s.function_id == 1 && s.id == 2 && s.name_len == 12 && memcmp(s.name, e->name, 12) == 0,
        "set_name fields");
  CHECK(!mxp_unpack_set_name(pl, (uint16_t)(pn - 1), &s), "name_len mismatch short");
  CHECK(!mxp_unpack_set_name(pl, (uint16_t)(pn + 1), &s), "name_len mismatch long");
  CHECK(!mxp_unpack_set_name(pl, 3, &s), "too short");
  /* empty name clears */
  uint8_t clr[4] = {1, 2, 0, 0};
  CHECK(mxp_unpack_set_name(clr, 4, &s) && s.name_len == 0, "empty name clears");
  /* 48 bytes ok (16 CJK chars), 49 rejected */
  uint8_t big[4 + 49];
  big[0] = 1; big[1] = 2; big[2] = 0; big[3] = 48;
  for (int i = 0; i < 16; i++) { big[4 + 3 * i] = 0xE7; big[5 + 3 * i] = 0xB4; big[6 + 3 * i] = 0x85; }
  CHECK(mxp_unpack_set_name(big, 4 + 48, &s) && s.name_len == 48, "48 bytes ok");
  big[3] = 49; big[4 + 48] = 'a';
  CHECK(!mxp_unpack_set_name(big, 4 + 49, &s), "49 bytes rejected");
  /* invalid UTF-8 */
  static const struct { const char *what; uint8_t b[4]; uint8_t n; } bad[] = {
      {"lone continuation", {0x80}, 1},
      {"truncated 2-byte", {0xC3}, 1},
      {"overlong 2-byte", {0xC0, 0x80}, 2},
      {"overlong 2-byte C1", {0xC1, 0xBF}, 2},
      {"overlong 3-byte", {0xE0, 0x80, 0x80}, 3},
      {"surrogate", {0xED, 0xA0, 0x80}, 3},
      {"overlong 4-byte", {0xF0, 0x80, 0x80, 0x80}, 4},
      {"above U+10FFFF", {0xF4, 0x90, 0x80, 0x80}, 4},
      {"F5 lead", {0xF5, 0x80, 0x80, 0x80}, 4},
      {"bad continuation", {0xE7, 0x41, 0x85}, 3},
      {"truncated 3-byte", {0xE7, 0xB4}, 2},
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    uint8_t p[8] = {1, 2, 0, bad[i].n};
    memcpy(p + 4, bad[i].b, bad[i].n);
    CHECK(!mxp_unpack_set_name(p, (uint16_t)(4 + bad[i].n), &s), "utf8 %s", bad[i].what);
  }
  static const struct { uint8_t b[4]; uint8_t n; } good[] = {
      {{0x7F}, 1}, {{0xC2, 0x80}, 2}, {{0xDF, 0xBF}, 2}, {{0xE0, 0xA0, 0x80}, 3},
      {{0xED, 0x9F, 0xBF}, 3}, {{0xEE, 0x80, 0x80}, 3}, {{0xF0, 0x90, 0x80, 0x80}, 4},
      {{0xF4, 0x8F, 0xBF, 0xBF}, 4},
  };
  for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
    uint8_t p[8] = {1, 2, 0, good[i].n};
    memcpy(p + 4, good[i].b, good[i].n);
    CHECK(mxp_unpack_set_name(p, (uint16_t)(4 + good[i].n), &s), "utf8 valid case %d", (int)i);
    CHECK(mxp_utf8_valid(good[i].b, good[i].n), "mxp_utf8_valid good %d", (int)i);
  }
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    CHECK(!mxp_utf8_valid(bad[i].b, bad[i].n), "mxp_utf8_valid bad %s", bad[i].what);
  CHECK(mxp_utf8_valid(NULL, 0), "empty string is valid UTF-8");
}

static void test_learned(void) {
  uint8_t pl[128], out[128];
  size_t pn = vec_payload("learned_list", pl, sizeof pl);
  size_t n = mxp_pack_learned(VEC_P_LEARNED_LIST, 2, out, sizeof out);
  CHECK(n == pn && memcmp(out, pl, pn) == 0, "pack learned");
  CHECK(mxp_pack_learned(VEC_P_LEARNED_LIST, 2, out, pn - 1) == 0, "learned small buffer");
  CHECK(mxp_pack_learned(NULL, 0, out, sizeof out) == 1 && out[0] == 0, "empty learned list");
  mxp_learned_t too_long = VEC_P_LEARNED_LIST[0];
  too_long.name_len = 49;
  CHECK(mxp_pack_learned(&too_long, 1, out, sizeof out) == 0, "learned name 49 rejected");
}

static void test_error_full(void) {
  uint8_t out[16];
  CHECK(MXP_ERR_FULL == 7, "FULL code");
  CHECK(mxp_encode_error(1, MXP_CMD_LEARN, MXP_ERR_FULL, out, sizeof out) == 11 && out[3] == 0xFF &&
            out[7] == 0x30 && out[8] == 0x07,
        "error full frame");
}

int main(void) {
  test_crc();
  test_encode();
  test_decoder_vectors();
  test_info_roundtrip();
  test_oversize_dropped();
  test_small_out();
  test_misc();
  test_results();
  test_config();
  test_learn();
  test_set_name();
  test_learned();
  test_error_full();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
