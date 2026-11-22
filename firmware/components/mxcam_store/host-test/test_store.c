#include <stdio.h>
#include <string.h>

#include "mxcam_store.h"

static int fails, checks;
#define CHECK(c)                                          \
  do {                                                    \
    checks++;                                             \
    if (!(c)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      fails++;                                            \
    }                                                     \
  } while (0)

static uint8_t blob[MXST_BLOB_MAX + 16];

/* Func config records serialize writes: line, track, gesture, shape, motion (M7). */
#define N_FCFG 5

static void test_defaults(void) {
  mxs_data_t d;
  memset(&d, 0xAB, sizeof(d));
  mxst_defaults(&d);
  CHECK(d.cfg.tolerance == 30 && d.cfg.min_area == 100 && d.cfg.color_max == 4);
  CHECK(d.cfg.color_names == 1 && d.cfg.tag_names == 1);
  CHECK(d.cfg.face_threshold == 60 && d.cfg.face_max == 4);
  CHECK(d.cfg.uart_baud == 115200 && d.cfg.i2c_address == 0x5A && d.cfg.led == 50);
  CHECK(d.cfg.dirty == 0 && d.n_colors == 0 && d.n_names == 0);
  mxp_gesture_config_t g;
  mxp_gesture_config_defaults(&g);
  CHECK(d.gesture_cfg.hand_threshold == g.hand_threshold &&
        d.gesture_cfg.gesture_threshold == g.gesture_threshold && d.gesture_cfg.max_hands == g.max_hands);
}

static void fill_full(mxs_data_t *d) {
  mxst_defaults(d);
  d->cfg.tolerance = 77;
  d->cfg.min_area = 1234;
  d->cfg.uart_baud = 9600;
  d->cfg.dirty = 1; /* must not be persisted */
  d->n_colors = 7;
  for (int i = 0; i < 7; i++) {
    d->colors[i].id = (uint8_t)(i + 1);
    d->colors[i].rgb[0] = (uint8_t)(10 * i);
    d->colors[i].rgb[1] = (uint8_t)(20 + i);
    d->colors[i].rgb[2] = (uint8_t)(255 - i);
  }
  const char *cjk = "\xE7\xB4\x85\xE8\x89\xB2"; /* 紅色 */
  CHECK(mxst_set_name(d, MXST_FN_COLOR, 1, cjk, 6));
  for (uint16_t i = 0; i < 31; i++) {
    char nm[MXP_NAME_MAX];
    memset(nm, 'a' + (i % 26), sizeof(nm));
    CHECK(mxst_set_name(d, MXST_FN_TAG, (uint16_t)(i * 18), nm, MXP_NAME_MAX));
  }
  CHECK(d->n_names == 32);
}

static void test_roundtrip(void) {
  mxs_data_t a, b;
  fill_full(&a);
  size_t n = mxst_serialize(&a, blob, sizeof(blob));
  CHECK(n > 0 && n <= MXST_BLOB_MAX);
  mxst_defaults(&b);
  CHECK(mxst_deserialize(blob, n, &b));
  CHECK(b.cfg.tolerance == 77 && b.cfg.min_area == 1234 && b.cfg.uart_baud == 9600);
  CHECK(b.cfg.dirty == 0);
  CHECK(b.n_colors == 7 && memcmp(b.colors, a.colors, sizeof(a.colors)) == 0);
  CHECK(b.n_names == 32);
  uint8_t l = 0;
  const char *s = mxst_get_name(&b, MXST_FN_COLOR, 1, &l);
  CHECK(s && l == 6 && memcmp(s, "\xE7\xB4\x85\xE8\x89\xB2", 6) == 0 && s[6] == 0);
  s = mxst_get_name(&b, MXST_FN_TAG, 18 * 30, &l);
  CHECK(s && l == MXP_NAME_MAX && s[0] == 'a' + (30 % 26));
  /* 7 colours + 32 names of 48 bytes hits the documented maximum exactly */
  memset(a.names, 0, sizeof(a.names));
  a.n_names = 0;
  for (uint16_t i = 0; i < 32; i++) {
    char nm[MXP_NAME_MAX];
    memset(nm, 'z', sizeof(nm));
    CHECK(mxst_set_name(&a, i == 0 ? MXST_FN_COLOR : MXST_FN_TAG, i == 0 ? 1 : i, nm, MXP_NAME_MAX));
  }
  size_t nmax = mxst_serialize(&a, blob, sizeof(blob));
  /* the v1 maximum (1719) + n_fcfg + the five records written (line, track,
   * gesture, shape, motion) + n_lrn (nothing learned) */
  CHECK(nmax == 1719 + 1 + N_FCFG * MXP_FUNC_CONFIG_SIZE + 1 && nmax <= (size_t)MXST_BLOB_MAX);
  /* too-small cap */
  CHECK(mxst_serialize(&a, blob, n - 1) == 0);
  /* empty data round trips too */
  mxst_defaults(&a);
  n = mxst_serialize(&a, blob, sizeof(blob));
  CHECK(n > 0 && mxst_deserialize(blob, n, &b) && b.n_colors == 0 && b.n_names == 0);
}

static void fix_crc(uint8_t *p, size_t n) {
  uint16_t c = mxp_crc16(p, n - 2);
  p[n - 2] = (uint8_t)(c & 0xFF);
  p[n - 1] = (uint8_t)(c >> 8);
}

/* Mutate a small valid blob, re-CRC (unless raw), expect rejection and *out untouched. */
static void expect_reject(const char *what, const uint8_t *src, size_t n) {
  mxs_data_t sentinel, probe;
  mxst_defaults(&sentinel);
  sentinel.cfg.tolerance = 99;
  sentinel.n_colors = 3;
  probe = sentinel;
  bool ok = mxst_deserialize(src, n, &probe);
  if (ok || memcmp(&probe, &sentinel, sizeof(probe)) != 0) {
    printf("FAIL reject case: %s\n", what);
    fails++;
  }
}

static void test_corruption(void) {
  mxs_data_t a;
  mxst_defaults(&a);
  a.n_colors = 2;
  a.colors[0].id = 1;
  a.colors[1].id = 2;
  mxst_set_name(&a, MXST_FN_COLOR, 1, "red", 3);
  size_t n = mxst_serialize(&a, blob, sizeof(blob));
  CHECK(n > 0);
  uint8_t t[MXST_BLOB_MAX + 16];
  const size_t o_nc = 4 + 1 + MXP_CONFIG_SIZE;
  const size_t o_nn = o_nc + 1 + 2 * 4;
  const size_t o_nm = o_nn + 1;
  const size_t o_fc = o_nm + 4 + 3; /* after the one name ("red") */

#define MUT(label, stmt, recrc)    \
  memcpy(t, blob, n);              \
  stmt;                            \
  if (recrc) fix_crc(t, n);        \
  expect_reject(label, t, n)

  MUT("bad magic", t[0] = 'X', 1);
  MUT("version 0", t[4] = 0, 1);
  MUT("version 4", t[4] = 4, 1);
  MUT("v1 with a record list", t[4] = 1, 1);
  MUT("v2 with a learned list", t[4] = 2, 1);
  MUT("n_fcfg > 16", t[o_fc] = 17, 1);
  MUT("n_fcfg too many for data", t[o_fc] = N_FCFG + 1, 1);
  MUT("n_fcfg too few", t[o_fc] = N_FCFG - 1, 1);
  MUT("n_lrn too many for data", t[n - 3] = 1, 1);
  MUT("bad crc", t[n - 1] ^= 0x55, 0);
  MUT("bit flip", t[10] ^= 1, 0);
  MUT("n_colors > 7", t[o_nc] = 8, 1);
  MUT("dup colour id", t[o_nc + 1 + 4] = 1, 1);
  MUT("colour id 0", t[o_nc + 1] = 0, 1);
  MUT("colour id 8", t[o_nc + 1] = 8, 1);
  MUT("n_names > 32", t[o_nn] = 33, 1);
  MUT("name fn 4", t[o_nm] = 4, 1);
  MUT("name fn 0", t[o_nm] = 0, 1);
  MUT("colour name id 8", t[o_nm + 1] = 8, 1);
  MUT("name len > 48", t[o_nm + 3] = 49, 1);
  MUT("name len 0", t[o_nm + 3] = 0, 1);
  MUT("name len overruns", t[o_nm + 3] = 40, 1);
  MUT("n_names too many for data", t[o_nn] = 2, 1);
  MUT("n_names too few (trailing)", t[o_nn] = 0, 1);
  MUT("tag id 587", (t[o_nm] = 2, t[o_nm + 1] = 0x4B, t[o_nm + 2] = 0x02), 1);
  MUT("config version", t[5] = 2, 1);

  /* truncation at every length */
  for (size_t k = 0; k < n; k++) {
    memcpy(t, blob, n);
    expect_reject("truncated", t, k);
  }
  /* trailing byte (valid CRC over the longer body) */
  memcpy(t, blob, n - 2);
  t[n - 2] = 0;
  n++;
  { /* rebuild: body + extra byte + new crc */
    uint8_t u[MXST_BLOB_MAX + 16];
    memcpy(u, t, n - 2);
    u[n - 2] = 0;
    u[n - 1] = 0;
    fix_crc(u, n);
    expect_reject("trailing byte", u, n);
  }
  expect_reject("null", NULL, 0);
}

static void test_names(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  uint8_t l = 99;
  CHECK(mxst_get_name(&d, MXST_FN_TAG, 5, &l) == NULL);
  CHECK(mxst_set_name(&d, MXST_FN_TAG, 5, "door", 4));
  CHECK(d.n_names == 1);
  const char *s = mxst_get_name(&d, MXST_FN_TAG, 5, &l);
  CHECK(s && l == 4 && strcmp(s, "door") == 0);
  /* replace */
  CHECK(mxst_set_name(&d, MXST_FN_TAG, 5, "window!", 7));
  CHECK(d.n_names == 1);
  s = mxst_get_name(&d, MXST_FN_TAG, 5, &l);
  CHECK(s && l == 7 && strcmp(s, "window!") == 0);
  /* same id, different function is a different key */
  CHECK(mxst_set_name(&d, MXST_FN_GESTURE, 5, "bob", 3));
  CHECK(d.n_names == 2);
  /* remove */
  CHECK(mxst_set_name(&d, MXST_FN_TAG, 5, NULL, 0));
  CHECK(d.n_names == 1 && mxst_get_name(&d, MXST_FN_TAG, 5, NULL) == NULL);
  CHECK(mxst_get_name(&d, MXST_FN_GESTURE, 5, NULL) != NULL);
  CHECK(mxst_set_name(&d, MXST_FN_TAG, 9, NULL, 0)); /* removing absent is fine */
  /* invalid */
  CHECK(!mxst_set_name(&d, 4, 1, "x", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_COLOR, 0, "x", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_COLOR, 8, "x", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_TAG, 587, "x", 1));
  CHECK(mxst_set_name(&d, MXST_FN_TAG, 586, "x", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_TAG, 1, "x", MXP_NAME_MAX + 1));
  /* full */
  mxst_defaults(&d);
  for (uint16_t i = 0; i < MXST_MAX_NAMES; i++) CHECK(mxst_set_name(&d, MXST_FN_TAG, i, "n", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_TAG, 100, "n", 1));
  CHECK(mxst_set_name(&d, MXST_FN_TAG, 3, "replaced", 8)); /* replace still ok when full */
  CHECK(d.n_names == MXST_MAX_NAMES);
}

