#include "mxcam_proto.h"

#include <string.h>

#define INFO_FIXED 23u

enum { S_M0, S_M1, S_VER, S_CMD, S_SEQ, S_LEN0, S_LEN1, S_PAYLOAD, S_CRC0, S_CRC1 };

static uint16_t crc_step(uint16_t crc, uint8_t b) {
  crc ^= (uint16_t)((uint16_t)b << 8);
  for (int i = 0; i < 8; i++) {
    crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
  }
  return crc;
}

uint16_t mxp_crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFFu;
  for (size_t i = 0; i < len; i++) crc = crc_step(crc, data[i]);
  return crc;
}

size_t mxp_encode(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t len,
                  uint8_t *out, size_t out_cap) {
  if (len > 0 && payload == NULL) return 0;
  size_t total = (size_t)len + MXP_OVERHEAD;
  if (out == NULL || out_cap < total) return 0;
  out[0] = MXP_MAGIC0;
  out[1] = MXP_MAGIC1;
  out[2] = MXP_VERSION;
  out[3] = cmd;
  out[4] = seq;
  out[5] = (uint8_t)(len & 0xFF);
  out[6] = (uint8_t)(len >> 8);
  if (len > 0) memcpy(out + MXP_HEADER_SIZE, payload, len);
  uint16_t crc = mxp_crc16(out + 2, (size_t)5 + len);
  out[MXP_HEADER_SIZE + len] = (uint8_t)(crc & 0xFF);
  out[MXP_HEADER_SIZE + len + 1] = (uint8_t)(crc >> 8);
  return total;
}

size_t mxp_encode_error(uint8_t seq, uint8_t orig_cmd, uint8_t code, uint8_t *out,
                        size_t out_cap) {
  uint8_t p[2] = {orig_cmd, code};
  return mxp_encode(MXP_CMD_ERROR, seq, p, 2, out, out_cap);
}

void mxp_decoder_init(mxp_decoder_t *d, uint8_t *buf, size_t cap) {
  memset(d, 0, sizeof(*d));
  d->buf = buf;
  d->cap = cap;
  d->state = S_M0;
}

bool mxp_decoder_feed(mxp_decoder_t *d, uint8_t b, mxp_frame_t *out) {
  switch (d->state) {
    case S_M0:
      if (b == MXP_MAGIC0) d->state = S_M1;
      break;
    case S_M1:
      if (b == MXP_MAGIC1) {
        d->state = S_VER;
      } else if (b != MXP_MAGIC0) {
        d->state = S_M0;
      }
      break;
    case S_VER:
      if (b != MXP_VERSION) {
        d->state = S_M0;
      } else {
        d->crc = crc_step(0xFFFFu, b);
        d->state = S_CMD;
      }
      break;
    case S_CMD:
      d->cmd = b;
      d->crc = crc_step(d->crc, b);
      d->state = S_SEQ;
      break;
    case S_SEQ:
      d->seq = b;
      d->crc = crc_step(d->crc, b);
      d->state = S_LEN0;
      break;
    case S_LEN0:
      d->len = b;
      d->crc = crc_step(d->crc, b);
      d->state = S_LEN1;
      break;
    case S_LEN1:
      d->len = (uint16_t)(d->len | ((uint16_t)b << 8));
      d->crc = crc_step(d->crc, b);
      if (d->len > d->cap) {
        d->state = S_M0; /* drop; resume magic search at the next byte */
      } else {
        d->pos = 0;
        d->state = (d->len == 0) ? S_CRC0 : S_PAYLOAD;
      }
      break;
    case S_PAYLOAD:
      d->buf[d->pos++] = b;
      d->crc = crc_step(d->crc, b);
      if (d->pos == d->len) d->state = S_CRC0;
      break;
    case S_CRC0:
      d->rx_crc = b;
      d->state = S_CRC1;
      break;
    case S_CRC1:
      d->state = S_M0;
      d->rx_crc = (uint16_t)(d->rx_crc | ((uint16_t)b << 8));
      if (d->rx_crc == d->crc) {
        out->cmd = d->cmd;
        out->seq = d->seq;
        out->len = d->len;
        out->payload = d->buf;
        return true;
      }
      break;
    default:
      d->state = S_M0;
      break;
  }
  return false;
}

