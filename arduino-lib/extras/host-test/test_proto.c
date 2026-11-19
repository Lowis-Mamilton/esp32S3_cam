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

/* ---- M6 groundwork (protocol.md 9) ---- */

static void test_info_32(void) {
  CHECK(MXP_INFO_MAX_FUNCS == 32, "MXP_INFO_MAX_FUNCS");
  mxp_info_t info, back;
  memset(&info, 0, sizeof info);
  info.protocol_version = 1;
  info.frame_width = 320;
  info.frame_height = 240;
  info.max_results = 8;
  info.function_count = 32;
  for (int i = 0; i < 32; i++) info.function_ids[i] = (uint8_t)(i + 1);
  uint8_t out[23 + 33 + 4];
  size_t n = mxp_pack_info(&info, out, sizeof out);
  CHECK(n == 23 + 32, "pack 32 ids: %d", (int)n);
  CHECK(mxp_unpack_info(out, (uint16_t)n, &back) && back.function_count == 32 &&
            memcmp(back.function_ids, info.function_ids, 32) == 0,
        "32 ids round trip");
  CHECK(mxp_pack_info(&info, out, 23 + 31) == 0, "pack 32 ids small buffer");
  info.function_count = 33;
  CHECK(mxp_pack_info(&info, out, sizeof out) == 0, "pack 33 ids rejected");
  out[22] = 33;
  out[23 + 32] = 33;
  CHECK(!mxp_unpack_info(out, 23 + 33, &back), "unpack 33 ids rejected");

  uint8_t pl[64];
  size_t pn = vec_payload("get_info_response_m6", pl, sizeof pl);
  static const uint8_t ids[5] = {1, 2, 3, 4, 5};
  CHECK(mxp_unpack_info(pl, (uint16_t)pn, &back) && back.function_count == 5 &&
            memcmp(back.function_ids, ids, 5) == 0,
        "get_info_response_m6 ids");
  CHECK(mxp_pack_info(&back, out, sizeof out) == pn && memcmp(out, pl, pn) == 0, "m6 info round trip");
}

static void test_m6_codes(void) {
  CHECK(MXP_FUNC_LINE == 4 && MXP_FUNC_TRACK == 5, "function ids 4, 5");
  CHECK(MXP_CMD_GET_FUNC_CONFIG == 0x44 && MXP_CMD_SET_FUNC_CONFIG == 0x45, "func config cmds");
  CHECK(MXP_FUNC_CONFIG_SIZE == 10 && MXP_LINE_CONFIG_VERSION == 1 && MXP_TRACK_CONFIG_VERSION == 1,
        "func config sizes");
  CHECK(MXP_LINE_MODE_DARK == 0 && MXP_LINE_MODE_LIGHT == 1 && MXP_LINE_MODE_LEARNED == 2, "line modes");
  CHECK(MXP_LINE_EXTRA_SIZE == 14 && MXP_LINE_FLAG_BRANCH == 0x01 && MXP_LINE_FLAG_CROSS == 0x02,
        "line extra constants");
}

static bool line_cfg_eq(const mxp_line_config_t *a, const mxp_line_config_t *b) {
  return a->mode == b->mode && a->threshold == b->threshold && a->roi_pct == b->roi_pct &&
         a->bands == b->bands && a->min_width == b->min_width && a->max_width == b->max_width &&
         a->color_tolerance == b->color_tolerance;
}

static bool track_cfg_eq(const mxp_track_config_t *a, const mxp_track_config_t *b) {
  return a->threshold == b->threshold && a->adapt == b->adapt && a->reacquire == b->reacquire &&
         a->scale == b->scale;
}

