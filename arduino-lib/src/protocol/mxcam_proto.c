#include "mxcam_proto.h"

#include <string.h>

#define INFO_FIXED 23u
#define INFO_MAX_FUNCS 8u

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
  if (in->function_count > INFO_MAX_FUNCS) return 0;
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
  if (n > INFO_MAX_FUNCS || len != INFO_FIXED + n) return false;
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
  if (len != 9) return false;
  out->function_id = p[0];
  out->x = get_u16(p + 1);
  out->y = get_u16(p + 3);
  out->w = get_u16(p + 5);
  out->h = get_u16(p + 7);
  return true;
}

static bool utf8_valid(const uint8_t *s, size_t n) {
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
  if (!utf8_valid(p + 4, n)) return false;
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