static void put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}
static void put_u32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

size_t mxp_pack_info(const mxp_info_t *in, uint8_t *out, size_t out_cap) {
  if (in->function_count > MXP_INFO_MAX_FUNCS) return 0;
  size_t total = INFO_FIXED + in->function_count;
  if (out_cap < total) return 0;
  out[0] = in->protocol_version;
  out[1] = in->fw_major;
  out[2] = in->fw_minor;
  out[3] = in->fw_patch;
  memcpy(out + 4, in->fw_git, 7);
  memcpy(out + 11, in->serial, 6);
  put_u16(out + 17, in->frame_width);
  put_u16(out + 19, in->frame_height);
  out[21] = in->max_results;
  out[22] = in->function_count;
  memcpy(out + 23, in->function_ids, in->function_count);
  return total;
}

bool mxp_unpack_info(const uint8_t *p, uint16_t len, mxp_info_t *out) {
  if (len < INFO_FIXED) return false;
  uint8_t n = p[22];
  if (n > MXP_INFO_MAX_FUNCS || len != INFO_FIXED + n) return false;
  memset(out, 0, sizeof(*out));
  out->protocol_version = p[0];
  out->fw_major = p[1];
  out->fw_minor = p[2];
  out->fw_patch = p[3];
  memcpy(out->fw_git, p + 4, 7);
  memcpy(out->serial, p + 11, 6);
  out->frame_width = get_u16(p + 17);
  out->frame_height = get_u16(p + 19);
  out->max_results = p[21];
  out->function_count = n;
  memcpy(out->function_ids, p + 23, n);
  return true;
}

bool mxp_unpack_stream_start(const uint8_t *p, uint16_t len, mxp_stream_start_t *out) {
  if (len != 3) return false;
  out->flags = p[0];
  out->max_fps = p[1];
  out->jpeg_quality = p[2];
  return true;
}

void mxp_pack_frame_header(const mxp_frame_header_t *h, uint8_t out[MXP_FRAME_HEADER_SIZE]) {
  put_u32(out, h->frame_id);
  put_u32(out + 4, h->timestamp_ms);
  put_u16(out + 8, h->width);
  put_u16(out + 10, h->height);
}

/* ---- M2 payloads (protocol.md 7) ---- */

#define RESULTS_FIXED 6u
#define RECORD_FIXED 12u /* id, x, y, w, h (10) + confidence + extra_len */

static int16_t get_i16(const uint8_t *p) { return (int16_t)get_u16(p); }
static uint32_t get_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t mxp_pack_results(const mxp_results_t *in, uint8_t *out, size_t cap) {
  if (in->count > MXP_MAX_RESULTS) return 0;
  size_t total = RESULTS_FIXED;
  for (uint8_t i = 0; i < in->count; i++) {
    if (in->r[i].extra_len > MXP_RESULT_EXTRA_MAX) return 0;
    total += RECORD_FIXED + in->r[i].extra_len;
  }
  if (out == NULL || cap < total) return 0;
  put_u32(out, in->frame_id);
  out[4] = in->function_id;
  out[5] = in->count;
  size_t o = RESULTS_FIXED;
  for (uint8_t i = 0; i < in->count; i++) {
    const mxp_result_t *r = &in->r[i];
    put_u16(out + o, r->id);
    put_u16(out + o + 2, (uint16_t)r->x);
    put_u16(out + o + 4, (uint16_t)r->y);
    put_u16(out + o + 6, r->w);
    put_u16(out + o + 8, r->h);
    out[o + 10] = r->confidence;
    out[o + 11] = r->extra_len;
    memcpy(out + o + RECORD_FIXED, r->extra, r->extra_len);
    o += RECORD_FIXED + r->extra_len;
  }
  return o;
}

