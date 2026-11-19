#include "mxcam_store.h"

#include <string.h>

void mxst_defaults(mxs_data_t *d) {
  memset(d, 0, sizeof(*d));
  d->cfg.tolerance = 30;
  d->cfg.min_area = 100;
  d->cfg.color_max = 4;
  d->cfg.color_names = 1;
  d->cfg.tag_names = 1;
  d->cfg.face_threshold = 60;
  d->cfg.face_max = 4;
  d->cfg.uart_baud = 115200;
  d->cfg.i2c_address = 0x5A;
  d->cfg.led = 50;
  d->cfg.dirty = 0;
}

static bool name_key_ok(uint8_t fn, uint16_t id) {
  if (fn == MXST_FN_COLOR) return id >= 1 && id <= MXST_MAX_COLORS;
  if (fn == MXST_FN_TAG) return id <= MXST_TAG_ID_MAX;
  if (fn == MXST_FN_FACE) return true;
  return false;
}

static bool color_id_ok(uint8_t id) { return id >= 1 && id <= MXST_MAX_COLORS; }

static int find_name(const mxs_data_t *d, uint8_t fn, uint16_t id) {
  for (int i = 0; i < d->n_names; i++)
    if (d->names[i].fn == fn && d->names[i].id == id) return i;
  return -1;
}

static void remove_name_at(mxs_data_t *d, int i) {
  for (int j = i; j + 1 < d->n_names; j++) d->names[j] = d->names[j + 1];
  d->n_names--;
  memset(&d->names[d->n_names], 0, sizeof(d->names[0]));
}

bool mxst_set_name(mxs_data_t *d, uint8_t fn, uint16_t id, const char *utf8, uint8_t len) {
  if (!name_key_ok(fn, id) || len > MXP_NAME_MAX || (len > 0 && !utf8)) return false;
  if (len > 0 && !mxp_utf8_valid((const uint8_t *)utf8, len)) return false;
  int i = find_name(d, fn, id);
  if (len == 0) {
    if (i >= 0) remove_name_at(d, i);
    return true;
  }
  if (i < 0) {
    if (d->n_names >= MXST_MAX_NAMES) return false;
    i = d->n_names++;
  }
  mxs_name_t *n = &d->names[i];
  memset(n, 0, sizeof(*n));
  n->fn = fn;
  n->id = id;
  n->len = len;
  memcpy(n->name, utf8, len);
  return true;
}

const char *mxst_get_name(const mxs_data_t *d, uint8_t fn, uint16_t id, uint8_t *len_out) {
  int i = find_name(d, fn, id);
  if (i < 0) return NULL;
  if (len_out) *len_out = d->names[i].len;
  return d->names[i].name;
}

void mxst_forget_color(mxs_data_t *d, uint8_t id) {
  if (id == 0xFF) {
    d->n_colors = 0;
    memset(d->colors, 0, sizeof(d->colors));
    for (int i = d->n_names - 1; i >= 0; i--)
      if (d->names[i].fn == MXST_FN_COLOR) remove_name_at(d, i);
    return;
  }
  for (int i = 0; i < d->n_colors; i++) {
    if (d->colors[i].id == id) {
      for (int j = i; j + 1 < d->n_colors; j++) d->colors[j] = d->colors[j + 1];
      d->n_colors--;
      memset(&d->colors[d->n_colors], 0, sizeof(d->colors[0]));
      break;
    }
  }
  int k = find_name(d, MXST_FN_COLOR, id);
  if (k >= 0) remove_name_at(d, k);
}

void mxst_forget_names(mxs_data_t *d, uint8_t fn, uint16_t id) {
  for (int i = d->n_names - 1; i >= 0; i--)
    if (d->names[i].fn == fn && (id == 0xFFFF || d->names[i].id == id)) remove_name_at(d, i);
}

int mxst_learn_color(mxs_data_t *d, const uint8_t rgb[3], uint8_t tolerance) {
  /* mxc_same_color is false whenever either side is grey. */
  if (!mxc_same_color(rgb, rgb, 100)) return MXST_LEARN_NO_COLOR;
  int match = -1;
  for (int i = 0; i < d->n_colors; i++)
    if (mxc_same_color(d->colors[i].rgb, rgb, tolerance) &&
        (match < 0 || d->colors[i].id < d->colors[match].id))
      match = i;
  if (match >= 0) {
    memcpy(d->colors[match].rgb, rgb, 3);
    return d->colors[match].id;
  }
  if (d->n_colors >= MXST_MAX_COLORS) return MXST_LEARN_FULL;
  uint8_t id = 1; /* lowest id not in use */
  for (bool used = true; used; ) {
    used = false;
    for (int i = 0; i < d->n_colors; i++)
      if (d->colors[i].id == id) { used = true; id++; break; }
  }
  int pos = 0; /* keep the list sorted by id (GET_LEARNED order) */
  while (pos < d->n_colors && d->colors[pos].id < id) pos++;
  for (int j = d->n_colors; j > pos; j--) d->colors[j] = d->colors[j - 1];
  d->colors[pos].id = id;
  memcpy(d->colors[pos].rgb, rgb, 3);
  d->n_colors++;
  return id;
}

