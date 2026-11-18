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