bool mxp_unpack_results(const uint8_t *p, uint16_t len, mxp_results_t *out) {
  if (len < RESULTS_FIXED) return false;
  uint8_t count = p[5];
  if (count > MXP_MAX_RESULTS) return false;
  memset(out, 0, sizeof(*out));
  out->frame_id = get_u32(p);
  out->function_id = p[4];
  out->count = count;
  size_t o = RESULTS_FIXED;
  for (uint8_t i = 0; i < count; i++) {
    if (o + RECORD_FIXED > len) return false;
    mxp_result_t *r = &out->r[i];
    r->id = get_u16(p + o);
    r->x = get_i16(p + o + 2);
    r->y = get_i16(p + o + 4);
    r->w = get_u16(p + o + 6);
    r->h = get_u16(p + o + 8);
    r->confidence = p[o + 10];
    r->extra_len = p[o + 11];
    o += RECORD_FIXED;
    if (r->extra_len > MXP_RESULT_EXTRA_MAX || o + r->extra_len > len) return false;
    memcpy(r->extra, p + o, r->extra_len);
    o += r->extra_len;
  }
  return o == len;
}

size_t mxp_pack_config(const mxp_config_t *c, uint8_t out[MXP_CONFIG_SIZE]) {
  out[0] = 1; /* config_version */
  out[1] = c->tolerance;
  put_u16(out + 2, c->min_area);
  out[4] = c->color_max;
  out[5] = c->color_names;
  out[6] = c->tag_names;
  out[7] = c->face_threshold;
  out[8] = c->face_max;
  put_u32(out + 9, c->uart_baud);
  out[13] = c->i2c_address;
  out[14] = c->led;
  out[15] = c->dirty;
  out[16] = 0;
  out[17] = 0;
  return MXP_CONFIG_SIZE;
}

static uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

bool mxp_unpack_config(const uint8_t *p, uint16_t len, mxp_config_t *out) {
  if (len != MXP_CONFIG_SIZE || p[0] != 1) return false;
  memset(out, 0, sizeof(*out));
  out->tolerance = clamp_u8(p[1], 0, 100);
  out->min_area = get_u16(p + 2);
  out->color_max = clamp_u8(p[4], 1, MXP_MAX_RESULTS);
  out->color_names = p[5] ? 1 : 0;
  out->tag_names = p[6] ? 1 : 0;
  out->face_threshold = clamp_u8(p[7], 0, 100);
  out->face_max = clamp_u8(p[8], 1, MXP_MAX_RESULTS);
  uint32_t baud = get_u32(p + 9);
  out->uart_baud = (baud == 115200u || baud == 9600u) ? baud : 115200u;
  out->i2c_address = (p[13] >= 0x08 && p[13] <= 0x77) ? p[13] : 0x5A;
  out->led = clamp_u8(p[14], 0, 100);
  out->dirty = 0; /* read-only; never taken from the host */
  return true;
}

bool mxp_unpack_learn(const uint8_t *p, uint16_t len, mxp_learn_req_t *out) {
  if (len != 9 && len != 11) return false;
  out->function_id = p[0];
  out->x = get_u16(p + 1);
  out->y = get_u16(p + 3);
  out->w = get_u16(p + 5);
  out->h = get_u16(p + 7);
  out->has_id = (len == 11) ? 1 : 0;
  out->id = (len == 11) ? get_u16(p + 9) : 0;
  return true;
}