static void test_forget(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  d.n_colors = 3;
  for (int i = 0; i < 3; i++) d.colors[i].id = (uint8_t)(i + 1);
  mxst_set_name(&d, MXST_FN_COLOR, 1, "a", 1);
  mxst_set_name(&d, MXST_FN_COLOR, 2, "b", 1);
  mxst_set_name(&d, MXST_FN_TAG, 2, "tag", 3);
  mxst_forget_color(&d, 2);
  CHECK(d.n_colors == 2 && d.colors[0].id == 1 && d.colors[1].id == 3);
  CHECK(mxst_get_name(&d, MXST_FN_COLOR, 2, NULL) == NULL);
  CHECK(mxst_get_name(&d, MXST_FN_COLOR, 1, NULL) != NULL);
  CHECK(mxst_get_name(&d, MXST_FN_TAG, 2, NULL) != NULL); /* tag name untouched */
  mxst_forget_color(&d, 0xFF);
  CHECK(d.n_colors == 0 && d.n_names == 1);
  CHECK(mxst_get_name(&d, MXST_FN_TAG, 2, NULL) != NULL);
  CHECK(mxst_get_name(&d, MXST_FN_COLOR, 1, NULL) == NULL);
}

/* serialize must refuse anything deserialize would reject (otherwise a save
 * silently loses everything at the next boot). */
static void expect_no_serialize(const char *what, const mxs_data_t *d) {
  if (mxst_serialize(d, blob, sizeof(blob)) != 0) {
    printf("FAIL serialize accepted: %s\n", what);
    fails++;
  }
}

static void test_serialize_validates(void) {
  mxs_data_t base, d;
  mxst_defaults(&base);
  base.n_colors = 2;
  base.colors[0].id = 1;
  base.colors[1].id = 2;
  CHECK(mxst_set_name(&base, MXST_FN_COLOR, 1, "red", 3));
  CHECK(mxst_set_name(&base, MXST_FN_TAG, 5, "door", 4));
  CHECK(mxst_serialize(&base, blob, sizeof(blob)) > 0);

  d = base; d.colors[1].id = 1;                    expect_no_serialize("dup colour id", &d);
  d = base; d.colors[0].id = 0;                    expect_no_serialize("colour id 0", &d);
  d = base; d.colors[0].id = 8;                    expect_no_serialize("colour id 8", &d);
  d = base; d.n_colors = 8;                        expect_no_serialize("n_colors 8", &d);
  d = base; d.names[0].len = 0;                    expect_no_serialize("name len 0", &d);
  d = base; d.names[0].len = MXP_NAME_MAX + 1;     expect_no_serialize("name len 49", &d);
  d = base; d.names[0].fn = 4;                     expect_no_serialize("name fn 4", &d);
  d = base; d.names[0].id = 8;                     expect_no_serialize("colour name id 8", &d);
  d = base; d.names[1].id = 587;                   expect_no_serialize("tag name id 587", &d);
  d = base; d.names[1] = d.names[0];               expect_no_serialize("dup name key", &d);
  d = base; d.names[1].name[0] = (char)0xC3;       expect_no_serialize("bad utf8 name", &d);
  d = base; d.n_names = 33;                        expect_no_serialize("n_names 33", &d);
  d = base; d.names[1].fn = 8; d.names[1].id = 10; expect_no_serialize("gesture name id 10", &d);
  d = base; d.gesture_cfg.hand_threshold = 9;      expect_no_serialize("hand_threshold 9", &d);
  d = base; d.gesture_cfg.hand_threshold = 96;     expect_no_serialize("hand_threshold 96", &d);
  d = base; d.gesture_cfg.gesture_threshold = 101; expect_no_serialize("gesture_threshold 101", &d);
  d = base; d.gesture_cfg.max_hands = 0;           expect_no_serialize("max_hands 0", &d);
  d = base; d.gesture_cfg.max_hands = 3;           expect_no_serialize("max_hands 3", &d);
}

static void test_deserialize_rejects_bad_utf8(void) {
  mxs_data_t a, b;
  mxst_defaults(&a);
  CHECK(mxst_set_name(&a, MXST_FN_TAG, 5, "ab", 2));
  size_t n = mxst_serialize(&a, blob, sizeof(blob));
  CHECK(n > 0);
  /* the name bytes are just before the func config list (n_fcfg + 5 records),
   * the empty learned list (n_lrn) and the CRC */
  const size_t tail = 1 + N_FCFG * MXP_FUNC_CONFIG_SIZE + 1 + 2;
  CHECK(blob[n - tail - 2] == 'a' && blob[n - tail - 1] == 'b');
  blob[n - tail - 2] = 0xC3;
  blob[n - tail - 1] = 0x28; /* C3 28: bad continuation */
  fix_crc(blob, n);
  CHECK(!mxst_deserialize(blob, n, &b));
}

static void test_set_name_utf8(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  CHECK(!mxst_set_name(&d, MXST_FN_TAG, 1, "\xC3", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_TAG, 1, "\xED\xA0\x80", 3)); /* surrogate */
  CHECK(d.n_names == 0);
  CHECK(mxst_set_name(&d, MXST_FN_TAG, 1, "\xC3\xA9", 2));
}

static void test_learn_color(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  const uint8_t red[3] = {230, 20, 20}, red2[3] = {220, 30, 25}, green[3] = {20, 200, 40},
                blue[3] = {20, 40, 220};
  CHECK(mxst_learn_color(&d, red, 30) == 1);
  CHECK(d.n_colors == 1 && d.colors[0].id == 1 && memcmp(d.colors[0].rgb, red, 3) == 0);
  /* same colour (within tolerance) updates the existing id */
  CHECK(mxst_learn_color(&d, red2, 30) == 1);
  CHECK(d.n_colors == 1 && memcmp(d.colors[0].rgb, red2, 3) == 0);
  CHECK(mxst_learn_color(&d, green, 30) == 2);
  CHECK(mxst_learn_color(&d, blue, 30) == 3);
  /* the lowest free id is reused and the list stays sorted by id */
  mxst_forget_color(&d, 2);
  CHECK(mxst_learn_color(&d, green, 30) == 2);
  CHECK(d.n_colors == 3 && d.colors[0].id == 1 && d.colors[1].id == 2 && d.colors[2].id == 3);
  /* fill up to 7 with distinct hues, then FULL */
  static const uint8_t more[4][3] = {{230, 230, 20}, {20, 220, 220}, {220, 20, 220}, {240, 120, 20}};
  for (int i = 0; i < 4; i++) CHECK(mxst_learn_color(&d, more[i], 5) == 4 + i);
  CHECK(d.n_colors == 7);
  const uint8_t violet[3] = {120, 20, 230};
  CHECK(mxst_learn_color(&d, violet, 5) == MXST_LEARN_FULL);
  CHECK(d.n_colors == 7);
  /* full, but an existing colour can still be refreshed */
  CHECK(mxst_learn_color(&d, red, 5) == 1);
  /* grey never matches anything, and is refused */
  const uint8_t grey[3] = {128, 128, 128};
  mxst_forget_color(&d, 0xFF);
  CHECK(mxst_learn_color(&d, grey, 30) == MXST_LEARN_NO_COLOR);
  CHECK(d.n_colors == 0);
}

static void test_forget_names(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  CHECK(mxst_set_name(&d, MXST_FN_TAG, 3, "a", 1));
  CHECK(mxst_set_name(&d, MXST_FN_TAG, 9, "b", 1));
  CHECK(mxst_set_name(&d, MXST_FN_COLOR, 1, "c", 1));
  mxst_forget_names(&d, MXST_FN_TAG, 3);
  CHECK(d.n_names == 2 && mxst_get_name(&d, MXST_FN_TAG, 3, NULL) == NULL);
  mxst_forget_names(&d, MXST_FN_TAG, 77); /* absent: no-op */
  CHECK(d.n_names == 2);
  mxst_forget_names(&d, MXST_FN_TAG, 0xFFFF);
  CHECK(d.n_names == 1 && mxst_get_name(&d, MXST_FN_COLOR, 1, NULL) != NULL);
}

/* ---- blob v2 (M8a): func config records ---- */

/* A v1 blob written by the v1 mxst_serialize (commit c92f6b0) before blob v2
 * existed: tolerance 45, colours 1 (230,20,20) and 3 (20,40,220), AprilTag 12
 * named "門口". Never change it: v2+ firmware must keep reading it (M6 Task 4
 * reuses it for blob v3). */
static const uint8_t k_v1_fixture[45] = {
    0x4d, 0x58, 0x53, 0x54, 0x01, 0x01, 0x2d, 0x64, 0x00, 0x04, 0x01, 0x01,
    0x3c, 0x04, 0x00, 0xc2, 0x01, 0x00, 0x5a, 0x32, 0x00, 0x00, 0x00, 0x02,
    0x01, 0xe6, 0x14, 0x14, 0x03, 0x14, 0x28, 0xdc, 0x01, 0x02, 0x0c, 0x00,
    0x06, 0xe9, 0x96, 0x80, 0xe5, 0x8f, 0xa3, 0xf6, 0x9c,
};

static bool gesture_is(const mxp_gesture_config_t *g, uint8_t hand, uint8_t gest, uint8_t max) {
  return g->hand_threshold == hand && g->gesture_threshold == gest && g->max_hands == max;
}

static bool gesture_is_default(const mxp_gesture_config_t *g) {
  mxp_gesture_config_t def;
  mxp_gesture_config_defaults(&def);
  return gesture_is(g, def.hand_threshold, def.gesture_threshold, def.max_hands);
}

static void test_deserialize_v1_fixture(void) {
  mxs_data_t d;
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(k_v1_fixture, sizeof(k_v1_fixture), &d));
  CHECK(d.cfg.tolerance == 45 && d.cfg.min_area == 100 && d.cfg.uart_baud == 115200 && d.cfg.dirty == 0);
  CHECK(d.n_colors == 2);
  CHECK(d.colors[0].id == 1 && d.colors[0].rgb[0] == 230 && d.colors[0].rgb[1] == 20 && d.colors[0].rgb[2] == 20);
  CHECK(d.colors[1].id == 3 && d.colors[1].rgb[0] == 20 && d.colors[1].rgb[1] == 40 && d.colors[1].rgb[2] == 220);
  uint8_t l = 0;
  const char *s = mxst_get_name(&d, MXST_FN_TAG, 12, &l);
  CHECK(d.n_names == 1 && s && l == 6 && memcmp(s, "\xE9\x96\x80\xE5\x8F\xA3", 6) == 0);
  /* no record list in v1: gesture parameters are the defaults (40/60/1 today;
   * hand_threshold is provisional, so compare with the defaults function) */
  CHECK(gesture_is_default(&d.gesture_cfg));
  CHECK(d.gesture_cfg.gesture_threshold == 60 && d.gesture_cfg.max_hands == 1);
}

static void test_roundtrip_gesture(void) {
  mxs_data_t a, b;
  mxst_defaults(&a);
  a.gesture_cfg.hand_threshold = 50;
  a.gesture_cfg.gesture_threshold = 70;
  a.gesture_cfg.max_hands = 2;
  CHECK(mxst_set_name(&a, MXST_FN_GESTURE, 7, "\xE5\x89\x8D\xE9\x80\xB2", 6)); /* 前進 */
  size_t n = mxst_serialize(&a, blob, sizeof(blob));
  CHECK(n > 2 + 1 + N_FCFG * MXP_FUNC_CONFIG_SIZE + 1 && blob[4] == 3);
  /* tail: n_fcfg 5, the SET_FUNC_CONFIG packets of line, track, gesture,
   * shape and motion, then n_lrn 0 (nothing learned) and the CRC */
  uint8_t rec[MXP_FUNC_CONFIG_SIZE];
  const size_t o_fc = n - 2 - 1 - N_FCFG * MXP_FUNC_CONFIG_SIZE - 1;
  CHECK(blob[o_fc] == N_FCFG && blob[n - 3] == 0);
  mxp_pack_line_config(&a.line_cfg, rec);
  CHECK(memcmp(blob + o_fc + 1, rec, sizeof(rec)) == 0);
  mxp_pack_track_config(&a.track_cfg, rec);
  CHECK(memcmp(blob + o_fc + 1 + MXP_FUNC_CONFIG_SIZE, rec, sizeof(rec)) == 0);
  mxp_pack_gesture_config(&a.gesture_cfg, rec);
  CHECK(memcmp(blob + o_fc + 1 + 2 * MXP_FUNC_CONFIG_SIZE, rec, sizeof(rec)) == 0);
  mxp_pack_shape_config(&a.shape_cfg, rec);
  CHECK(memcmp(blob + o_fc + 1 + 3 * MXP_FUNC_CONFIG_SIZE, rec, sizeof(rec)) == 0);
  mxp_pack_motion_config(&a.motion_cfg, rec);
  CHECK(memcmp(blob + o_fc + 1 + 4 * MXP_FUNC_CONFIG_SIZE, rec, sizeof(rec)) == 0);
  mxst_defaults(&b);
  CHECK(mxst_deserialize(blob, n, &b));
  CHECK(gesture_is(&b.gesture_cfg, 50, 70, 2));
  uint8_t l = 0;
  const char *s = mxst_get_name(&b, MXST_FN_GESTURE, 7, &l);
  CHECK(s && l == 6 && memcmp(s, "\xE5\x89\x8D\xE9\x80\xB2", 6) == 0);
  CHECK(memcmp(&a, &b, sizeof(a)) == 0);
}

