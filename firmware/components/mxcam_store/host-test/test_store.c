#include <stdio.h>
#include <string.h>

#include "mxcam_store.h"

static int fails;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
      fails++;                                            \
    }                                                     \
  } while (0)

static uint8_t blob[MXST_BLOB_MAX + 16];

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
  /* the v1 maximum (1719) + n_fcfg + the three records written (line, track,
   * gesture) + n_lrn (nothing learned) */
  CHECK(nmax == 1719 + 1 + 3 * MXP_FUNC_CONFIG_SIZE + 1 && nmax <= (size_t)MXST_BLOB_MAX);
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
  MUT("n_fcfg too many for data", t[o_fc] = 4, 1);
  MUT("n_fcfg too few", t[o_fc] = 2, 1);
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
  /* the name bytes are just before the func config list (n_fcfg + 3 records),
   * the empty learned list (n_lrn) and the CRC */
  const size_t tail = 1 + 3 * MXP_FUNC_CONFIG_SIZE + 1 + 2;
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
  CHECK(n > 2 + 1 + 3 * MXP_FUNC_CONFIG_SIZE + 1 && blob[4] == 3);
  /* tail: n_fcfg 3, the SET_FUNC_CONFIG packets of line, track and gesture,
   * then n_lrn 0 (nothing learned) and the CRC */
  uint8_t rec[MXP_FUNC_CONFIG_SIZE];
  const size_t o_fc = n - 2 - 1 - 3 * MXP_FUNC_CONFIG_SIZE - 1;
  CHECK(blob[o_fc] == 3 && blob[n - 3] == 0);
  mxp_pack_line_config(&a.line_cfg, rec);
  CHECK(memcmp(blob + o_fc + 1, rec, sizeof(rec)) == 0);
  mxp_pack_track_config(&a.track_cfg, rec);
  CHECK(memcmp(blob + o_fc + 1 + MXP_FUNC_CONFIG_SIZE, rec, sizeof(rec)) == 0);
  mxp_pack_gesture_config(&a.gesture_cfg, rec);
  CHECK(memcmp(blob + o_fc + 1 + 2 * MXP_FUNC_CONFIG_SIZE, rec, sizeof(rec)) == 0);
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

static void test_serialize_max_fits(void) {
  /* v2 maximum (1880) + n_lrn + line record (4 + 3) + object record (4 + 77 + 32 x 32) */
  CHECK(MXST_BLOB_MAX == 1880 + 1 + 4 + 3 + 4 + 77 + MXK_TMPL_MAX * MXK_TMPL_MAX);
  CHECK(MXST_BLOB_MAX == 2993);
  mxs_data_t a, b;
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
  size_t n = mxst_serialize(&a, blob, sizeof(blob));
  /* everything this firmware writes: v1 max + 3 func configs + both learned records */
  CHECK(n == 1719 + 1 + 3 * MXP_FUNC_CONFIG_SIZE + 1 + 4 + 3 + 4 + 77 + 1024);
  CHECK(n > 0 && n <= (size_t)MXST_BLOB_MAX);
  CHECK(mxst_deserialize(blob, n, &b) && memcmp(&a, &b, sizeof(a)) == 0);
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
  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("store tests passed\n");
  return 0;
}