/* The same content rules deserialize enforces (so a save never writes a blob
 * the next boot would throw away). */
static bool data_valid(const mxs_data_t *d) {
  if (d->n_colors > MXST_MAX_COLORS || d->n_names > MXST_MAX_NAMES) return false;
  for (int i = 0; i < d->n_colors; i++) {
    if (!color_id_ok(d->colors[i].id)) return false;
    for (int j = 0; j < i; j++)
      if (d->colors[j].id == d->colors[i].id) return false;
  }
  for (int i = 0; i < d->n_names; i++) {
    const mxs_name_t *n = &d->names[i];
    if (!name_key_ok(n->fn, n->id) || n->len == 0 || n->len > MXP_NAME_MAX) return false;
    if (!mxp_utf8_valid((const uint8_t *)n->name, n->len)) return false;
    for (int j = 0; j < i; j++)
      if (d->names[j].fn == n->fn && d->names[j].id == n->id) return false;
  }
  return true;
}

static const uint8_t MAGIC[4] = {'M', 'X', 'S', 'T'};
#define MXST_VERSION 1

size_t mxst_serialize(const mxs_data_t *d, uint8_t *out, size_t cap) {
  if (!data_valid(d)) return 0;
  size_t need = 4 + 1 + MXP_CONFIG_SIZE + 1 + (size_t)d->n_colors * 4 + 1 + 2;
  for (int i = 0; i < d->n_names; i++) need += 4u + d->names[i].len;
  if (need > cap) return 0;
  size_t o = 0;
  memcpy(out, MAGIC, 4);
  o = 4;
  out[o++] = MXST_VERSION;
  mxp_config_t c = d->cfg;
  c.dirty = 0;
  mxp_pack_config(&c, out + o);
  o += MXP_CONFIG_SIZE;
  out[o++] = d->n_colors;
  for (int i = 0; i < d->n_colors; i++) {
    out[o++] = d->colors[i].id;
    memcpy(out + o, d->colors[i].rgb, 3);
    o += 3;
  }
  out[o++] = d->n_names;
  for (int i = 0; i < d->n_names; i++) {
    const mxs_name_t *n = &d->names[i];
    out[o++] = n->fn;
    out[o++] = (uint8_t)(n->id & 0xFF);
    out[o++] = (uint8_t)(n->id >> 8);
    out[o++] = n->len;
    memcpy(out + o, n->name, n->len);
    o += n->len;
  }
  uint16_t crc = mxp_crc16(out, o);
  out[o++] = (uint8_t)(crc & 0xFF);
  out[o++] = (uint8_t)(crc >> 8);
  return o;
}

bool mxst_deserialize(const uint8_t *p, size_t len, mxs_data_t *out) {
  if (!p || len < 4 + 1 + MXP_CONFIG_SIZE + 1 + 1 + 2) return false;
  if (memcmp(p, MAGIC, 4) != 0 || p[4] != MXST_VERSION) return false;
  uint16_t want = (uint16_t)(p[len - 2] | (p[len - 1] << 8));
  if (mxp_crc16(p, len - 2) != want) return false;
  size_t end = len - 2, o = 5;

  mxs_data_t t;
  mxst_defaults(&t);
  if (!mxp_unpack_config(p + o, MXP_CONFIG_SIZE, &t.cfg)) return false;
  o += MXP_CONFIG_SIZE;

  uint8_t nc = p[o++];
  if (nc > MXST_MAX_COLORS || o + (size_t)nc * 4 + 1 > end) return false;
  for (int i = 0; i < nc; i++) {
    uint8_t id = p[o];
    if (!color_id_ok(id)) return false;
    for (int j = 0; j < i; j++)
      if (t.colors[j].id == id) return false;
    t.colors[i].id = id;
    memcpy(t.colors[i].rgb, p + o + 1, 3);
    o += 4;
  }
  t.n_colors = nc;

  uint8_t nn = p[o++];
  if (nn > MXST_MAX_NAMES) return false;
  for (int i = 0; i < nn; i++) {
    if (o + 4 > end) return false;
    uint8_t fn = p[o];
    uint16_t id = (uint16_t)(p[o + 1] | (p[o + 2] << 8));
    uint8_t nl = p[o + 3];
    o += 4;
    if (!name_key_ok(fn, id) || nl == 0 || nl > MXP_NAME_MAX || o + nl > end) return false;
    if (find_name(&t, fn, id) >= 0) return false;
    if (!mxp_utf8_valid(p + o, nl)) return false;
    mxs_name_t *n = &t.names[i];
    n->fn = fn;
    n->id = id;
    n->len = nl;
    memcpy(n->name, p + o, nl);
    o += nl;
    t.n_names = (uint8_t)(i + 1);
  }
  if (o != end) return false;
  *out = t;
  return true;
}