static void test_func_config_vectors(void) {
  uint8_t pl[32], out[MXP_FUNC_CONFIG_SIZE];
  /* line default */
  size_t pn = vec_payload("func_config_line_default", pl, sizeof pl);
  CHECK(pn == MXP_FUNC_CONFIG_SIZE, "line config 10 bytes");
  mxp_line_config_t lc, ld;
  memset(&ld, 0xAA, sizeof ld);
  mxp_line_config_defaults(&ld);
  CHECK(line_cfg_eq(&ld, &VEC_P_LINE_CONFIG_DEFAULT), "line defaults match vector");
  CHECK(ld.mode == 0 && ld.threshold == 0 && ld.roi_pct == 60 && ld.bands == 8 && ld.min_width == 4 &&
            ld.max_width == 80 && ld.color_tolerance == 30,
        "line defaults");
  CHECK(mxp_unpack_line_config(pl, (uint16_t)pn, &lc) && line_cfg_eq(&lc, &VEC_P_LINE_CONFIG_DEFAULT),
        "unpack line default");
  memset(out, 0xEE, sizeof out);
  CHECK(mxp_pack_line_config(&ld, out) == MXP_FUNC_CONFIG_SIZE && memcmp(out, pl, pn) == 0,
        "pack line default");
  /* track set */
  pn = vec_payload("set_func_config_track_request", pl, sizeof pl);
  CHECK(pn == MXP_FUNC_CONFIG_SIZE, "track config 10 bytes");
  mxp_track_config_t tc, td;
  CHECK(mxp_unpack_track_config(pl, (uint16_t)pn, &tc) && track_cfg_eq(&tc, &VEC_P_TRACK_CONFIG_SET) &&
            tc.threshold == 60 && tc.adapt == 20 && tc.reacquire == 1 && tc.scale == 0,
        "unpack track set");
  memset(out, 0xEE, sizeof out);
  CHECK(mxp_pack_track_config(&tc, out) == MXP_FUNC_CONFIG_SIZE && memcmp(out, pl, pn) == 0,
        "pack track set");
  memset(&td, 0xAA, sizeof td);
  mxp_track_config_defaults(&td);
  CHECK(td.threshold == 50 && td.adapt == 10 && td.reacquire == 1 && td.scale == 1, "track defaults");
}

static void test_func_config_clamp(void) {
  uint8_t out[MXP_FUNC_CONFIG_SIZE];
  const uint8_t line_in[10] = {4, 1, 7, 0, 5, 30, 0, 0, 150, 9};
  mxp_line_config_t lc;
  CHECK(mxp_unpack_line_config(line_in, 10, &lc), "unpack line clamp");
  CHECK(lc.mode == 0 && lc.threshold == 0 && lc.roi_pct == 20 && lc.bands == 12 && lc.min_width == 1 &&
            lc.max_width == 1 && lc.color_tolerance == 100,
        "line clamped: mode %d roi %d bands %d min %d max %d tol %d", lc.mode, lc.roi_pct, lc.bands,
        lc.min_width, lc.max_width, lc.color_tolerance);
  static const uint8_t line_exp[10] = {4, 1, 0, 0, 20, 12, 1, 1, 100, 0};
  CHECK(mxp_pack_line_config(&lc, out) == 10 && memcmp(out, line_exp, 10) == 0, "line repack, reserved 0");
  const uint8_t line_in2[10] = {4, 1, 2, 255, 150, 3, 50, 20, 0, 0};
  CHECK(mxp_unpack_line_config(line_in2, 10, &lc) && lc.mode == 2 && lc.threshold == 255 &&
            lc.roi_pct == 100 && lc.bands == 4 && lc.min_width == 50 && lc.max_width == 50 &&
            lc.color_tolerance == 0,
        "line clamp upper/lower bounds");
  const uint8_t line_in3[10] = {4, 1, 1, 0, 60, 8, 200, 255, 30, 0};
  CHECK(mxp_unpack_line_config(line_in3, 10, &lc) && lc.mode == 1 && lc.min_width == 100 &&
            lc.max_width == 255,
        "line min_width 100, max_width kept");

  const uint8_t track_in[10] = {5, 1, 0, 80, 2, 5, 1, 2, 3, 4};
  mxp_track_config_t tc;
  CHECK(mxp_unpack_track_config(track_in, 10, &tc) && tc.threshold == 10 && tc.adapt == 50 &&
            tc.reacquire == 1 && tc.scale == 1,
        "track clamped");
  static const uint8_t track_exp[10] = {5, 1, 10, 50, 1, 1, 0, 0, 0, 0};
  CHECK(mxp_pack_track_config(&tc, out) == 10 && memcmp(out, track_exp, 10) == 0, "track repack, reserved 0");
  const uint8_t track_in2[10] = {5, 1, 200, 0, 0, 0, 0, 0, 0, 0};
  CHECK(mxp_unpack_track_config(track_in2, 10, &tc) && tc.threshold == 95 && tc.adapt == 0 &&
            tc.reacquire == 0 && tc.scale == 0,
        "track clamp upper bound / zeros");
}