/* v2 blob = the v1 fixture's content (version 2) + n_list + n_real records + CRC. */
static size_t build_v2(const uint8_t (*recs)[MXP_FUNC_CONFIG_SIZE], uint8_t n_list, uint8_t n_real,
                       uint8_t *out) {
  size_t o = sizeof(k_v1_fixture) - 2;
  memcpy(out, k_v1_fixture, o);
  out[4] = 2;
  out[o++] = n_list;
  for (int i = 0; i < n_real; i++) {
    memcpy(out + o, recs[i], MXP_FUNC_CONFIG_SIZE);
    o += MXP_FUNC_CONFIG_SIZE;
  }
  o += 2;
  fix_crc(out, o);
  return o;
}

static void test_v2_skips_unknown_record(void) {
  static const uint8_t recs[3][MXP_FUNC_CONFIG_SIZE] = {
      {4, 1, 1, 0, 60, 8, 4, 80, 30, 0}, /* line, mode 1 */
      {30, 1, 1, 2, 3, 4, 5, 6, 7, 8},   /* unknown function */
      {8, 1, 50, 70, 2, 0, 0, 0, 0, 0},  /* gesture v1 */
  };
  uint8_t t[MXST_BLOB_MAX + 16];
  size_t n = build_v2(recs, 3, 3, t);
  mxs_data_t d;
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(t, n, &d));
  CHECK(gesture_is(&d.gesture_cfg, 50, 70, 2));
  CHECK(d.line_cfg.mode == 1 && d.line_cfg.roi_pct == 60);
  CHECK(d.cfg.tolerance == 45 && d.n_colors == 2 && d.n_names == 1);
  /* reserved bytes are ignored, values are clamped like SET_FUNC_CONFIG */
  static const uint8_t wild[1][MXP_FUNC_CONFIG_SIZE] = {{8, 1, 5, 150, 0, 9, 9, 9, 9, 9}};
  n = build_v2(wild, 1, 1, t);
  CHECK(mxst_deserialize(t, n, &d) && gesture_is(&d.gesture_cfg, 10, 100, 1));
  /* an empty record list is fine */
  n = build_v2(NULL, 0, 0, t);
  CHECK(mxst_deserialize(t, n, &d) && gesture_is_default(&d.gesture_cfg));
}

static void test_v2_gesture_bad_version_skipped(void) {
  static const uint8_t recs[1][MXP_FUNC_CONFIG_SIZE] = {{8, 2, 50, 70, 2, 0, 0, 0, 0, 0}};
  uint8_t t[MXST_BLOB_MAX + 16];
  size_t n = build_v2(recs, 1, 1, t);
  mxs_data_t d;
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(t, n, &d));
  CHECK(gesture_is_default(&d.gesture_cfg));
  CHECK(d.cfg.tolerance == 45 && d.n_colors == 2 && d.n_names == 1);
}

static void test_v2_rejects_duplicate_gesture(void) {
  static const uint8_t recs[2][MXP_FUNC_CONFIG_SIZE] = {
      {8, 1, 50, 70, 2, 0, 0, 0, 0, 0},
      {8, 1, 30, 60, 1, 0, 0, 0, 0, 0},
  };
  uint8_t t[MXST_BLOB_MAX + 16];
  size_t n = build_v2(recs, 2, 2, t);
  expect_reject("duplicate gesture record", t, n);
  /* also when one of them has a layout version this firmware cannot read */
  static const uint8_t mixed[2][MXP_FUNC_CONFIG_SIZE] = {
      {8, 2, 50, 70, 2, 0, 0, 0, 0, 0},
      {8, 1, 30, 60, 1, 0, 0, 0, 0, 0},
  };
  n = build_v2(mixed, 2, 2, t);
  expect_reject("duplicate gesture record (one unreadable)", t, n);
  /* unknown functions may repeat (they are skipped) */
  static const uint8_t unk[2][MXP_FUNC_CONFIG_SIZE] = {
      {30, 1, 0, 0, 0, 0, 0, 0, 0, 0},
      {30, 1, 0, 0, 0, 0, 0, 0, 0, 0},
  };
  mxs_data_t d;
  n = build_v2(unk, 2, 2, t);
  CHECK(mxst_deserialize(t, n, &d));
}

static void test_v2_rejects_too_many_records(void) {
  static uint8_t recs[MXST_MAX_FCFG + 1][MXP_FUNC_CONFIG_SIZE];
  for (int i = 0; i <= MXST_MAX_FCFG; i++) {
    memset(recs[i], 0, MXP_FUNC_CONFIG_SIZE);
    recs[i][0] = (uint8_t)(100 + i); /* distinct unknown functions */
    recs[i][1] = 1;
  }
  static uint8_t t[MXST_BLOB_MAX + 16 + MXP_FUNC_CONFIG_SIZE];
  size_t n = build_v2((const uint8_t(*)[MXP_FUNC_CONFIG_SIZE])recs, MXST_MAX_FCFG + 1, MXST_MAX_FCFG + 1, t);
  expect_reject("n_fcfg 17", t, n);
  mxs_data_t d;
  n = build_v2((const uint8_t(*)[MXP_FUNC_CONFIG_SIZE])recs, MXST_MAX_FCFG, MXST_MAX_FCFG, t);
  CHECK(mxst_deserialize(t, n, &d)); /* 16 is the limit */
}

/* A valid object model without running the tracker: tw x th template with a
 * pattern, box 2x the template (in 320 px), centred at (cx, cy). */
static void make_model(mxk_model_t *m, uint8_t tw, uint8_t th, int16_t cx, int16_t cy) {
  memset(m, 0, sizeof(*m));
  m->tw = tw;
  m->th = th;
  m->box_w = (uint16_t)(tw * 2);
  m->box_h = (uint16_t)(th * 2);
  m->cx = cx;
  m->cy = cy;
  m->rgb[0] = 200;
  m->rgb[1] = 40;
  m->rgb[2] = 30;
  for (int i = 0; i < MXK_HIST_BINS; i++) m->hist[i] = (uint16_t)(i * 1000 + 7);
  for (int i = 0; i < tw * th; i++) m->tmpl[i] = (uint8_t)(i * 37 + 11);
}

/* QR content of n bytes: seed, seed + 1, ... */
static void qr_bytes(uint8_t *p, size_t n, uint8_t seed) {
  for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i);
}

static void test_serialize_max_fits(void) {
  /* v2 maximum (1880) + n_lrn + line record (4 + 3) + object record (4 + 77 + 32 x 32)
   * + QR record (4 + 1 + 8 x (4 + 128)) */
  CHECK(MXST_QR_REC_MAX == 1057);
  CHECK(MXST_BLOB_MAX == 1880 + 1 + 4 + 3 + 4 + 77 + MXK_TMPL_MAX * MXK_TMPL_MAX + 4 + MXST_QR_REC_MAX);
  CHECK(MXST_BLOB_MAX == 4054);
  static mxs_data_t a, b;
  mxst_defaults(&a);
  a.n_colors = 7;
  for (int i = 0; i < 7; i++) a.colors[i].id = (uint8_t)(i + 1);
  a.gesture_cfg.max_hands = 2;
  a.line_cfg.bands = 12;
  a.track_cfg.adapt = 50;
  const uint8_t teal[3] = {20, 200, 180};
  CHECK(mxst_set_line_color(&a, teal));
  mxk_model_t m;
  make_model(&m, MXK_TMPL_MAX, MXK_TMPL_MAX, 160, 120);
  CHECK(mxst_set_object(&a, &m));
  char nm[MXP_NAME_MAX];
  memset(nm, 'q', sizeof(nm));
  CHECK(mxst_set_name(&a, MXST_FN_TRACK, 1, nm, MXP_NAME_MAX));
  for (uint16_t i = 1; i < MXST_MAX_NAMES; i++) CHECK(mxst_set_name(&a, MXST_FN_TAG, i, nm, MXP_NAME_MAX));
  /* 8 QR codes of 128 stored bytes (content longer than 128) */
  uint8_t q[MXP_RESULT_DATA_MAX];
  for (int i = 0; i < MXST_MAX_QR; i++) {
    qr_bytes(q, sizeof(q), (uint8_t)(i * 7));
    CHECK(mxst_learn_qr(&a, q, MXP_RESULT_DATA_MAX, (uint16_t)(300 + i)) == i + 1);
  }
  a.shape_cfg.max_results = 8;
  a.motion_cfg.threshold = 100;
  size_t n = mxst_serialize(&a, blob, sizeof(blob));
  /* everything this firmware writes: v1 max + 5 func configs + the three learned records */
  CHECK(n == 1719 + 1 + N_FCFG * MXP_FUNC_CONFIG_SIZE + 1 + 4 + 3 + 4 + 77 + 1024 + 4 + 1057);
  CHECK(n > 0 && n <= (size_t)MXST_BLOB_MAX);
  CHECK(mxst_deserialize(blob, n, &b) && memcmp(&a, &b, sizeof(a)) == 0);
  CHECK(b.n_qr == MXST_MAX_QR && b.qr[7].total_len == 307 && b.qr[7].len == 128 && b.qr[7].bytes[1] == 50);
}

/* ---- blob v3 (M6): line / track func configs, learned line colour and object ---- */

/* A v2 blob written by the v2 mxst_serialize (commit 3a0587d, M8a) before blob
 * v3 existed: tolerance 45, colours 1 (230,20,20) and 2 (20,40,220), AprilTag
 * 12 named "門口", gesture 6 named "讚", gesture config 30 / 70 / 2. Never
 * change it: v3+ firmware must keep reading it. */
static const uint8_t k_v2_fixture[63] = {
    0x4d, 0x58, 0x53, 0x54, 0x02, 0x01, 0x2d, 0x64, 0x00, 0x04, 0x01, 0x01,
    0x3c, 0x04, 0x00, 0xc2, 0x01, 0x00, 0x5a, 0x32, 0x00, 0x00, 0x00, 0x02,
    0x01, 0xe6, 0x14, 0x14, 0x02, 0x14, 0x28, 0xdc, 0x02, 0x02, 0x0c, 0x00,
    0x06, 0xe9, 0x96, 0x80, 0xe5, 0x8f, 0xa3, 0x08, 0x06, 0x00, 0x03, 0xe8,
    0xae, 0x9a, 0x01, 0x08, 0x01, 0x1e, 0x46, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x96, 0x2b,
};

static bool line_cfg_is_default(const mxp_line_config_t *c) {
  mxp_line_config_t def;
  mxp_line_config_defaults(&def);
  uint8_t a[MXP_FUNC_CONFIG_SIZE], b[MXP_FUNC_CONFIG_SIZE];
  mxp_pack_line_config(c, a);
  mxp_pack_line_config(&def, b);
  return memcmp(a, b, sizeof(a)) == 0;
}