bool mxp_utf8_valid(const uint8_t *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    uint8_t b = s[i];
    size_t extra;
    uint32_t cp, min;
    if (b < 0x80) {
      i++;
      continue;
    } else if (b >= 0xC2 && b <= 0xDF) {
      extra = 1; cp = b & 0x1Fu; min = 0x80;
    } else if (b >= 0xE0 && b <= 0xEF) {
      extra = 2; cp = b & 0x0Fu; min = 0x800;
    } else if (b >= 0xF0 && b <= 0xF4) {
      extra = 3; cp = b & 0x07u; min = 0x10000;
    } else {
      return false; /* stray continuation, C0/C1, F5..FF */
    }
    if (extra > n - i - 1) return false; /* truncated sequence */
    for (size_t k = 1; k <= extra; k++) {
      if ((s[i + k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (s[i + k] & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;
    i += extra + 1;
  }
  return true;
}

bool mxp_unpack_set_name(const uint8_t *p, uint16_t len, mxp_set_name_t *out) {
  if (len < 4) return false;
  uint8_t n = p[3];
  if (n > MXP_NAME_MAX || len != 4u + n) return false;
  if (!mxp_utf8_valid(p + 4, n)) return false;
  memset(out, 0, sizeof(*out));
  out->function_id = p[0];
  out->id = get_u16(p + 1);
  out->name_len = n;
  memcpy(out->name, p + 4, n);
  return true;
}

size_t mxp_pack_learned(const mxp_learned_t *items, uint8_t n, uint8_t *out, size_t cap) {
  if (n > 0 && items == NULL) return 0;
  size_t total = 1;
  for (uint8_t i = 0; i < n; i++) {
    if (items[i].name_len > MXP_NAME_MAX) return 0;
    total += 6u + items[i].name_len;
  }
  if (out == NULL || cap < total) return 0;
  out[0] = n;
  size_t o = 1;
  for (uint8_t i = 0; i < n; i++) {
    put_u16(out + o, items[i].id);
    memcpy(out + o + 2, items[i].rgb, 3);
    out[o + 5] = items[i].name_len;
    memcpy(out + o + 6, items[i].name, items[i].name_len);
    o += 6u + items[i].name_len;
  }
  return o;
}

/* ---- M6 payloads (protocol.md 9) ---- */

void mxp_line_config_defaults(mxp_line_config_t *c) {
  c->mode = MXP_LINE_MODE_DARK;
  c->threshold = 0;
  c->roi_pct = 60;
  c->bands = 8;
  c->min_width = 4;
  c->max_width = 80;
  c->color_tolerance = 30;
}

void mxp_track_config_defaults(mxp_track_config_t *c) {
  c->threshold = 50;
  c->adapt = 10;
  c->reacquire = 1;
  c->scale = 1;
}

size_t mxp_pack_line_config(const mxp_line_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]) {
  out[0] = MXP_FUNC_LINE;
  out[1] = MXP_LINE_CONFIG_VERSION;
  out[2] = c->mode;
  out[3] = c->threshold;
  out[4] = c->roi_pct;
  out[5] = c->bands;
  out[6] = c->min_width;
  out[7] = c->max_width;
  out[8] = c->color_tolerance;
  out[9] = 0;
  return MXP_FUNC_CONFIG_SIZE;
}

bool mxp_unpack_line_config(const uint8_t *p, uint16_t len, mxp_line_config_t *out) {
  if (len != MXP_FUNC_CONFIG_SIZE || p[0] != MXP_FUNC_LINE || p[1] != MXP_LINE_CONFIG_VERSION)
    return false;
  out->mode = p[2] > MXP_LINE_MODE_LEARNED ? MXP_LINE_MODE_DARK : p[2];
  out->threshold = p[3];
  out->roi_pct = clamp_u8(p[4], 20, 100);
  out->bands = clamp_u8(p[5], 4, 12);
  out->min_width = clamp_u8(p[6], 1, 100);
  out->max_width = p[7] < out->min_width ? out->min_width : p[7];
  out->color_tolerance = clamp_u8(p[8], 0, 100);
  return true;
}

size_t mxp_pack_track_config(const mxp_track_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]) {
  memset(out, 0, MXP_FUNC_CONFIG_SIZE);
  out[0] = MXP_FUNC_TRACK;
  out[1] = MXP_TRACK_CONFIG_VERSION;
  out[2] = c->threshold;
  out[3] = c->adapt;
  out[4] = c->reacquire;
  out[5] = c->scale;
  return MXP_FUNC_CONFIG_SIZE;
}

bool mxp_unpack_track_config(const uint8_t *p, uint16_t len, mxp_track_config_t *out) {
  if (len != MXP_FUNC_CONFIG_SIZE || p[0] != MXP_FUNC_TRACK || p[1] != MXP_TRACK_CONFIG_VERSION)
    return false;
  out->threshold = clamp_u8(p[2], 10, 95);
  out->adapt = clamp_u8(p[3], 0, 50);
  out->reacquire = p[4] ? 1 : 0;
  out->scale = p[5] ? 1 : 0;
  return true;
}

void mxp_gesture_config_defaults(mxp_gesture_config_t *c) {
  c->hand_threshold = 40; /* provisional, see docs/measurements/m8a.md */
  c->gesture_threshold = 60;
  c->max_hands = 1;
}

size_t mxp_pack_gesture_config(const mxp_gesture_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]) {
  memset(out, 0, MXP_FUNC_CONFIG_SIZE);
  out[0] = MXP_FUNC_GESTURE;
  out[1] = MXP_GESTURE_CONFIG_VERSION;
  out[2] = c->hand_threshold;
  out[3] = c->gesture_threshold;
  out[4] = c->max_hands;
  return MXP_FUNC_CONFIG_SIZE;
}

bool mxp_unpack_gesture_config(const uint8_t *p, uint16_t len, mxp_gesture_config_t *out) {
  if (len != MXP_FUNC_CONFIG_SIZE || p[0] != MXP_FUNC_GESTURE || p[1] != MXP_GESTURE_CONFIG_VERSION)
    return false;
  out->hand_threshold = clamp_u8(p[2], 10, 95);
  out->gesture_threshold = clamp_u8(p[3], 0, 100);
  out->max_hands = clamp_u8(p[4], 1, 2);
  return true;
}

void mxp_pack_line_extra(const mxp_line_extra_t *e, mxp_result_t *r) {
  uint8_t *x = r->extra;
  put_u16(x, (uint16_t)e->angle_cdeg);
  put_u16(x + 2, (uint16_t)e->offset_x);
  put_u16(x + 4, (uint16_t)e->x0);
  put_u16(x + 6, (uint16_t)e->y0);
  put_u16(x + 8, (uint16_t)e->x1);
  put_u16(x + 10, (uint16_t)e->y1);
  x[12] = e->flags;
  x[13] = 0;
  r->extra_len = MXP_LINE_EXTRA_SIZE;
}

bool mxp_unpack_line_extra(const mxp_result_t *r, mxp_line_extra_t *out) {
  if (r->extra_len != MXP_LINE_EXTRA_SIZE) return false;
  const uint8_t *x = r->extra;
  out->angle_cdeg = get_i16(x);
  out->offset_x = get_i16(x + 2);
  out->x0 = get_i16(x + 4);
  out->y0 = get_i16(x + 6);
  out->x1 = get_i16(x + 8);
  out->y1 = get_i16(x + 10);
  out->flags = x[12];
  return true;
}

/* ---- M7 payloads (protocol.md 11) ---- */

void mxp_shape_config_defaults(mxp_shape_config_t *c) {
  c->mode = 0;
  c->threshold = 0;
  c->min_size = 32;
  c->max_results = 4;
}

void mxp_motion_config_defaults(mxp_motion_config_t *c) {
  c->threshold = 25;
  c->min_size = 24;
  c->max_results = 4;
}

size_t mxp_pack_shape_config(const mxp_shape_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]) {
  memset(out, 0, MXP_FUNC_CONFIG_SIZE);
  out[0] = MXP_FUNC_SHAPE;
  out[1] = MXP_SHAPE_CONFIG_VERSION;
  out[2] = c->mode;
  out[3] = c->threshold;
  out[4] = c->min_size;
  out[5] = c->max_results;
  return MXP_FUNC_CONFIG_SIZE;
}