static void test_func_config_reject(void) {
  uint8_t pl[16];
  mxp_line_config_t lc;
  mxp_track_config_t tc;
  vec_payload("func_config_line_default", pl, sizeof pl);
  pl[10] = 0;
  CHECK(!mxp_unpack_line_config(pl, 9, &lc) && !mxp_unpack_line_config(pl, 11, &lc), "line length");
  pl[0] = 5;
  CHECK(!mxp_unpack_line_config(pl, 10, &lc), "line wrong fn");
  pl[0] = 4;
  pl[1] = 2;
  CHECK(!mxp_unpack_line_config(pl, 10, &lc), "line version 2");
  pl[1] = 0;
  CHECK(!mxp_unpack_line_config(pl, 10, &lc), "line version 0");

  vec_payload("set_func_config_track_request", pl, sizeof pl);
  pl[10] = 0;
  CHECK(!mxp_unpack_track_config(pl, 9, &tc) && !mxp_unpack_track_config(pl, 11, &tc), "track length");
  pl[0] = 4;
  CHECK(!mxp_unpack_track_config(pl, 10, &tc), "track wrong fn");
  pl[0] = 5;
  pl[1] = 2;
  CHECK(!mxp_unpack_track_config(pl, 10, &tc), "track version 2");
}

/* hand_threshold default is provisional (docs/measurements/m8a.md); if it changes, update
 * GESTURE_DEFAULT_HAND_THRESHOLD here and mxp_gesture_config_defaults() - nothing else. The
 * "default" vectors are only sample wire bytes (round trip) and do not follow the default. */
#define GESTURE_DEFAULT_HAND_THRESHOLD 40

static bool gesture_cfg_eq(const mxp_gesture_config_t *a, const mxp_gesture_config_t *b) {
  return a->hand_threshold == b->hand_threshold && a->gesture_threshold == b->gesture_threshold &&
         a->max_hands == b->max_hands;
}

static void test_gesture_config_vectors(void) {
  uint8_t pl[32], out[MXP_FUNC_CONFIG_SIZE];
  CHECK(MXP_FUNC_GESTURE == 8 && MXP_GESTURE_CONFIG_VERSION == 1, "gesture ids");
  size_t pn = vec_payload("func_config_gesture_default", pl, sizeof pl);
  CHECK(pn == MXP_FUNC_CONFIG_SIZE, "gesture config 10 bytes");
  mxp_gesture_config_t g, d;
  CHECK(mxp_unpack_gesture_config(pl, (uint16_t)pn, &g) && gesture_cfg_eq(&g, &VEC_P_GESTURE_CONFIG_DEFAULT),
        "unpack gesture default");
  memset(out, 0xEE, sizeof out);
  CHECK(mxp_pack_gesture_config(&g, out) == MXP_FUNC_CONFIG_SIZE && memcmp(out, pl, pn) == 0,
        "pack gesture default round trip");
  memset(&d, 0xAA, sizeof d);
  mxp_gesture_config_defaults(&d);
  CHECK(d.hand_threshold == GESTURE_DEFAULT_HAND_THRESHOLD && d.gesture_threshold == 60 && d.max_hands == 1,
        "gesture defaults");
  pn = vec_payload("set_func_config_gesture_request", pl, sizeof pl);
  CHECK(pn == MXP_FUNC_CONFIG_SIZE && mxp_unpack_gesture_config(pl, (uint16_t)pn, &g) &&
            gesture_cfg_eq(&g, &VEC_P_GESTURE_CONFIG_SET) && g.hand_threshold == 50 &&
            g.gesture_threshold == 70 && g.max_hands == 2,
        "unpack gesture set");
  memset(out, 0xEE, sizeof out);
  CHECK(mxp_pack_gesture_config(&g, out) == MXP_FUNC_CONFIG_SIZE && memcmp(out, pl, pn) == 0,
        "pack gesture set");
  pn = vec_payload("get_func_config_gesture_request", pl, sizeof pl);
  CHECK(pn == 1 && pl[0] == MXP_FUNC_GESTURE, "get gesture config request");
}