static bool track_cfg_is_default(const mxp_track_config_t *c) {
  mxp_track_config_t def;
  mxp_track_config_defaults(&def);
  uint8_t a[MXP_FUNC_CONFIG_SIZE], b[MXP_FUNC_CONFIG_SIZE];
  mxp_pack_track_config(c, a);
  mxp_pack_track_config(&def, b);
  return memcmp(a, b, sizeof(a)) == 0;
}

static void test_v1_fixture_new_fields_default(void) {
  mxs_data_t d;
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(k_v1_fixture, sizeof(k_v1_fixture), &d));
  CHECK(line_cfg_is_default(&d.line_cfg) && track_cfg_is_default(&d.track_cfg));
  CHECK(d.line_has_color == 0 && d.has_object == 0);
}

static void test_deserialize_v2_fixture(void) {
  mxs_data_t d;
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(k_v2_fixture, sizeof(k_v2_fixture), &d));
  CHECK(d.cfg.tolerance == 45 && d.cfg.min_area == 100 && d.cfg.dirty == 0);
  CHECK(d.n_colors == 2 && d.colors[0].id == 1 && d.colors[0].rgb[0] == 230 && d.colors[1].id == 2 &&
        d.colors[1].rgb[2] == 220);
  uint8_t l = 0;
  const char *s = mxst_get_name(&d, MXST_FN_TAG, 12, &l);
  CHECK(d.n_names == 2 && s && l == 6 && memcmp(s, "\xE9\x96\x80\xE5\x8F\xA3", 6) == 0);
  s = mxst_get_name(&d, MXST_FN_GESTURE, 6, &l);
  CHECK(s && l == 3 && memcmp(s, "\xE8\xAE\x9A", 3) == 0);
  CHECK(gesture_is(&d.gesture_cfg, 30, 70, 2));
  /* new in v3: defaults, nothing learned */
  CHECK(line_cfg_is_default(&d.line_cfg) && track_cfg_is_default(&d.track_cfg));
  CHECK(d.line_has_color == 0 && d.has_object == 0);
}

static void test_roundtrip_v3_full(void) {
  mxs_data_t a, b;
  mxst_defaults(&a);
  a.line_cfg.threshold = 90;
  a.line_cfg.roi_pct = 40;
  a.line_cfg.bands = 6;
  a.line_cfg.min_width = 10;
  a.line_cfg.max_width = 120;
  a.line_cfg.color_tolerance = 44;
  a.track_cfg.threshold = 65;
  a.track_cfg.adapt = 0;
  a.track_cfg.reacquire = 0;
  a.track_cfg.scale = 0;
  const uint8_t yellow[3] = {230, 210, 30};
  CHECK(mxst_set_line_color(&a, yellow));
  CHECK(a.line_cfg.mode == MXP_LINE_MODE_LEARNED);
  mxk_model_t m;
  make_model(&m, 32, 24, 100, 80);
  CHECK(mxst_set_object(&a, &m));
  CHECK(mxst_set_name(&a, MXST_FN_TRACK, 1, "\xE7\x90\x83", 3)); /* 球 */
  CHECK(mxst_set_name(&a, MXST_FN_TAG, 3, "x", 1));
  size_t n = mxst_serialize(&a, blob, sizeof(blob));
  CHECK(n > 0 && blob[4] == 3);
  /* tail: n_lrn 2, line {4, 1, len 3, rgb}, object {5, 1, len 77 + 768, ...}, CRC */
  const size_t obj_len = 77 + 32 * 24;
  const size_t o_lrn = n - 2 - (4 + obj_len) - (4 + 3) - 1;
  CHECK(blob[o_lrn] == 2);
  CHECK(blob[o_lrn + 1] == 4 && blob[o_lrn + 2] == 1 && blob[o_lrn + 3] == 3 && blob[o_lrn + 4] == 0);
  CHECK(memcmp(blob + o_lrn + 5, yellow, 3) == 0);
  const uint8_t *r = blob + o_lrn + 8;
  CHECK(r[0] == 5 && r[1] == 1 && r[2] == (uint8_t)obj_len && r[3] == (uint8_t)(obj_len >> 8));
  CHECK(r[4] == 32 && r[5] == 24 && r[6] == 64 && r[7] == 0 && r[8] == 48 && r[9] == 0);
  CHECK(r[10] == 100 && r[11] == 0 && r[12] == 80 && r[13] == 0 && r[14] == 200 && r[15] == 40 && r[16] == 30);
  CHECK(r[17] == 7 && r[18] == 0 && r[19] == (uint8_t)1007 && r[20] == (uint8_t)(1007 >> 8)); /* hist[0], hist[1] */
  CHECK(r[4 + 77] == m.tmpl[0] && r[4 + obj_len - 1] == m.tmpl[32 * 24 - 1]);
  memset(&b, 0xAB, sizeof(b));
  CHECK(mxst_deserialize(blob, n, &b));
  CHECK(memcmp(&a, &b, sizeof(a)) == 0);
  CHECK(b.line_has_color == 1 && memcmp(b.line_rgb, yellow, 3) == 0 && b.has_object == 1);
  CHECK(b.object.tw == 32 && b.object.th == 24 && b.object.tmpl[32 * 24] == 0);
  uint8_t l = 0;
  const char *s = mxst_get_name(&b, MXST_FN_TRACK, 1, &l);
  CHECK(s && l == 3 && memcmp(s, "\xE7\x90\x83", 3) == 0);
}

/* v3 blob = the v2 fixture's content (version 3) + n_lrn + raw learned records + CRC. */
static size_t build_v3(const uint8_t *lrn, size_t lrn_len, uint8_t n_lrn, uint8_t *out) {
  size_t o = sizeof(k_v2_fixture) - 2;
  memcpy(out, k_v2_fixture, o);
  out[4] = 3;
  out[o++] = n_lrn;
  if (lrn_len) memcpy(out + o, lrn, lrn_len);
  o += lrn_len + 2;
  fix_crc(out, o);
  return o;
}

/* Writes an object record of model m (layout version ver, body length
 * 77 + tw*th + extra) to p; returns the record size (4 + body). */
static size_t obj_record(uint8_t *p, const mxk_model_t *m, uint8_t ver, int extra) {
  size_t body = (size_t)(77 + m->tw * m->th + extra);
  p[0] = 5;
  p[1] = ver;
  p[2] = (uint8_t)body;
  p[3] = (uint8_t)(body >> 8);
  uint8_t *q = p + 4;
  q[0] = m->tw;
  q[1] = m->th;
  q[2] = (uint8_t)m->box_w;
  q[3] = (uint8_t)(m->box_w >> 8);
  q[4] = (uint8_t)m->box_h;
  q[5] = (uint8_t)(m->box_h >> 8);
  q[6] = (uint8_t)m->cx;
  q[7] = (uint8_t)((uint16_t)m->cx >> 8);
  q[8] = (uint8_t)m->cy;
  q[9] = (uint8_t)((uint16_t)m->cy >> 8);
  memcpy(q + 10, m->rgb, 3);
  for (int i = 0; i < MXK_HIST_BINS; i++) {
    q[13 + 2 * i] = (uint8_t)m->hist[i];
    q[14 + 2 * i] = (uint8_t)(m->hist[i] >> 8);
  }
  memcpy(q + 77, m->tmpl, (size_t)m->tw * m->th);
  if (extra > 0) memset(q + 77 + (size_t)m->tw * m->th, 0, (size_t)extra);
  return 4 + body;
}

static void test_v3_learned_records(void) {
  static uint8_t lrn[2 * (4 + 77 + 1024) + 64];
  static uint8_t t[MXST_BLOB_MAX + 2048];
  mxs_data_t d;
  mxk_model_t m;
  make_model(&m, 20, 16, 50, 60);

  /* a line colour record only */
  const uint8_t line[7] = {4, 1, 3, 0, 10, 20, 200};
  size_t n = build_v3(line, sizeof(line), 1, t);
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(t, n, &d));
  CHECK(d.line_has_color == 1 && d.line_rgb[0] == 10 && d.line_rgb[1] == 20 && d.line_rgb[2] == 200);
  CHECK(d.has_object == 0 && d.n_names == 2 && gesture_is(&d.gesture_cfg, 30, 70, 2));
  /* the line colour does not change the mode (that is the func config's) */
  CHECK(d.line_cfg.mode == 0);

  /* an object record only */
  n = build_v3(lrn, obj_record(lrn, &m, 1, 0), 1, t);
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(t, n, &d));
  CHECK(d.has_object == 1 && d.line_has_color == 0);
  CHECK(d.object.tw == 20 && d.object.th == 16 && d.object.box_w == 40 && d.object.box_h == 32 &&
        d.object.cx == 50 && d.object.cy == 60 && d.object.rgb[0] == 200);
  CHECK(memcmp(d.object.hist, m.hist, sizeof(m.hist)) == 0 && memcmp(d.object.tmpl, m.tmpl, sizeof(m.tmpl)) == 0);

  /* unknown functions and unknown layout versions are skipped */
  size_t k = 0;
  const uint8_t unk[9] = {30, 1, 5, 0, 1, 2, 3, 4, 5};
  memcpy(lrn, unk, sizeof(unk));
  k += sizeof(unk);
  const uint8_t line_v2[8] = {4, 2, 4, 0, 1, 2, 3, 4};
  memcpy(lrn + k, line_v2, sizeof(line_v2));
  k += sizeof(line_v2);
  k += obj_record(lrn + k, &m, 2, 5); /* a future object layout */
  memcpy(lrn + k, unk, sizeof(unk));
  k += sizeof(unk);
  n = build_v3(lrn, k, 4, t);
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(t, n, &d));
  CHECK(d.line_has_color == 0 && d.has_object == 0 && d.n_colors == 2 && d.n_names == 2);

  /* an empty learned list is fine */
  n = build_v3(NULL, 0, 0, t);
  CHECK(mxst_deserialize(t, n, &d) && d.line_has_color == 0 && d.has_object == 0);

  /* rejected: */
  uint8_t two[14];
  memcpy(two, line, 7);
  memcpy(two + 7, line, 7);
  n = build_v3(two, sizeof(two), 2, t);
  expect_reject("line record twice", t, n);
  memcpy(two + 7, line_v2, 7); /* same function, other version: still a duplicate */
  two[9] = 3;
  n = build_v3(two, sizeof(two), 2, t);
  expect_reject("line record twice (one unreadable)", t, n);
  k = obj_record(lrn, &m, 1, 0);
  k += obj_record(lrn + k, &m, 1, 0);
  n = build_v3(lrn, k, 2, t);
  expect_reject("object record twice", t, n);
  const uint8_t line_len4[8] = {4, 1, 4, 0, 10, 20, 200, 0};
  n = build_v3(line_len4, sizeof(line_len4), 1, t);
  expect_reject("line record len 4", t, n);
  n = build_v3(lrn, obj_record(lrn, &m, 1, 1), 1, t);
  expect_reject("object record longer than tw*th", t, n);
  n = build_v3(lrn, obj_record(lrn, &m, 1, -1), 1, t);
  expect_reject("object record shorter than tw*th", t, n);
  mxk_model_t bad = m;
  bad.tw = 40;
  n = build_v3(lrn, obj_record(lrn, &bad, 1, 0), 1, t);
  expect_reject("object tw 40", t, n);
  bad = m;
  bad.box_w = 8;
  n = build_v3(lrn, obj_record(lrn, &bad, 1, 0), 1, t);
  expect_reject("object box_w 8", t, n);
  bad = m;
  bad.cx = 320;
  n = build_v3(lrn, obj_record(lrn, &bad, 1, 0), 1, t);
  expect_reject("object cx 320", t, n);
  n = build_v3(line, sizeof(line) - 1, 1, t);
  expect_reject("record overruns", t, n);
  n = build_v3(line, sizeof(line), 2, t);
  expect_reject("n_lrn too many", t, n);
  n = build_v3(line, sizeof(line), 0, t);
  expect_reject("n_lrn too few (trailing)", t, n);
  /* more than MXST_MAX_LRN records (unknown ones, empty bodies) */
  for (int i = 0; i <= MXST_MAX_LRN; i++) {
    const uint8_t u[4] = {(uint8_t)(100 + i), 1, 0, 0};
    memcpy(lrn + 4 * i, u, 4);
  }
  n = build_v3(lrn, 4 * (MXST_MAX_LRN + 1), MXST_MAX_LRN + 1, t);
  expect_reject("n_lrn > max", t, n);
  n = build_v3(lrn, 4 * MXST_MAX_LRN, MXST_MAX_LRN, t);
  CHECK(mxst_deserialize(t, n, &d));
}