bool mxp_unpack_shape_config(const uint8_t *p, uint16_t len, mxp_shape_config_t *out) {
  if (len != MXP_FUNC_CONFIG_SIZE || p[0] != MXP_FUNC_SHAPE || p[1] != MXP_SHAPE_CONFIG_VERSION) return false;
  out->mode = p[2] > 1 ? 0 : p[2];
  out->threshold = p[3];
  out->min_size = clamp_u8(p[4], 16, 240);
  out->max_results = clamp_u8(p[5], 1, MXP_MAX_RESULTS);
  return true;
}

size_t mxp_pack_motion_config(const mxp_motion_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]) {
  memset(out, 0, MXP_FUNC_CONFIG_SIZE);
  out[0] = MXP_FUNC_MOTION;
  out[1] = MXP_MOTION_CONFIG_VERSION;
  out[2] = c->threshold;
  out[3] = c->min_size;
  out[4] = c->max_results;
  return MXP_FUNC_CONFIG_SIZE;
}

bool mxp_unpack_motion_config(const uint8_t *p, uint16_t len, mxp_motion_config_t *out) {
  if (len != MXP_FUNC_CONFIG_SIZE || p[0] != MXP_FUNC_MOTION || p[1] != MXP_MOTION_CONFIG_VERSION) return false;
  out->threshold = clamp_u8(p[2], 5, 100);
  out->min_size = clamp_u8(p[3], 8, 240);
  out->max_results = clamp_u8(p[4], 1, MXP_MAX_RESULTS);
  return true;
}