static void test_gesture_config_clamp(void) {
  uint8_t out[MXP_FUNC_CONFIG_SIZE];
  mxp_gesture_config_t g;
  const uint8_t in1[10] = {8, 1, 5, 150, 0, 9, 9, 9, 9, 9};
  CHECK(mxp_unpack_gesture_config(in1, 10, &g) && g.hand_threshold == 10 && g.gesture_threshold == 100 &&
            g.max_hands == 1,
        "gesture clamp 1");
  static const uint8_t exp1[10] = {8, 1, 10, 100, 1, 0, 0, 0, 0, 0};
  CHECK(mxp_pack_gesture_config(&g, out) == 10 && memcmp(out, exp1, 10) == 0, "gesture repack, reserved 0");
  const uint8_t in2[10] = {8, 1, 99, 0, 7, 0, 0, 0, 0, 0};
  CHECK(mxp_unpack_gesture_config(in2, 10, &g) && g.hand_threshold == 95 && g.gesture_threshold == 0 &&
            g.max_hands == 2,
        "gesture clamp 2");
}

static void test_gesture_config_reject(void) {
  uint8_t pl[16];
  mxp_gesture_config_t g;
  vec_payload("func_config_gesture_default", pl, sizeof pl);
  pl[10] = 0;
  CHECK(!mxp_unpack_gesture_config(pl, 9, &g) && !mxp_unpack_gesture_config(pl, 11, &g), "gesture length");
  pl[0] = 4;
  CHECK(!mxp_unpack_gesture_config(pl, 10, &g), "gesture wrong fn");
  pl[0] = 8;
  pl[1] = 2;
  CHECK(!mxp_unpack_gesture_config(pl, 10, &g), "gesture version 2");
  pl[1] = 0;
  CHECK(!mxp_unpack_gesture_config(pl, 10, &g), "gesture version 0");
}

static void test_gesture_results(void) {
  uint8_t pl[128], out[128];
  size_t pn = vec_payload("results_gesture_two", pl, sizeof pl);
  mxp_results_t r;
  CHECK(mxp_unpack_results(pl, (uint16_t)pn, &r), "unpack results_gesture_two");
  check_results_equal(&r, &VEC_P_RESULTS_GESTURE_TWO, "results_gesture_two");
  CHECK(r.function_id == MXP_FUNC_GESTURE && r.count == 2 && r.r[0].id == MXP_GESTURE_OK &&
            r.r[0].confidence == 92 && r.r[1].id == MXP_GESTURE_UNKNOWN && r.r[1].confidence == 81 &&
            r.r[0].extra_len == 0 && r.r[1].extra_len == 0,
        "gesture records");
  CHECK(mxp_pack_results(&VEC_P_RESULTS_GESTURE_TWO, out, sizeof out) == pn && memcmp(out, pl, pn) == 0,
        "pack results_gesture_two");
  mxp_info_t info;
  pn = vec_payload("get_info_response_gesture", pl, sizeof pl);
  CHECK(mxp_unpack_info(pl, (uint16_t)pn, &info) && info.function_count == 4 &&
            info.function_ids[0] == 1 && info.function_ids[3] == MXP_FUNC_GESTURE,
        "gesture info ids");
  CHECK(mxp_pack_info(&info, out, sizeof out) == pn && memcmp(out, pl, pn) == 0, "gesture info round trip");
}