static void test_v3_func_config_records(void) {
  static const uint8_t recs[3][MXP_FUNC_CONFIG_SIZE] = {
      {5, 1, 200, 99, 7, 0, 9, 9, 9, 9}, /* track, clamped like SET_FUNC_CONFIG */
      {4, 1, 7, 3, 5, 2, 0, 1, 101, 9},  /* line, clamped */
      {8, 1, 50, 70, 2, 0, 0, 0, 0, 0},
  };
  uint8_t t[MXST_BLOB_MAX + 16];
  size_t n = build_v2(recs, 3, 3, t);
  mxs_data_t d;
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(t, n, &d));
  CHECK(d.track_cfg.threshold == 95 && d.track_cfg.adapt == 50 && d.track_cfg.reacquire == 1 &&
        d.track_cfg.scale == 0);
  CHECK(d.line_cfg.mode == 0 && d.line_cfg.threshold == 3 && d.line_cfg.roi_pct == 20 && d.line_cfg.bands == 4 &&
        d.line_cfg.min_width == 1 && d.line_cfg.max_width == 1 && d.line_cfg.color_tolerance == 100);
  CHECK(gesture_is(&d.gesture_cfg, 50, 70, 2));
  /* unknown layout versions keep the defaults */
  static const uint8_t v2s[2][MXP_FUNC_CONFIG_SIZE] = {
      {4, 2, 1, 0, 60, 8, 4, 80, 30, 0},
      {5, 2, 60, 10, 1, 1, 0, 0, 0, 0},
  };
  n = build_v2(v2s, 2, 2, t);
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(t, n, &d));
  CHECK(line_cfg_is_default(&d.line_cfg) && track_cfg_is_default(&d.track_cfg));
  /* duplicates reject */
  static const uint8_t dup_line[2][MXP_FUNC_CONFIG_SIZE] = {
      {4, 1, 1, 0, 60, 8, 4, 80, 30, 0},
      {4, 2, 1, 0, 60, 8, 4, 80, 30, 0},
  };
  n = build_v2(dup_line, 2, 2, t);
  expect_reject("duplicate line record", t, n);
  static const uint8_t dup_track[2][MXP_FUNC_CONFIG_SIZE] = {
      {5, 1, 60, 10, 1, 1, 0, 0, 0, 0},
      {5, 1, 60, 10, 1, 1, 0, 0, 0, 0},
  };
  n = build_v2(dup_track, 2, 2, t);
  expect_reject("duplicate track record", t, n);
}

static void test_set_line_color_mode(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  const uint8_t teal[3] = {20, 200, 180}, grey[3] = {128, 128, 128};
  /* grey has no hue: refused, nothing changes */
  CHECK(!mxst_set_line_color(&d, grey));
  CHECK(d.line_has_color == 0 && d.line_cfg.mode == 0);
  CHECK(mxst_set_line_color(&d, teal));
  CHECK(d.line_has_color == 1 && memcmp(d.line_rgb, teal, 3) == 0 && d.line_cfg.mode == MXP_LINE_MODE_LEARNED);
  /* clearing goes back to mode 0 */
  CHECK(mxst_set_line_color(&d, NULL));
  CHECK(d.line_has_color == 0 && d.line_rgb[0] == 0 && d.line_rgb[1] == 0 && d.line_rgb[2] == 0 &&
        d.line_cfg.mode == MXP_LINE_MODE_DARK);
  /* mode 1 is kept when the colour is cleared */
  CHECK(mxst_set_line_color(&d, teal));
  d.line_cfg.mode = MXP_LINE_MODE_LIGHT;
  CHECK(mxst_set_line_color(&d, NULL));
  CHECK(d.line_has_color == 0 && d.line_cfg.mode == MXP_LINE_MODE_LIGHT);
  /* mode 2 without a colour (set by SET_FUNC_CONFIG) also goes back to 0 */
  d.line_cfg.mode = MXP_LINE_MODE_LEARNED;
  CHECK(mxst_set_line_color(&d, NULL));
  CHECK(d.line_cfg.mode == MXP_LINE_MODE_DARK);
  /* a grey learn does not touch an existing colour */
  CHECK(mxst_set_line_color(&d, teal));
  CHECK(!mxst_set_line_color(&d, grey));
  CHECK(d.line_has_color == 1 && memcmp(d.line_rgb, teal, 3) == 0);
}

static void test_set_object_clears_name(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  mxk_model_t m, m2;
  make_model(&m, 24, 24, 160, 120);
  make_model(&m2, 16, 12, 40, 30);
  CHECK(mxst_set_object(&d, &m));
  CHECK(d.has_object == 1 && memcmp(&d.object, &m, sizeof(m)) == 0);
  CHECK(mxst_set_name(&d, MXST_FN_TRACK, 1, "cup", 3));
  CHECK(mxst_set_name(&d, MXST_FN_TAG, 1, "tag", 3));
  /* learning again replaces the object and drops its name */
  CHECK(mxst_set_object(&d, &m2));
  CHECK(d.has_object == 1 && d.object.tw == 16 && d.object.cx == 40);
  CHECK(mxst_get_name(&d, MXST_FN_TRACK, 1, NULL) == NULL && d.n_names == 1);
  /* template bytes beyond tw*th are zeroed (only tw*th are saved) */
  mxk_model_t junk = m2;
  junk.tmpl[16 * 12] = 99;
  CHECK(mxst_set_object(&d, &junk) && d.object.tmpl[16 * 12] == 0);
  /* an invalid model is refused, nothing changes */
  CHECK(mxst_set_name(&d, MXST_FN_TRACK, 1, "cup", 3));
  mxk_model_t bad = m;
  bad.tw = 40;
  CHECK(!mxst_set_object(&d, &bad));
  CHECK(d.has_object == 1 && d.object.tw == 16 && mxst_get_name(&d, MXST_FN_TRACK, 1, NULL) != NULL);
  /* clearing drops the object and the name */
  CHECK(mxst_set_object(&d, NULL));
  CHECK(d.has_object == 0 && d.object.tw == 0 && mxst_get_name(&d, MXST_FN_TRACK, 1, NULL) == NULL);
  CHECK(mxst_get_name(&d, MXST_FN_TAG, 1, NULL) != NULL);
}

static void test_name_keys_m6(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  CHECK(MXST_FN_LINE == MXP_FUNC_LINE && MXST_FN_TRACK == MXP_FUNC_TRACK);
  CHECK(!mxst_set_name(&d, MXST_FN_LINE, 1, "a", 1));
  CHECK(mxst_set_name(&d, MXST_FN_TRACK, 1, "b", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_TRACK, 0, "c", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_TRACK, 2, "c", 1));
  /* face detection has no names (M3 review item) */
  CHECK(!mxst_set_name(&d, MXST_FN_FACE, 0, "d", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_FACE, 5, "d", 1));
  CHECK(d.n_names == 1);
  /* a face / line name, or an object name other than id 1, in a blob is corrupt */
  size_t n = mxst_serialize(&d, blob, sizeof(blob));
  CHECK(n > 0);
  const size_t o_nm = 4 + 1 + MXP_CONFIG_SIZE + 1 + 1;
  CHECK(blob[o_nm] == 5 && blob[o_nm + 1] == 1);
  blob[o_nm] = 3;
  fix_crc(blob, n);
  expect_reject("face name", blob, n);
  blob[o_nm] = 4;
  fix_crc(blob, n);
  expect_reject("line name", blob, n);
  blob[o_nm] = 5;
  blob[o_nm + 1] = 2;
  fix_crc(blob, n);
  expect_reject("object name id 2", blob, n);
}

static void test_serialize_validates_m6(void) {
  mxs_data_t base, d;
  mxst_defaults(&base);
  const uint8_t teal[3] = {20, 200, 180};
  CHECK(mxst_set_line_color(&base, teal));
  mxk_model_t m;
  make_model(&m, 20, 20, 100, 100);
  CHECK(mxst_set_object(&base, &m));
  CHECK(mxst_serialize(&base, blob, sizeof(blob)) > 0);

  d = base; d.line_cfg.mode = 3;              expect_no_serialize("line mode 3", &d);
  d = base; d.line_cfg.roi_pct = 19;          expect_no_serialize("line roi 19", &d);
  d = base; d.line_cfg.roi_pct = 101;         expect_no_serialize("line roi 101", &d);
  d = base; d.line_cfg.bands = 3;             expect_no_serialize("line bands 3", &d);
  d = base; d.line_cfg.bands = 13;            expect_no_serialize("line bands 13", &d);
  d = base; d.line_cfg.min_width = 0;         expect_no_serialize("line min_width 0", &d);
  d = base; d.line_cfg.min_width = 101;       expect_no_serialize("line min_width 101", &d);
  d = base; d.line_cfg.max_width = 3;         expect_no_serialize("line max < min", &d);
  d = base; d.line_cfg.color_tolerance = 101; expect_no_serialize("line tolerance 101", &d);
  d = base; d.track_cfg.threshold = 9;        expect_no_serialize("track threshold 9", &d);
  d = base; d.track_cfg.threshold = 96;       expect_no_serialize("track threshold 96", &d);
  d = base; d.track_cfg.adapt = 51;           expect_no_serialize("track adapt 51", &d);
  d = base; d.track_cfg.reacquire = 2;        expect_no_serialize("track reacquire 2", &d);
  d = base; d.track_cfg.scale = 2;            expect_no_serialize("track scale 2", &d);
  d = base; d.line_has_color = 2;             expect_no_serialize("line_has_color 2", &d);
  d = base; d.has_object = 2;                 expect_no_serialize("has_object 2", &d);
  d = base; d.object.tw = 7;                  expect_no_serialize("object tw 7", &d);
  d = base; d.object.th = 33;                 expect_no_serialize("object th 33", &d);
  d = base; d.object.box_h = 241;             expect_no_serialize("object box_h 241", &d);
  d = base; d.object.cy = -1;                 expect_no_serialize("object cy -1", &d);
  /* an invalid object is fine while has_object is 0 (it is not saved) */
  d = base; d.has_object = 0; d.object.tw = 99;
  CHECK(mxst_serialize(&d, blob, sizeof(blob)) > 0);
  /* boundary values are valid */
  d = base;
  d.line_cfg.roi_pct = 20; d.line_cfg.bands = 12; d.line_cfg.min_width = 100; d.line_cfg.max_width = 100;
  d.track_cfg.threshold = 95; d.track_cfg.adapt = 50; d.track_cfg.reacquire = 0; d.track_cfg.scale = 0;
  CHECK(mxst_serialize(&d, blob, sizeof(blob)) > 0);
}

static void test_name_keys_gesture(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  CHECK(MXST_FN_GESTURE == MXP_FUNC_GESTURE && MXST_GESTURE_ID_MAX == MXP_GESTURE_MAX);
  CHECK(mxst_set_name(&d, MXST_FN_GESTURE, 1, "a", 1));
  CHECK(mxst_set_name(&d, MXST_FN_GESTURE, 9, "b", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_GESTURE, 0, "c", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_GESTURE, 10, "d", 1));
  CHECK(d.n_names == 2);
  /* other functions still have no names */
  CHECK(!mxst_set_name(&d, 9, 1, "e", 1));
  CHECK(!mxst_set_name(&d, 4, 1, "f", 1));
  /* a gesture name with id 10 in a blob is corrupt */
  size_t n = mxst_serialize(&d, blob, sizeof(blob));
  CHECK(n > 0);
  /* names start right after the n_names byte; the 2nd name (8, 9, "b") has its id at +6 */
  const size_t o_nm = 4 + 1 + MXP_CONFIG_SIZE + 1 + 1;
  CHECK(blob[o_nm + 5] == 8 && blob[o_nm + 6] == 9);
  blob[o_nm + 6] = 10;
  fix_crc(blob, n);
  expect_reject("gesture name id 10", blob, n);
}

