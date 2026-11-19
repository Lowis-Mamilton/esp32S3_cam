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
  CHECK(nmax == (size_t)MXST_BLOB_MAX);
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

#define MUT(label, stmt, recrc)    \
  memcpy(t, blob, n);              \
  stmt;                            \
  if (recrc) fix_crc(t, n);        \
  expect_reject(label, t, n)

  MUT("bad magic", t[0] = 'X', 1);
  MUT("bad version", t[4] = 2, 1);
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
  CHECK(mxst_set_name(&d, MXST_FN_FACE, 5, "bob", 3));
  CHECK(d.n_names == 2);
  /* remove */
  CHECK(mxst_set_name(&d, MXST_FN_TAG, 5, NULL, 0));
  CHECK(d.n_names == 1 && mxst_get_name(&d, MXST_FN_TAG, 5, NULL) == NULL);
  CHECK(mxst_get_name(&d, MXST_FN_FACE, 5, NULL) != NULL);
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
}

static void test_deserialize_rejects_bad_utf8(void) {
  mxs_data_t a, b;
  mxst_defaults(&a);
  CHECK(mxst_set_name(&a, MXST_FN_TAG, 5, "ab", 2));
  size_t n = mxst_serialize(&a, blob, sizeof(blob));
  CHECK(n > 0);
  /* the name bytes are just before the CRC */
  blob[n - 4] = 0xC3;
  blob[n - 3] = 0x28; /* C3 28: bad continuation */
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
  if (fails) {
    printf("%d failure(s)\n", fails);
    return 1;
  }
  printf("store tests passed\n");
  return 0;
}