static void test_learn_with_id(void) {
  uint8_t pl[16];
  mxp_learn_req_t l;
  size_t pn = vec_payload("learn_request_with_id", pl, sizeof pl);
  const mxp_learn_req_t *e = &VEC_P_LEARN_REQUEST_WITH_ID;
  CHECK(pn == 11 && mxp_unpack_learn(pl, 11, &l), "unpack learn with id");
  CHECK(l.function_id == 5 && l.x == 100 && l.y == 80 && l.w == 40 && l.h == 40 && l.has_id == 1 &&
            l.id == 1 && l.function_id == e->function_id && l.id == e->id && e->has_id == 1,
        "learn with id fields");
  memset(&l, 0xAA, sizeof l);
  CHECK(mxp_unpack_learn(pl, 9, &l) && l.has_id == 0 && l.id == 0 && l.x == 100, "9 bytes: no id");
  CHECK(!mxp_unpack_learn(pl, 10, &l) && !mxp_unpack_learn(pl, 12, &l), "learn length 10 / 12");
  CHECK(VEC_P_LEARN_REQUEST.has_id == 0, "old learn vector has no id");
}

static void test_line_extra(void) {
  uint8_t pl[128], out[128];
  size_t pn = vec_payload("results_line_one", pl, sizeof pl);
  mxp_results_t r;
  CHECK(mxp_unpack_results(pl, (uint16_t)pn, &r), "unpack results_line_one");
  check_results_equal(&r, &VEC_P_RESULTS_LINE_ONE, "results_line_one");
  CHECK(r.r[0].extra_len == MXP_LINE_EXTRA_SIZE, "line extra_len 14");
  CHECK(mxp_pack_results(&VEC_P_RESULTS_LINE_ONE, out, sizeof out) == pn && memcmp(out, pl, pn) == 0,
        "pack results_line_one");

  mxp_line_extra_t le;
  memset(&le, 0, sizeof le);
  CHECK(mxp_unpack_line_extra(&r.r[0], &le), "unpack line extra");
  const mxp_line_extra_t *e = &VEC_P_LINE_EXTRA_ONE;
  CHECK(le.angle_cdeg == -849 && le.offset_x == 20 && le.x0 == 180 && le.y0 == 231 && le.x1 == 160 &&
            le.y1 == 97 && le.flags == MXP_LINE_FLAG_CROSS,
        "line extra fields");
  CHECK(le.angle_cdeg == e->angle_cdeg && le.offset_x == e->offset_x && le.x0 == e->x0 && le.y0 == e->y0 &&
            le.x1 == e->x1 && le.y1 == e->y1 && le.flags == e->flags,
        "line extra matches vector");

  mxp_result_t packed;
  memset(&packed, 0xFF, sizeof packed);
  mxp_pack_line_extra(e, &packed);
  CHECK(packed.extra_len == MXP_LINE_EXTRA_SIZE &&
            memcmp(packed.extra, r.r[0].extra, MXP_LINE_EXTRA_SIZE) == 0 && packed.extra[13] == 0,
        "pack line extra bytes, reserved 0");

  mxp_result_t bad = r.r[0];
  bad.extra_len = 13;
  CHECK(!mxp_unpack_line_extra(&bad, &le), "extra_len 13 rejected");
  bad.extra_len = 0;
  CHECK(!mxp_unpack_line_extra(&bad, &le), "extra_len 0 rejected");
  bad.extra_len = 18;
  CHECK(!mxp_unpack_line_extra(&bad, &le), "extra_len 18 rejected");
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
  test_info_32();
  test_m6_codes();
  test_func_config_vectors();
  test_func_config_clamp();
  test_func_config_reject();
  test_gesture_config_vectors();
  test_gesture_config_clamp();
  test_gesture_config_reject();
  test_gesture_results();
  test_learn_with_id();
  test_line_extra();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
