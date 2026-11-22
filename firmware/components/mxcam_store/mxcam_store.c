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
  mxp_gesture_config_defaults(&d->gesture_cfg);
  mxp_line_config_defaults(&d->line_cfg);
  mxp_track_config_defaults(&d->track_cfg);
  mxp_shape_config_defaults(&d->shape_cfg);
  mxp_motion_config_defaults(&d->motion_cfg);
}

static int find_qr(const mxs_data_t *d, uint16_t id) {
  for (int i = 0; i < d->n_qr; i++)
    if (d->qr[i].id == id) return i;
  return -1;
}

/* Name keys by id range only; a QR (6) name also needs the id to be learned
 * (name_key_ok). */
static bool name_key_range_ok(uint8_t fn, uint16_t id) {
  if (fn == MXST_FN_COLOR) return id >= 1 && id <= MXST_MAX_COLORS;
  if (fn == MXST_FN_TAG) return id <= MXST_TAG_ID_MAX;
  if (fn == MXST_FN_TRACK) return id == 1; /* one object */
  if (fn == MXST_FN_QR) return id >= 1 && id <= MXST_MAX_QR;
  if (fn == MXST_FN_GESTURE) return id >= 1 && id <= MXST_GESTURE_ID_MAX;
  if (fn == MXST_FN_SHAPE) return id >= 1 && id <= MXP_SHAPE_MAX;
  return false; /* face (3), line (4) and motion (12) have no names */
}