/* ---- M7: shape / motion func configs, QR table (learned record fn 6) ---- */

/* A v3 blob written by the M6 / M8a mxst_serialize (commit c727fb3) before M7:
 * colour 1 (230,20,20), object 16x16 (box 32x32 at 120,90, rgb 200,40,30)
 * named "小車" (5, 1), line colour (200,30,30) with line mode 2, gesture
 * config 50 / 70 / 2. Never change it: M7+ firmware must keep reading it. */
static const uint8_t k_v3_m6_fixture[417] = {
    0x4d, 0x58, 0x53, 0x54, 0x03, 0x01, 0x1e, 0x64, 0x00, 0x04, 0x01, 0x01,
    0x3c, 0x04, 0x00, 0xc2, 0x01, 0x00, 0x5a, 0x32, 0x00, 0x00, 0x00, 0x01,
    0x01, 0xe6, 0x14, 0x14, 0x01, 0x05, 0x01, 0x00, 0x06, 0xe5, 0xb0, 0x8f,
    0xe8, 0xbb, 0x8a, 0x03, 0x04, 0x01, 0x02, 0x00, 0x3c, 0x08, 0x04, 0x50,
    0x1e, 0x00, 0x05, 0x01, 0x32, 0x0a, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x01, 0x32, 0x46, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x04,
    0x01, 0x03, 0x00, 0xc8, 0x1e, 0x1e, 0x05, 0x01, 0x4d, 0x01, 0x10, 0x10,
    0x20, 0x00, 0x20, 0x00, 0x78, 0x00, 0x5a, 0x00, 0xc8, 0x28, 0x1e, 0x07,
    0x00, 0xef, 0x03, 0xd7, 0x07, 0xbf, 0x0b, 0xa7, 0x0f, 0x8f, 0x13, 0x77,
    0x17, 0x5f, 0x1b, 0x47, 0x1f, 0x2f, 0x23, 0x17, 0x27, 0xff, 0x2a, 0xe7,
    0x2e, 0xcf, 0x32, 0xb7, 0x36, 0x9f, 0x3a, 0x87, 0x3e, 0x6f, 0x42, 0x57,
    0x46, 0x3f, 0x4a, 0x27, 0x4e, 0x0f, 0x52, 0xf7, 0x55, 0xdf, 0x59, 0xc7,
    0x5d, 0xaf, 0x61, 0x97, 0x65, 0x7f, 0x69, 0x67, 0x6d, 0x4f, 0x71, 0x37,
    0x75, 0x1f, 0x79, 0x0b, 0x30, 0x55, 0x7a, 0x9f, 0xc4, 0xe9, 0x0e, 0x33,
    0x58, 0x7d, 0xa2, 0xc7, 0xec, 0x11, 0x36, 0x5b, 0x80, 0xa5, 0xca, 0xef,
    0x14, 0x39, 0x5e, 0x83, 0xa8, 0xcd, 0xf2, 0x17, 0x3c, 0x61, 0x86, 0xab,
    0xd0, 0xf5, 0x1a, 0x3f, 0x64, 0x89, 0xae, 0xd3, 0xf8, 0x1d, 0x42, 0x67,
    0x8c, 0xb1, 0xd6, 0xfb, 0x20, 0x45, 0x6a, 0x8f, 0xb4, 0xd9, 0xfe, 0x23,
    0x48, 0x6d, 0x92, 0xb7, 0xdc, 0x01, 0x26, 0x4b, 0x70, 0x95, 0xba, 0xdf,
    0x04, 0x29, 0x4e, 0x73, 0x98, 0xbd, 0xe2, 0x07, 0x2c, 0x51, 0x76, 0x9b,
    0xc0, 0xe5, 0x0a, 0x2f, 0x54, 0x79, 0x9e, 0xc3, 0xe8, 0x0d, 0x32, 0x57,
    0x7c, 0xa1, 0xc6, 0xeb, 0x10, 0x35, 0x5a, 0x7f, 0xa4, 0xc9, 0xee, 0x13,
    0x38, 0x5d, 0x82, 0xa7, 0xcc, 0xf1, 0x16, 0x3b, 0x60, 0x85, 0xaa, 0xcf,
    0xf4, 0x19, 0x3e, 0x63, 0x88, 0xad, 0xd2, 0xf7, 0x1c, 0x41, 0x66, 0x8b,
    0xb0, 0xd5, 0xfa, 0x1f, 0x44, 0x69, 0x8e, 0xb3, 0xd8, 0xfd, 0x22, 0x47,
    0x6c, 0x91, 0xb6, 0xdb, 0x00, 0x25, 0x4a, 0x6f, 0x94, 0xb9, 0xde, 0x03,
    0x28, 0x4d, 0x72, 0x97, 0xbc, 0xe1, 0x06, 0x2b, 0x50, 0x75, 0x9a, 0xbf,
    0xe4, 0x09, 0x2e, 0x53, 0x78, 0x9d, 0xc2, 0xe7, 0x0c, 0x31, 0x56, 0x7b,
    0xa0, 0xc5, 0xea, 0x0f, 0x34, 0x59, 0x7e, 0xa3, 0xc8, 0xed, 0x12, 0x37,
    0x5c, 0x81, 0xa6, 0xcb, 0xf0, 0x15, 0x3a, 0x5f, 0x84, 0xa9, 0xce, 0xf3,
    0x18, 0x3d, 0x62, 0x87, 0xac, 0xd1, 0xf6, 0x1b, 0x40, 0x65, 0x8a, 0xaf,
    0xd4, 0xf9, 0x1e, 0x43, 0x68, 0x8d, 0xb2, 0xd7, 0xfc, 0x21, 0x46, 0x6b,
    0x90, 0xb5, 0xda, 0xff, 0x24, 0x49, 0x6e, 0x93, 0xb8, 0xdd, 0x02, 0x27,
    0x4c, 0x71, 0x96, 0xbb, 0xe0, 0x05, 0x2a, 0x4f, 0x74, 0x99, 0xbe, 0xe3,
    0x08, 0x2d, 0x52, 0x77, 0x9c, 0xc1, 0xe6, 0x15, 0xde,
};

static bool shape_cfg_is_default(const mxp_shape_config_t *c) {
  mxp_shape_config_t def;
  mxp_shape_config_defaults(&def);
  return c->mode == def.mode && c->threshold == def.threshold && c->min_size == def.min_size &&
         c->max_results == def.max_results;
}

static bool motion_cfg_is_default(const mxp_motion_config_t *c) {
  mxp_motion_config_t def;
  mxp_motion_config_defaults(&def);
  return c->threshold == def.threshold && c->min_size == def.min_size && c->max_results == def.max_results;
}

static void test_defaults_m7(void) {
  mxs_data_t d;
  memset(&d, 0xAB, sizeof(d));
  mxst_defaults(&d);
  CHECK(shape_cfg_is_default(&d.shape_cfg) && motion_cfg_is_default(&d.motion_cfg) && d.n_qr == 0);
  CHECK(d.shape_cfg.min_size == 32 && d.motion_cfg.threshold == 25);
  CHECK(MXST_FN_QR == MXP_FUNC_QR && MXST_FN_SHAPE == MXP_FUNC_SHAPE && MXST_FN_MOTION == MXP_FUNC_MOTION);
}

static void test_deserialize_m6_v3_fixture(void) {
  static mxs_data_t d, e;
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(k_v3_m6_fixture, sizeof(k_v3_m6_fixture), &d));
  CHECK(d.cfg.tolerance == 30 && d.cfg.dirty == 0);
  CHECK(d.n_colors == 1 && d.colors[0].id == 1 && d.colors[0].rgb[0] == 230 && d.colors[0].rgb[1] == 20);
  uint8_t l = 0;
  const char *s = mxst_get_name(&d, MXST_FN_TRACK, 1, &l);
  CHECK(d.n_names == 1 && s && l == 6 && memcmp(s, "\xE5\xB0\x8F\xE8\xBB\x8A", 6) == 0);
  CHECK(d.line_has_color == 1 && d.line_rgb[0] == 200 && d.line_rgb[1] == 30 && d.line_rgb[2] == 30);
  CHECK(d.line_cfg.mode == MXP_LINE_MODE_LEARNED);
  CHECK(d.has_object == 1 && d.object.tw == 16 && d.object.th == 16 && d.object.box_w == 32 && d.object.cx == 120 &&
        d.object.cy == 90 && d.object.tmpl[0] == 11);
  CHECK(gesture_is(&d.gesture_cfg, 50, 70, 2));
  /* new in M7: defaults, no QR codes (review item 5) */
  CHECK(shape_cfg_is_default(&d.shape_cfg) && motion_cfg_is_default(&d.motion_cfg));
  CHECK(d.n_qr == 0);
  /* and it saves again, now with the M7 records */
  size_t n = mxst_serialize(&d, blob, sizeof(blob));
  CHECK(n == sizeof(k_v3_m6_fixture) + 2 * MXP_FUNC_CONFIG_SIZE);
  CHECK(mxst_deserialize(blob, n, &e) && memcmp(&d, &e, sizeof(d)) == 0);
}

static void test_roundtrip_m7(void) {
  static mxs_data_t a, b;
  mxst_defaults(&a);
  a.shape_cfg.mode = 1;
  a.shape_cfg.threshold = 99;
  a.shape_cfg.min_size = 200;
  a.shape_cfg.max_results = 2;
  a.motion_cfg.threshold = 7;
  a.motion_cfg.min_size = 100;
  a.motion_cfg.max_results = 8;
  uint8_t q[MXP_RESULT_DATA_MAX];
  CHECK(mxst_learn_qr(&a, (const uint8_t *)"LEFT", 4, 4) == 1);
  CHECK(mxst_learn_qr(&a, (const uint8_t *)"RIGHT", 5, 5) == 2);
  qr_bytes(q, sizeof(q), 0x80); /* binary, not UTF-8, 1000 bytes long */
  CHECK(mxst_learn_qr(&a, q, MXP_RESULT_DATA_MAX, 1000) == 3);
  CHECK(mxst_set_name(&a, MXST_FN_QR, 2, "\xE5\x8F\xB3", 3)); /* 右 */
  CHECK(mxst_set_name(&a, MXST_FN_SHAPE, 3, "box", 3));
  size_t n = mxst_serialize(&a, blob, sizeof(blob));
  CHECK(n > 0 && blob[4] == 3);
  /* tail: n_lrn 1, QR record {6, 1, len, n 3, {1, 4, 0, 4, "LEFT"}, {2, ...}, {3, ...}}, CRC */
  const size_t body = 1 + (4 + 4) + (4 + 5) + (4 + 128);
  const size_t o_lrn = n - 2 - (4 + body) - 1;
  CHECK(blob[o_lrn] == 1);
  const uint8_t *r = blob + o_lrn + 1;
  CHECK(r[0] == 6 && r[1] == 1 && r[2] == (uint8_t)body && r[3] == (uint8_t)(body >> 8) && r[4] == 3);
  CHECK(r[5] == 1 && r[6] == 4 && r[7] == 0 && r[8] == 4 && memcmp(r + 9, "LEFT", 4) == 0);
  CHECK(r[13] == 2 && r[14] == 5 && r[16] == 5 && memcmp(r + 17, "RIGHT", 5) == 0);
  CHECK(r[22] == 3 && r[23] == (uint8_t)1000 && r[24] == (uint8_t)(1000 >> 8) && r[25] == 128 && r[26] == 0x80);
  /* the shape and motion func configs are the 4th and 5th records */
  uint8_t rec[MXP_FUNC_CONFIG_SIZE];
  mxp_pack_motion_config(&a.motion_cfg, rec);
  CHECK(memcmp(blob + o_lrn - MXP_FUNC_CONFIG_SIZE, rec, sizeof(rec)) == 0);
  mxp_pack_shape_config(&a.shape_cfg, rec);
  CHECK(memcmp(blob + o_lrn - 2 * MXP_FUNC_CONFIG_SIZE, rec, sizeof(rec)) == 0);
  memset(&b, 0xAB, sizeof(b));
  CHECK(mxst_deserialize(blob, n, &b));
  CHECK(memcmp(&a, &b, sizeof(a)) == 0);
  CHECK(b.n_qr == 3 && b.qr[2].total_len == 1000 && b.qr[2].len == 128 && b.qr[0].bytes[4] == 0);
  CHECK(b.shape_cfg.mode == 1 && b.shape_cfg.threshold == 99 && b.motion_cfg.threshold == 7);
  uint8_t l = 0;
  const char *s = mxst_get_name(&b, MXST_FN_QR, 2, &l);
  CHECK(s && l == 3 && memcmp(s, "\xE5\x8F\xB3", 3) == 0);
  CHECK(mxst_get_name(&b, MXST_FN_SHAPE, 3, NULL) != NULL);
}