void mxp_pack_corners_extra(const mxp_corners_extra_t *e, mxp_result_t *r) {
  uint8_t *x = r->extra;
  put_u16(x, (uint16_t)e->rotation_cdeg);
  for (int k = 0; k < 4; k++) {
    put_u16(x + 2 + 4 * k, (uint16_t)e->x[k]);
    put_u16(x + 4 + 4 * k, (uint16_t)e->y[k]);
  }
  r->extra_len = MXP_CORNERS_EXTRA_SIZE;
}

bool mxp_unpack_corners_extra(const mxp_result_t *r, mxp_corners_extra_t *out) {
  if (r->extra_len != MXP_CORNERS_EXTRA_SIZE) return false;
  const uint8_t *x = r->extra;
  out->rotation_cdeg = get_i16(x);
  for (int k = 0; k < 4; k++) {
    out->x[k] = get_i16(x + 2 + 4 * k);
    out->y[k] = get_i16(x + 4 + 4 * k);
  }
  return true;
}

#define RESULT_DATA_FIXED 9u
#define LEARNED_DATA_FIXED 7u

bool mxp_unpack_get_result_data(const uint8_t *p, uint16_t len, uint8_t *index) {
  if (len != 1) return false;
  *index = p[0];
  return true;
}

size_t mxp_pack_result_data(const mxp_result_data_t *d, uint8_t *out, size_t cap) {
  if (d->len > MXP_RESULT_DATA_MAX) return 0;
  size_t total = RESULT_DATA_FIXED + d->len;
  if (out == NULL || cap < total) return 0;
  put_u32(out, d->frame_id);
  out[4] = d->index;
  out[5] = d->flags;
  put_u16(out + 6, d->total_len);
  out[8] = d->len;
  memcpy(out + RESULT_DATA_FIXED, d->data, d->len);
  return total;
}

bool mxp_unpack_result_data(const uint8_t *p, uint16_t len, mxp_result_data_t *out) {
  if (len < RESULT_DATA_FIXED) return false;
  uint8_t n = p[8];
  if (n > MXP_RESULT_DATA_MAX || len != RESULT_DATA_FIXED + n) return false;
  memset(out, 0, sizeof(*out));
  out->frame_id = get_u32(p);
  out->index = p[4];
  out->flags = p[5];
  out->total_len = get_u16(p + 6);
  out->len = n;
  memcpy(out->data, p + RESULT_DATA_FIXED, n);
  return true;
}

bool mxp_unpack_get_learned_data(const uint8_t *p, uint16_t len, uint8_t *function_id, uint16_t *id) {
  if (len != 3) return false;
  *function_id = p[0];
  *id = get_u16(p + 1);
  return true;
}

size_t mxp_pack_learned_data(const mxp_learned_data_t *d, uint8_t *out, size_t cap) {
  if (d->len > MXP_RESULT_DATA_MAX) return 0;
  size_t total = LEARNED_DATA_FIXED + d->len;
  if (out == NULL || cap < total) return 0;
  out[0] = d->function_id;
  put_u16(out + 1, d->id);
  out[3] = d->flags;
  put_u16(out + 4, d->total_len);
  out[6] = d->len;
  memcpy(out + LEARNED_DATA_FIXED, d->data, d->len);
  return total;
}

bool mxp_unpack_learned_data(const uint8_t *p, uint16_t len, mxp_learned_data_t *out) {
  if (len < LEARNED_DATA_FIXED) return false;
  uint8_t n = p[6];
  if (n > MXP_RESULT_DATA_MAX || len != LEARNED_DATA_FIXED + n) return false;
  memset(out, 0, sizeof(*out));
  out->function_id = p[0];
  out->id = get_u16(p + 1);
  out->flags = p[3];
  out->total_len = get_u16(p + 4);
  out->len = n;
  memcpy(out->data, p + LEARNED_DATA_FIXED, n);
  return true;
}