static bool name_key_ok(const mxs_data_t *d, uint8_t fn, uint16_t id) {
  if (!name_key_range_ok(fn, id)) return false;
  return fn != MXST_FN_QR || find_qr(d, id) >= 0;
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
  if (!name_key_ok(d, fn, id) || len > MXP_NAME_MAX || (len > 0 && !utf8)) return false;
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

bool mxst_set_line_color(mxs_data_t *d, const uint8_t *rgb) {
  if (!rgb) {
    d->line_has_color = 0;
    memset(d->line_rgb, 0, sizeof(d->line_rgb));
    if (d->line_cfg.mode == MXP_LINE_MODE_LEARNED) d->line_cfg.mode = MXP_LINE_MODE_DARK;
    return true;
  }
  if (!mxc_same_color(rgb, rgb, 100)) return false; /* grey: would never match */
  d->line_has_color = 1;
  memcpy(d->line_rgb, rgb, 3);
  d->line_cfg.mode = MXP_LINE_MODE_LEARNED;
  return true;
}

bool mxst_set_object(mxs_data_t *d, const mxk_model_t *m) {
  if (m && !mxk_model_valid(m)) return false;
  mxst_forget_names(d, MXST_FN_TRACK, 1);
  memset(&d->object, 0, sizeof(d->object));
  d->has_object = m ? 1 : 0;
  if (!m) return true;
  d->object.tw = m->tw;
  d->object.th = m->th;
  d->object.box_w = m->box_w;
  d->object.box_h = m->box_h;
  d->object.cx = m->cx;
  d->object.cy = m->cy;
  memcpy(d->object.rgb, m->rgb, 3);
  memcpy(d->object.hist, m->hist, sizeof(m->hist));
  memcpy(d->object.tmpl, m->tmpl, (size_t)m->tw * m->th);
  return true;
}

/* A stored QR content: the first min(total_len, 128) bytes, at least one. */
static bool qr_len_ok(uint8_t len, uint16_t total_len) {
  const uint16_t want = total_len > MXP_RESULT_DATA_MAX ? MXP_RESULT_DATA_MAX : total_len;
  return len >= 1 && len == want;
}

uint16_t mxst_qr_lookup(const mxs_data_t *d, const uint8_t *bytes, uint8_t len, uint16_t total_len) {
  if (!bytes || len == 0) return 0;
  for (int i = 0; i < d->n_qr; i++) {
    const mxst_qr_t *q = &d->qr[i];
    if (q->total_len == total_len && q->len == len && memcmp(q->bytes, bytes, len) == 0) return q->id;
  }
  return 0;
}

const mxst_qr_t *mxst_qr_get(const mxs_data_t *d, uint16_t id) {
  int i = find_qr(d, id);
  return i < 0 ? NULL : &d->qr[i];
}

int mxst_learn_qr(mxs_data_t *d, const uint8_t *bytes, uint8_t len, uint16_t total_len) {
  if (!bytes || !qr_len_ok(len, total_len)) return MXST_LEARN_NO_COLOR;
  uint16_t known = mxst_qr_lookup(d, bytes, len, total_len);
  if (known) return known;
  if (d->n_qr >= MXST_MAX_QR) return MXST_LEARN_FULL;
  uint8_t id = 1; /* lowest id not in use; the table is sorted by id */
  int pos = 0;
  while (pos < d->n_qr && d->qr[pos].id == id) {
    pos++;
    id++;
  }
  for (int j = d->n_qr; j > pos; j--) d->qr[j] = d->qr[j - 1];
  mxst_qr_t *q = &d->qr[pos];
  memset(q, 0, sizeof(*q));
  q->id = id;
  q->len = len;
  q->total_len = total_len;
  memcpy(q->bytes, bytes, len);
  d->n_qr++;
  return id;
}

static void remove_qr_at(mxs_data_t *d, int i) {
  for (int j = i; j + 1 < d->n_qr; j++) d->qr[j] = d->qr[j + 1];
  d->n_qr--;
  memset(&d->qr[d->n_qr], 0, sizeof(d->qr[0]));
}

void mxst_forget_qr(mxs_data_t *d, uint16_t id) {
  for (int i = d->n_qr - 1; i >= 0; i--)
    if (id == 0xFFFF || d->qr[i].id == id) remove_qr_at(d, i);
  mxst_forget_names(d, MXST_FN_QR, id);
}

/* A func config is valid when the clamping codec leaves it unchanged (the
 * ranges live in one place: mxcam_proto.c, protocol.md 9). */
static bool line_cfg_valid(const mxp_line_config_t *c) {
  uint8_t a[MXP_FUNC_CONFIG_SIZE], b[MXP_FUNC_CONFIG_SIZE];
  mxp_line_config_t u;
  mxp_pack_line_config(c, a);
  if (!mxp_unpack_line_config(a, sizeof(a), &u)) return false;
  mxp_pack_line_config(&u, b);
  return memcmp(a, b, sizeof(a)) == 0;
}

static bool track_cfg_valid(const mxp_track_config_t *c) {
  uint8_t a[MXP_FUNC_CONFIG_SIZE], b[MXP_FUNC_CONFIG_SIZE];
  mxp_track_config_t u;
  mxp_pack_track_config(c, a);
  if (!mxp_unpack_track_config(a, sizeof(a), &u)) return false;
  mxp_pack_track_config(&u, b);
  return memcmp(a, b, sizeof(a)) == 0;
}

static bool shape_cfg_valid(const mxp_shape_config_t *c) {
  uint8_t a[MXP_FUNC_CONFIG_SIZE], b[MXP_FUNC_CONFIG_SIZE];
  mxp_shape_config_t u;
  mxp_pack_shape_config(c, a);
  if (!mxp_unpack_shape_config(a, sizeof(a), &u)) return false;
  mxp_pack_shape_config(&u, b);
  return memcmp(a, b, sizeof(a)) == 0;
}

static bool motion_cfg_valid(const mxp_motion_config_t *c) {
  uint8_t a[MXP_FUNC_CONFIG_SIZE], b[MXP_FUNC_CONFIG_SIZE];
  mxp_motion_config_t u;
  mxp_pack_motion_config(c, a);
  if (!mxp_unpack_motion_config(a, sizeof(a), &u)) return false;
  mxp_pack_motion_config(&u, b);
  return memcmp(a, b, sizeof(a)) == 0;
}

/* Gesture config v1 ranges (protocol.md 10); deserialize gets them from the
 * clamping mxp_unpack_gesture_config. */
static bool gesture_cfg_valid(const mxp_gesture_config_t *g) {
  return g->hand_threshold >= 10 && g->hand_threshold <= 95 && g->gesture_threshold <= 100 &&
         g->max_hands >= 1 && g->max_hands <= 2;
}

/* The QR table: ids 1-8 in increasing order, valid lengths. */
static bool qr_table_valid(const mxs_data_t *d) {
  if (d->n_qr > MXST_MAX_QR) return false;
  for (int i = 0; i < d->n_qr; i++) {
    const mxst_qr_t *q = &d->qr[i];
    if (q->id < 1 || q->id > MXST_MAX_QR || !qr_len_ok(q->len, q->total_len)) return false;
    if (i > 0 && q->id <= d->qr[i - 1].id) return false;
  }
  return true;
}

/* The same content rules deserialize enforces (so a save never writes a blob
 * the next boot would throw away). */
static bool data_valid(const mxs_data_t *d) {
  if (d->n_colors > MXST_MAX_COLORS || d->n_names > MXST_MAX_NAMES) return false;
  if (!gesture_cfg_valid(&d->gesture_cfg)) return false;
  if (!line_cfg_valid(&d->line_cfg) || !track_cfg_valid(&d->track_cfg)) return false;
  if (!shape_cfg_valid(&d->shape_cfg) || !motion_cfg_valid(&d->motion_cfg)) return false;
  if (d->line_has_color > 1 || d->has_object > 1) return false;
  if (d->has_object && !mxk_model_valid(&d->object)) return false;
  if (!qr_table_valid(d)) return false;
  for (int i = 0; i < d->n_colors; i++) {
    if (!color_id_ok(d->colors[i].id)) return false;
    for (int j = 0; j < i; j++)
      if (d->colors[j].id == d->colors[i].id) return false;
  }
  for (int i = 0; i < d->n_names; i++) {
    const mxs_name_t *n = &d->names[i];
    if (!name_key_ok(d, n->fn, n->id) || n->len == 0 || n->len > MXP_NAME_MAX) return false;
    if (!mxp_utf8_valid((const uint8_t *)n->name, n->len)) return false;
    for (int j = 0; j < i; j++)
      if (d->names[j].fn == n->fn && d->names[j].id == n->id) return false;
  }
  return true;
}

static const uint8_t MAGIC[4] = {'M', 'X', 'S', 'T'};
#define MXST_VERSION_V1 1
#define MXST_VERSION_V2 2
#define MXST_VERSION 3     /* written by serialize */
#define MXST_N_FCFG 5      /* func config records serialize writes (line, track, gesture, shape, motion) */
#define MXST_LRN_VERSION 1 /* layout version of the learned records */

static void put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static size_t obj_rec_len(const mxk_model_t *m) { return MXST_OBJ_REC_FIXED + (size_t)m->tw * m->th; }

static size_t qr_rec_len(const mxs_data_t *d) {
  size_t n = 1;
  for (int i = 0; i < d->n_qr; i++) n += 4u + d->qr[i].len;
  return n;
}

size_t mxst_serialize(const mxs_data_t *d, uint8_t *out, size_t cap) {
  if (!data_valid(d)) return 0;
  size_t need = 4 + 1 + MXP_CONFIG_SIZE + 1 + (size_t)d->n_colors * 4 + 1 + 1 +
                MXST_N_FCFG * MXP_FUNC_CONFIG_SIZE + 1 + 2;
  for (int i = 0; i < d->n_names; i++) need += 4u + d->names[i].len;
  if (d->line_has_color) need += 4 + MXST_LINE_REC_LEN;
  if (d->has_object) need += 4 + obj_rec_len(&d->object);
  if (d->n_qr) need += 4 + qr_rec_len(d);
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
  out[o++] = MXST_N_FCFG;
  o += mxp_pack_line_config(&d->line_cfg, out + o);
  o += mxp_pack_track_config(&d->track_cfg, out + o);
  o += mxp_pack_gesture_config(&d->gesture_cfg, out + o);
  o += mxp_pack_shape_config(&d->shape_cfg, out + o);
  o += mxp_pack_motion_config(&d->motion_cfg, out + o);
  out[o++] = (uint8_t)(d->line_has_color + d->has_object + (d->n_qr ? 1 : 0));
  if (d->line_has_color) {
    out[o++] = MXST_FN_LINE;
    out[o++] = MXST_LRN_VERSION;
    put_u16(out + o, MXST_LINE_REC_LEN);
    o += 2;
    memcpy(out + o, d->line_rgb, 3);
    o += 3;
  }
  if (d->has_object) {
    const mxk_model_t *m = &d->object;
    out[o++] = MXST_FN_TRACK;
    out[o++] = MXST_LRN_VERSION;
    put_u16(out + o, (uint16_t)obj_rec_len(m));
    o += 2;
    out[o++] = m->tw;
    out[o++] = m->th;
    put_u16(out + o, m->box_w);
    put_u16(out + o + 2, m->box_h);
    put_u16(out + o + 4, (uint16_t)m->cx);
    put_u16(out + o + 6, (uint16_t)m->cy);
    o += 8;
    memcpy(out + o, m->rgb, 3);
    o += 3;
    for (int i = 0; i < MXK_HIST_BINS; i++, o += 2) put_u16(out + o, m->hist[i]);
    memcpy(out + o, m->tmpl, (size_t)m->tw * m->th);
    o += (size_t)m->tw * m->th;
  }
  if (d->n_qr) {
    out[o++] = MXST_FN_QR;
    out[o++] = MXST_LRN_VERSION;
    put_u16(out + o, (uint16_t)qr_rec_len(d));
    o += 2;
    out[o++] = d->n_qr;
    for (int i = 0; i < d->n_qr; i++) {
      const mxst_qr_t *q = &d->qr[i];
      out[o++] = q->id;
      put_u16(out + o, q->total_len);
      o += 2;
      out[o++] = q->len;
      memcpy(out + o, q->bytes, q->len);
      o += q->len;
    }
  }
  uint16_t crc = mxp_crc16(out, o);
  out[o++] = (uint8_t)(crc & 0xFF);
  out[o++] = (uint8_t)(crc >> 8);
  return o;
}

/* v2 func config record list at p[*o]. Unknown functions / layout versions
 * are skipped; a known function twice or too many records -> false. Add a
 * function here (and to serialize) without bumping the blob version. */
static bool read_fcfg_list(const uint8_t *p, size_t *o, size_t end, mxs_data_t *t) {
  if (*o + 1 > end) return false;
  uint8_t n = p[(*o)++];
  if (n > MXST_MAX_FCFG || *o + (size_t)n * MXP_FUNC_CONFIG_SIZE > end) return false;
  bool seen_line = false, seen_track = false, seen_gesture = false, seen_shape = false, seen_motion = false;
  for (int i = 0; i < n; i++, *o += MXP_FUNC_CONFIG_SIZE) {
    const uint8_t *r = p + *o;
    /* unpack false = another layout version: keep the default */
    switch (r[0]) {
    case MXP_FUNC_LINE:
      if (seen_line) return false;
      seen_line = true;
      mxp_unpack_line_config(r, MXP_FUNC_CONFIG_SIZE, &t->line_cfg);
      break;
    case MXP_FUNC_TRACK:
      if (seen_track) return false;
      seen_track = true;
      mxp_unpack_track_config(r, MXP_FUNC_CONFIG_SIZE, &t->track_cfg);
      break;
    case MXP_FUNC_GESTURE:
      if (seen_gesture) return false;
      seen_gesture = true;
      mxp_unpack_gesture_config(r, MXP_FUNC_CONFIG_SIZE, &t->gesture_cfg);
      break;
    case MXP_FUNC_SHAPE:
      if (seen_shape) return false;
      seen_shape = true;
      mxp_unpack_shape_config(r, MXP_FUNC_CONFIG_SIZE, &t->shape_cfg);
      break;
    case MXP_FUNC_MOTION:
      if (seen_motion) return false;
      seen_motion = true;
      mxp_unpack_motion_config(r, MXP_FUNC_CONFIG_SIZE, &t->motion_cfg);
      break;
    default:
      break; /* not known to this firmware */
    }
  }
  return true;
}

/* Object record body (layout version 1, len bytes) -> t->object. */
static bool read_object(const uint8_t *b, size_t len, mxs_data_t *t) {
  if (len < MXST_OBJ_REC_FIXED) return false;
  mxk_model_t *m = &t->object;
  m->tw = b[0];
  m->th = b[1];
  m->box_w = get_u16(b + 2);
  m->box_h = get_u16(b + 4);
  m->cx = (int16_t)get_u16(b + 6);
  m->cy = (int16_t)get_u16(b + 8);
  memcpy(m->rgb, b + 10, 3);
  for (int i = 0; i < MXK_HIST_BINS; i++) m->hist[i] = get_u16(b + 13 + 2 * i);
  if (!mxk_model_valid(m) || len != obj_rec_len(m)) return false;
  memcpy(m->tmpl, b + MXST_OBJ_REC_FIXED, (size_t)m->tw * m->th);
  t->has_object = 1;
  return true;
}

/* QR table body (layout version 1, len bytes) -> t->qr, sorted by id. */
static bool read_qr_table(const uint8_t *b, size_t len, mxs_data_t *t) {
  if (len < 1 || b[0] > MXST_MAX_QR) return false;
  const uint8_t n = b[0];
  size_t o = 1;
  for (int i = 0; i < n; i++) {
    if (o + 4 > len) return false;
    const uint8_t id = b[o], ql = b[o + 3];
    const uint16_t total = get_u16(b + o + 1);
    if (id < 1 || id > MXST_MAX_QR || !qr_len_ok(ql, total) || o + 4 + ql > len) return false;
    if (find_qr(t, id) >= 0) return false;
    int pos = t->n_qr; /* insertion keeps the table sorted by id */
    while (pos > 0 && t->qr[pos - 1].id > id) {
      t->qr[pos] = t->qr[pos - 1];
      pos--;
    }
    mxst_qr_t *q = &t->qr[pos];
    memset(q, 0, sizeof(*q));
    q->id = id;
    q->len = ql;
    q->total_len = total;
    memcpy(q->bytes, b + o + 4, ql);
    t->n_qr++;
    o += 4 + (size_t)ql;
  }
  return o == len;
}

/* v3 learned record list at p[*o]. Unknown functions / layout versions are
 * skipped; a known function twice, too many records, a wrong length or an
 * invalid model -> false. Add a function here (and to serialize) without
 * bumping the blob version. */
static bool read_lrn_list(const uint8_t *p, size_t *o, size_t end, mxs_data_t *t) {
  if (*o + 1 > end) return false;
  uint8_t n = p[(*o)++];
  if (n > MXST_MAX_LRN) return false;
  bool seen_line = false, seen_track = false, seen_qr = false;
  for (int i = 0; i < n; i++) {
    if (*o + 4 > end) return false;
    const uint8_t fn = p[*o], ver = p[*o + 1];
    const size_t len = get_u16(p + *o + 2);
    const uint8_t *b = p + *o + 4;
    if (*o + 4 + len > end) return false;
    *o += 4 + len;
    switch (fn) {
    case MXP_FUNC_LINE:
      if (seen_line) return false;
      seen_line = true;
      if (ver != MXST_LRN_VERSION) break; /* unreadable: nothing learned */
      if (len != MXST_LINE_REC_LEN) return false;
      t->line_has_color = 1;
      memcpy(t->line_rgb, b, 3);
      break;
    case MXP_FUNC_TRACK:
      if (seen_track) return false;
      seen_track = true;
      if (ver != MXST_LRN_VERSION) break;
      if (!read_object(b, len, t)) return false;
      break;
    case MXP_FUNC_QR:
      if (seen_qr) return false;
      seen_qr = true;
      if (ver != MXST_LRN_VERSION) break;
      if (!read_qr_table(b, len, t)) return false;
      break;
    default:
      break; /* not known to this firmware */
    }
  }
  return true;
}

bool mxst_deserialize(const uint8_t *p, size_t len, mxs_data_t *out) {
  if (!p || len < 4 + 1 + MXP_CONFIG_SIZE + 1 + 1 + 2) return false;
  if (memcmp(p, MAGIC, 4) != 0 || p[4] < MXST_VERSION_V1 || p[4] > MXST_VERSION) return false;
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
    /* QR names are matched with the learned codes after the learned list */
    if (!name_key_range_ok(fn, id) || nl == 0 || nl > MXP_NAME_MAX || o + nl > end) return false;
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
  if (p[4] >= MXST_VERSION_V2 && !read_fcfg_list(p, &o, end, &t)) return false;
  if (p[4] >= MXST_VERSION && !read_lrn_list(p, &o, end, &t)) return false;
  if (o != end) return false;
  /* A QR name whose code is not learned (its record was of a layout this
   * firmware cannot read, or missing) is dropped, not a reason to lose the
   * whole blob. */
  for (int i = t.n_names - 1; i >= 0; i--)
    if (t.names[i].fn == MXST_FN_QR && find_qr(&t, t.names[i].id) < 0) remove_name_at(&t, i);
  *out = t;
  return true;
}