/* One QR table entry for qr_record. */
typedef struct {
  uint8_t id;
  uint16_t total;
  uint8_t len, fill;
} qr_ent_t;

/* Writes a QR learned record (fn 6, layout ver) of n entries to p; returns its size. */
static size_t qr_record(uint8_t *p, uint8_t ver, const qr_ent_t *e, uint8_t n) {
  size_t o = 4;
  p[o++] = n;
  for (int i = 0; i < n; i++) {
    p[o++] = e[i].id;
    p[o++] = (uint8_t)e[i].total;
    p[o++] = (uint8_t)(e[i].total >> 8);
    p[o++] = e[i].len;
    memset(p + o, e[i].fill, e[i].len);
    o += e[i].len;
  }
  const size_t body = o - 4;
  p[0] = 6;
  p[1] = ver;
  p[2] = (uint8_t)body;
  p[3] = (uint8_t)(body >> 8);
  return o;
}

static void test_qr_record_read(void) {
  static uint8_t lrn[1200];
  static uint8_t t[MXST_BLOB_MAX + 64];
  static mxs_data_t d;
  /* ids out of order are read sorted; bytes beyond len are 0 */
  const qr_ent_t ok[3] = {{5, 3, 3, 'c'}, {2, 129, 128, 'b'}, {7, 1, 1, 'a'}};
  size_t n = build_v3(lrn, qr_record(lrn, 1, ok, 3), 1, t);
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(t, n, &d));
  CHECK(d.n_qr == 3 && d.qr[0].id == 2 && d.qr[1].id == 5 && d.qr[2].id == 7);
  CHECK(d.qr[0].total_len == 129 && d.qr[0].len == 128 && d.qr[0].bytes[127] == 'b');
  CHECK(d.qr[1].len == 3 && d.qr[1].bytes[2] == 'c' && d.qr[1].bytes[3] == 0);
  CHECK(d.n_colors == 2 && d.n_names == 2 && gesture_is(&d.gesture_cfg, 30, 70, 2));
  /* an empty QR record is fine */
  n = build_v3(lrn, qr_record(lrn, 1, NULL, 0), 1, t);
  CHECK(mxst_deserialize(t, n, &d) && d.n_qr == 0);
  /* next to the line record */
  const uint8_t line[7] = {4, 1, 3, 0, 10, 20, 200};
  memcpy(lrn, line, sizeof(line));
  n = build_v3(lrn, sizeof(line) + qr_record(lrn + sizeof(line), 1, ok, 1), 2, t);
  CHECK(mxst_deserialize(t, n, &d) && d.n_qr == 1 && d.qr[0].id == 5 && d.line_has_color == 1);
}

static void test_reject_bad_qr_record(void) {
  static uint8_t lrn[1400];
  static uint8_t t[MXST_BLOB_MAX + 512];
  size_t k;
#define QR_REJECT(label, ...)                                         \
  do {                                                                \
    const qr_ent_t e_[] = {__VA_ARGS__};                              \
    k = qr_record(lrn, 1, e_, (uint8_t)(sizeof(e_) / sizeof(e_[0]))); \
    expect_reject(label, t, build_v3(lrn, k, 1, t));                  \
  } while (0)
  QR_REJECT("qr dup id", {1, 2, 2, 'a'}, {1, 2, 2, 'b'});
  QR_REJECT("qr id 9", {9, 2, 2, 'a'});
  QR_REJECT("qr id 0", {0, 2, 2, 'a'});
  QR_REJECT("qr len 129", {1, 129, 129, 'a'});
  QR_REJECT("qr len > total_len", {1, 3, 4, 'a'});
  QR_REJECT("qr total 300, len 5", {1, 300, 5, 'a'});
  QR_REJECT("qr total 50, len 30", {1, 50, 30, 'a'});
  QR_REJECT("qr len 0", {1, 0, 0, 'a'});
#undef QR_REJECT
  /* 9 entries */
  qr_ent_t nine[9];
  for (int i = 0; i < 9; i++) nine[i] = (qr_ent_t){(uint8_t)(i + 1), 1, 1, 'x'};
  nine[8].id = 1;
  expect_reject("qr n 9", t, build_v3(lrn, qr_record(lrn, 1, nine, 9), 1, t));
  /* body length does not match the entries */
  const qr_ent_t one[1] = {{1, 4, 4, 'a'}};
  k = qr_record(lrn, 1, one, 1);
  lrn[2] = (uint8_t)(k - 4 + 1);
  lrn[k] = 0;
  expect_reject("qr body longer than entries", t, build_v3(lrn, k + 1, 1, t));
  k = qr_record(lrn, 1, one, 1);
  lrn[2] = (uint8_t)(k - 4 - 1);
  expect_reject("qr body shorter than entries", t, build_v3(lrn, k - 1, 1, t));
  k = qr_record(lrn, 1, NULL, 0);
  lrn[2] = 0; /* not even n */
  expect_reject("qr body empty", t, build_v3(lrn, 4, 1, t));
  /* the QR record twice (also when one is a layout this firmware cannot read) */
  k = qr_record(lrn, 1, one, 1);
  k += qr_record(lrn + k, 2, one, 1);
  expect_reject("qr record twice", t, build_v3(lrn, k, 2, t));
}

static void test_qr_record_unknown_layout_skipped(void) {
  static uint8_t lrn[600];
  static uint8_t t[MXST_BLOB_MAX + 64];
  static mxs_data_t d;
  const qr_ent_t e[2] = {{1, 4, 4, 'a'}, {1, 4, 4, 'a'}}; /* layout 2 is not checked */
  size_t n = build_v3(lrn, qr_record(lrn, 2, e, 2), 1, t);
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(t, n, &d));
  CHECK(d.n_qr == 0 && d.n_colors == 2 && d.n_names == 2);
  /* the names of QR codes it could not read are dropped, the rest is kept */
  mxs_data_t a;
  mxst_defaults(&a);
  CHECK(mxst_learn_qr(&a, (const uint8_t *)"GO", 2, 2) == 1);
  CHECK(mxst_set_name(&a, MXST_FN_QR, 1, "go", 2));
  CHECK(mxst_set_name(&a, MXST_FN_TAG, 4, "tag", 3));
  n = mxst_serialize(&a, blob, sizeof(blob));
  CHECK(n > 0);
  const size_t o_qr = n - 2 - (4 + 1 + 4 + 2);
  CHECK(blob[o_qr] == 6 && blob[o_qr + 1] == 1);
  blob[o_qr + 1] = 2;
  fix_crc(blob, n);
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(blob, n, &d));
  CHECK(d.n_qr == 0 && d.n_names == 1 && mxst_get_name(&d, MXST_FN_TAG, 4, NULL) != NULL);
  CHECK(mxst_get_name(&d, MXST_FN_QR, 1, NULL) == NULL && d.names[1].fn == 0);
  /* a QR name without its code in a readable record: dropped too */
  blob[o_qr + 1] = 1;
  blob[o_qr + 5] = 2; /* the code is id 2 now, the name is (6, 1) */
  fix_crc(blob, n);
  CHECK(mxst_deserialize(blob, n, &d) && d.n_qr == 1 && d.qr[0].id == 2 && d.n_names == 1);
}

static void test_learn_qr_ids(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  CHECK(mxst_learn_qr(&d, (const uint8_t *)"LEFT", 4, 4) == 1);
  CHECK(d.n_qr == 1 && d.qr[0].id == 1 && d.qr[0].len == 4 && d.qr[0].total_len == 4);
  CHECK(memcmp(d.qr[0].bytes, "LEFT", 4) == 0 && d.qr[0].bytes[4] == 0);
  /* the same content: the same id, nothing added */
  CHECK(mxst_learn_qr(&d, (const uint8_t *)"LEFT", 4, 4) == 1 && d.n_qr == 1);
  CHECK(mxst_learn_qr(&d, (const uint8_t *)"RIGHT", 5, 5) == 2 && d.n_qr == 2);
  /* forget 1: the next new one gets the lowest free id; the table stays sorted */
  mxst_forget_qr(&d, 1);
  CHECK(d.n_qr == 1 && d.qr[0].id == 2);
  CHECK(mxst_learn_qr(&d, (const uint8_t *)"UP", 2, 2) == 1);
  CHECK(d.n_qr == 2 && d.qr[0].id == 1 && d.qr[1].id == 2 && memcmp(d.qr[0].bytes, "UP", 2) == 0);
  /* fill up to 8, then FULL; a learned one still answers its id */
  char c[2] = {'a', 0};
  for (int i = 3; i <= MXST_MAX_QR; i++) {
    c[0] = (char)('a' + i);
    CHECK(mxst_learn_qr(&d, (const uint8_t *)c, 1, 1) == i);
  }
  CHECK(d.n_qr == MXST_MAX_QR);
  CHECK(mxst_learn_qr(&d, (const uint8_t *)"NEW", 3, 3) == MXST_LEARN_FULL && d.n_qr == MXST_MAX_QR);
  CHECK(mxst_learn_qr(&d, (const uint8_t *)"RIGHT", 5, 5) == 2);
  /* nothing to learn: empty content, NULL, or lengths that cannot be a code */
  mxst_defaults(&d);
  CHECK(mxst_learn_qr(&d, (const uint8_t *)"", 0, 0) == MXST_LEARN_NO_COLOR);
  CHECK(mxst_learn_qr(&d, NULL, 4, 4) == MXST_LEARN_NO_COLOR);
  uint8_t q[200];
  qr_bytes(q, sizeof(q), 1);
  CHECK(mxst_learn_qr(&d, q, 129, 129) == MXST_LEARN_NO_COLOR);  /* more than 128 stored bytes */
  CHECK(mxst_learn_qr(&d, q, 100, 300) == MXST_LEARN_NO_COLOR);  /* long content cut short of 128 */
  CHECK(mxst_learn_qr(&d, q, 5, 4) == MXST_LEARN_NO_COLOR);      /* len > total_len */
  CHECK(mxst_learn_qr(&d, q, 3, 4) == MXST_LEARN_NO_COLOR);      /* short content cut */
  CHECK(d.n_qr == 0);
}

static void test_learn_qr_long_payload(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  uint8_t q[MXP_RESULT_DATA_MAX];
  qr_bytes(q, sizeof(q), 9);
  /* the same first 128 bytes, different lengths: different codes (review item 2) */
  CHECK(mxst_learn_qr(&d, q, MXP_RESULT_DATA_MAX, 300) == 1);
  CHECK(mxst_learn_qr(&d, q, MXP_RESULT_DATA_MAX, 301) == 2);
  CHECK(mxst_learn_qr(&d, q, MXP_RESULT_DATA_MAX, 128) == 3); /* exactly 128: not cut */
  CHECK(mxst_learn_qr(&d, q, MXP_RESULT_DATA_MAX, 300) == 1);
  CHECK(mxst_qr_lookup(&d, q, MXP_RESULT_DATA_MAX, 301) == 2);
  CHECK(mxst_qr_lookup(&d, q, MXP_RESULT_DATA_MAX, 302) == 0);
  CHECK(d.n_qr == 3 && d.qr[0].total_len == 300 && d.qr[1].total_len == 301 && d.qr[2].total_len == 128);
}

static void test_qr_lookup(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  CHECK(mxst_qr_lookup(&d, (const uint8_t *)"LEFT", 4, 4) == 0);
  CHECK(mxst_learn_qr(&d, (const uint8_t *)"LEFT", 4, 4) == 1);
  CHECK(mxst_learn_qr(&d, (const uint8_t *)"LEFTIST", 7, 7) == 2);
  CHECK(mxst_qr_lookup(&d, (const uint8_t *)"LEFT", 4, 4) == 1);
  CHECK(mxst_qr_lookup(&d, (const uint8_t *)"LEFTIST", 7, 7) == 2);
  CHECK(mxst_qr_lookup(&d, (const uint8_t *)"LEF", 3, 3) == 0);
  CHECK(mxst_qr_lookup(&d, (const uint8_t *)"left", 4, 4) == 0); /* bytes compare exactly */
  CHECK(mxst_qr_lookup(&d, (const uint8_t *)"LEFT", 4, 5) == 0);
  CHECK(mxst_qr_lookup(&d, NULL, 0, 0) == 0);
  CHECK(mxst_qr_lookup(&d, (const uint8_t *)"", 0, 0) == 0);
}

static void test_forget_qr_drops_name(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  CHECK(mxst_learn_qr(&d, (const uint8_t *)"A", 1, 1) == 1);
  CHECK(mxst_learn_qr(&d, (const uint8_t *)"B", 1, 1) == 2);
  CHECK(mxst_learn_qr(&d, (const uint8_t *)"C", 1, 1) == 3);
  CHECK(mxst_set_name(&d, MXST_FN_QR, 1, "one", 3));
  CHECK(mxst_set_name(&d, MXST_FN_QR, 2, "two", 3));
  CHECK(mxst_set_name(&d, MXST_FN_TAG, 2, "tag", 3));
  CHECK(mxst_set_name(&d, MXST_FN_SHAPE, 2, "tri", 3));
  mxst_forget_qr(&d, 2);
  CHECK(d.n_qr == 2 && d.qr[0].id == 1 && d.qr[1].id == 3 && d.qr[2].id == 0 && d.qr[2].len == 0);
  CHECK(mxst_get_name(&d, MXST_FN_QR, 2, NULL) == NULL && mxst_get_name(&d, MXST_FN_QR, 1, NULL) != NULL);
  CHECK(mxst_get_name(&d, MXST_FN_TAG, 2, NULL) != NULL && mxst_get_name(&d, MXST_FN_SHAPE, 2, NULL) != NULL);
  mxst_forget_qr(&d, 7); /* absent: no-op */
  CHECK(d.n_qr == 2 && d.n_names == 3);
  mxst_forget_qr(&d, 0xFFFF);
  CHECK(d.n_qr == 0 && d.n_names == 2 && mxst_get_name(&d, MXST_FN_QR, 1, NULL) == NULL);
  CHECK(mxst_get_name(&d, MXST_FN_TAG, 2, NULL) != NULL);
}

static void test_name_keys_m7(void) {
  mxs_data_t d;
  mxst_defaults(&d);
  /* QR (6): only learned ids */
  CHECK(!mxst_set_name(&d, MXST_FN_QR, 1, "a", 1));
  CHECK(mxst_learn_qr(&d, (const uint8_t *)"X", 1, 1) == 1);
  CHECK(mxst_set_name(&d, MXST_FN_QR, 1, "a", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_QR, 2, "a", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_QR, 0, "a", 1));
  /* shapes (11): ids 1-4 */
  CHECK(mxst_set_name(&d, MXST_FN_SHAPE, 1, "o", 1));
  CHECK(mxst_set_name(&d, MXST_FN_SHAPE, MXP_SHAPE_MAX, "r", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_SHAPE, 5, "x", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_SHAPE, 0, "x", 1));
  /* motion (12): never */
  CHECK(!mxst_set_name(&d, MXST_FN_MOTION, 0, "m", 1));
  CHECK(!mxst_set_name(&d, MXST_FN_MOTION, 1, "m", 1));
  CHECK(d.n_names == 3);
  /* in a blob: a shape name with id 5 or a motion name is corrupt */
  size_t n = mxst_serialize(&d, blob, sizeof(blob));
  CHECK(n > 0);
  const size_t o_nm = 4 + 1 + MXP_CONFIG_SIZE + 1 + 1; /* names: (6,1,"a"), (11,1,"o"), (11,4,"r") */
  CHECK(blob[o_nm] == 6 && blob[o_nm + 5] == 11 && blob[o_nm + 10] == 11 && blob[o_nm + 11] == 4);
  blob[o_nm + 11] = 5;
  fix_crc(blob, n);
  expect_reject("shape name id 5", blob, n);
  blob[o_nm + 11] = 4;
  blob[o_nm + 10] = 12;
  fix_crc(blob, n);
  expect_reject("motion name", blob, n);
  blob[o_nm + 10] = 11;
  blob[o_nm + 1] = 9; /* QR name id 9: never a QR id */
  fix_crc(blob, n);
  expect_reject("qr name id 9", blob, n);
}

static void test_serialize_validates_m7(void) {
  static mxs_data_t base, d;
  mxst_defaults(&base);
  CHECK(mxst_learn_qr(&base, (const uint8_t *)"AB", 2, 2) == 1);
  CHECK(mxst_learn_qr(&base, (const uint8_t *)"CD", 2, 2) == 2);
  CHECK(mxst_serialize(&base, blob, sizeof(blob)) > 0);

  d = base; d.shape_cfg.mode = 2;          expect_no_serialize("shape mode 2", &d);
  d = base; d.shape_cfg.min_size = 15;     expect_no_serialize("shape min_size 15", &d);
  d = base; d.shape_cfg.min_size = 241;    expect_no_serialize("shape min_size 241", &d);
  d = base; d.shape_cfg.max_results = 0;   expect_no_serialize("shape max_results 0", &d);
  d = base; d.shape_cfg.max_results = 9;   expect_no_serialize("shape max_results 9", &d);
  d = base; d.motion_cfg.threshold = 4;    expect_no_serialize("motion threshold 4", &d);
  d = base; d.motion_cfg.threshold = 101;  expect_no_serialize("motion threshold 101", &d);
  d = base; d.motion_cfg.min_size = 7;     expect_no_serialize("motion min_size 7", &d);
  d = base; d.motion_cfg.max_results = 9;  expect_no_serialize("motion max_results 9", &d);
  d = base; d.n_qr = 9;                    expect_no_serialize("n_qr 9", &d);
  d = base; d.qr[1].id = 1;                expect_no_serialize("qr dup id", &d);
  d = base; d.qr[1].id = 9;                expect_no_serialize("qr id 9", &d);
  d = base; d.qr[0].id = 0;                expect_no_serialize("qr id 0", &d);
  d = base; d.qr[0].len = 129;             expect_no_serialize("qr len 129", &d);
  d = base; d.qr[0].len = 3;               expect_no_serialize("qr len > total_len", &d);
  d = base; d.qr[0].total_len = 300;       expect_no_serialize("qr total 300, len 2", &d);
  d = base; d.qr[0].len = 0; d.qr[0].total_len = 0; expect_no_serialize("qr empty", &d);
  d = base; d.names[0] = (mxs_name_t){MXST_FN_QR, 3, 1, "q"}; d.n_names = 1;
  expect_no_serialize("qr name of an unlearned id", &d);
  /* boundary values are valid; an unused QR slot is not checked */
  d = base;
  d.shape_cfg.mode = 1; d.shape_cfg.threshold = 255; d.shape_cfg.min_size = 240; d.shape_cfg.max_results = 1;
  d.motion_cfg.threshold = 5; d.motion_cfg.min_size = 240; d.motion_cfg.max_results = 1;
  d.qr[5].id = 77;
  CHECK(mxst_serialize(&d, blob, sizeof(blob)) > 0);
}

static void test_func_config_records_m7(void) {
  static const uint8_t recs[2][MXP_FUNC_CONFIG_SIZE] = {
      {11, 1, 3, 200, 5, 20, 9, 9, 9, 9}, /* shape, clamped like SET_FUNC_CONFIG */
      {12, 1, 1, 250, 0, 9, 9, 9, 9, 9},  /* motion, clamped */
  };
  uint8_t t[MXST_BLOB_MAX + 16];
  size_t n = build_v2(recs, 2, 2, t);
  mxs_data_t d;
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(t, n, &d));
  CHECK(d.shape_cfg.mode == 0 && d.shape_cfg.threshold == 200 && d.shape_cfg.min_size == 16 &&
        d.shape_cfg.max_results == 8);
  CHECK(d.motion_cfg.threshold == 5 && d.motion_cfg.min_size == 240 && d.motion_cfg.max_results == 1);
  /* unknown layout versions keep the defaults */
  static const uint8_t v2s[2][MXP_FUNC_CONFIG_SIZE] = {
      {11, 2, 1, 0, 32, 4, 0, 0, 0, 0},
      {12, 2, 30, 24, 4, 0, 0, 0, 0, 0},
  };
  n = build_v2(v2s, 2, 2, t);
  memset(&d, 0xAB, sizeof(d));
  CHECK(mxst_deserialize(t, n, &d));
  CHECK(shape_cfg_is_default(&d.shape_cfg) && motion_cfg_is_default(&d.motion_cfg));
  /* duplicates reject */
  static const uint8_t dup_shape[2][MXP_FUNC_CONFIG_SIZE] = {
      {11, 1, 1, 0, 32, 4, 0, 0, 0, 0},
      {11, 2, 1, 0, 32, 4, 0, 0, 0, 0},
  };
  n = build_v2(dup_shape, 2, 2, t);
  expect_reject("duplicate shape record", t, n);
  static const uint8_t dup_motion[2][MXP_FUNC_CONFIG_SIZE] = {
      {12, 1, 30, 24, 4, 0, 0, 0, 0, 0},
      {12, 1, 30, 24, 4, 0, 0, 0, 0, 0},
  };
  n = build_v2(dup_motion, 2, 2, t);
  expect_reject("duplicate motion record", t, n);
}

int main(void) {
  test_defaults();
  test_roundtrip();
  test_corruption();
  test_names();
  test_forget();
  test_serialize_validates();
  test_deserialize_rejects_bad_utf8();
  test_set_name_utf8();
  test_learn_color();
  test_forget_names();
  test_deserialize_v1_fixture();
  test_roundtrip_gesture();
  test_v2_skips_unknown_record();
  test_v2_gesture_bad_version_skipped();
  test_v2_rejects_duplicate_gesture();
  test_v2_rejects_too_many_records();
  test_serialize_max_fits();
  test_name_keys_gesture();
  test_v1_fixture_new_fields_default();
  test_deserialize_v2_fixture();
  test_roundtrip_v3_full();
  test_v3_learned_records();
  test_v3_func_config_records();
  test_set_line_color_mode();
  test_set_object_clears_name();
  test_name_keys_m6();
  test_serialize_validates_m6();
  test_defaults_m7();
  test_deserialize_m6_v3_fixture();
  test_roundtrip_m7();
  test_qr_record_read();
  test_reject_bad_qr_record();
  test_qr_record_unknown_layout_skipped();
  test_learn_qr_ids();
  test_learn_qr_long_payload();
  test_qr_lookup();
  test_forget_qr_drops_name();
  test_name_keys_m7();
  test_serialize_validates_m7();
  test_func_config_records_m7();
  if (fails) {
    printf("%d failure(s) of %d checks\n", fails, checks);
    return 1;
  }
  printf("store tests passed (%d checks)\n", checks);
  return 0;
}
