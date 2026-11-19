/* MXCam Protocol v1 codec. Pure C99, no Arduino / ESP-IDF dependencies.
 * Spec: docs/protocol.md. Shared by firmware and the Arduino library. */
#ifndef MXCAM_PROTO_H
#define MXCAM_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MXP_MAGIC0 0x4D
#define MXP_MAGIC1 0x58
#define MXP_VERSION 1
#define MXP_HEADER_SIZE 7
#define MXP_OVERHEAD 9 /* header + crc */
#define MXP_MAX_REQUEST_PAYLOAD 1024
#define MXP_MAX_RESULTS 8
#define MXP_NAME_MAX 48
#define MXP_RESULT_EXTRA_MAX 18
#define MXP_CONFIG_SIZE 18
#define MXP_INFO_MAX_FUNCS 32 /* GET_INFO function list limit (ADR-0016) */
#define MXP_RESPONSE(cmd) ((uint8_t)((cmd) | 0x80))

enum {
  MXP_CMD_GET_INFO = 0x01,
  MXP_CMD_PING = 0x02,
  MXP_CMD_SET_FUNCTION = 0x10,
  MXP_CMD_GET_FUNCTION = 0x11,
  MXP_CMD_GET_RESULTS = 0x20,
  MXP_CMD_LEARN = 0x30,
  MXP_CMD_FORGET = 0x31,
  MXP_CMD_SET_NAME = 0x32,
  MXP_CMD_GET_LEARNED = 0x33,
  MXP_CMD_GET_CONFIG = 0x40,
  MXP_CMD_SET_CONFIG = 0x41,
  MXP_CMD_SAVE = 0x42,
  MXP_CMD_FACTORY_RESET = 0x43,
  MXP_CMD_GET_FUNC_CONFIG = 0x44,
  MXP_CMD_SET_FUNC_CONFIG = 0x45,
  MXP_CMD_STREAM_START = 0x50,
  MXP_CMD_STREAM_STOP = 0x51,
  MXP_CMD_PUSH_FRAME = 0x60,
  MXP_CMD_PUSH_RESULTS = 0x61,
  MXP_CMD_ERROR = 0xFF
};

enum {
  MXP_ERR_UNKNOWN_CMD = 0x01,
  MXP_ERR_BAD_LENGTH = 0x02,
  MXP_ERR_BAD_PARAM = 0x03,
  MXP_ERR_NOT_SUPPORTED = 0x04,
  MXP_ERR_BUSY = 0x05,
  MXP_ERR_INTERNAL = 0x06,
  MXP_ERR_FULL = 0x07
};

enum {
  MXP_FUNC_NONE = 0,
  MXP_FUNC_COLOR = 1,
  MXP_FUNC_APRILTAG = 2,
  MXP_FUNC_FACE = 3,
  MXP_FUNC_LINE = 4,
  MXP_FUNC_TRACK = 5
};

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). */
uint16_t mxp_crc16(const uint8_t *data, size_t len);

/* Encode a frame into out. Returns bytes written, or 0 if out_cap is too
 * small (or payload is NULL while len > 0). */
size_t mxp_encode(uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t len,
                  uint8_t *out, size_t out_cap);
size_t mxp_encode_error(uint8_t seq, uint8_t orig_cmd, uint8_t code, uint8_t *out,
                        size_t out_cap);

typedef struct {
  uint8_t cmd, seq;
  uint16_t len;
  const uint8_t *payload;
} mxp_frame_t;

typedef struct {
  uint8_t *buf; /* caller-owned payload buffer */
  size_t cap;   /* size of buf in payload bytes; frames with len > cap are dropped */
  uint8_t state;
  uint8_t cmd, seq;
  uint16_t len, pos, crc, rx_crc;
} mxp_decoder_t;

void mxp_decoder_init(mxp_decoder_t *d, uint8_t *buf, size_t cap);
/* Feed one byte. Returns true when a valid frame completed; out->payload points
 * into buf and stays valid until the next feed call. */
bool mxp_decoder_feed(mxp_decoder_t *d, uint8_t byte, mxp_frame_t *out);

/* GET_INFO response payload (23 + function_count bytes, function_count <= 32). */
typedef struct {
  uint8_t protocol_version, fw_major, fw_minor, fw_patch;
  char fw_git[7];
  uint8_t serial[6];
  uint16_t frame_width, frame_height;
  uint8_t max_results, function_count, function_ids[MXP_INFO_MAX_FUNCS];
} mxp_info_t;
size_t mxp_pack_info(const mxp_info_t *in, uint8_t *out, size_t out_cap);
bool mxp_unpack_info(const uint8_t *p, uint16_t len, mxp_info_t *out);

typedef struct {
  uint8_t flags, max_fps, jpeg_quality;
} mxp_stream_start_t;
bool mxp_unpack_stream_start(const uint8_t *p, uint16_t len, mxp_stream_start_t *out);

#define MXP_FRAME_HEADER_SIZE 12
typedef struct {
  uint32_t frame_id, timestamp_ms;
  uint16_t width, height;
} mxp_frame_header_t;
void mxp_pack_frame_header(const mxp_frame_header_t *h, uint8_t out[MXP_FRAME_HEADER_SIZE]);

/* ResultRecord (protocol.md 7). x, y are the box centre; extra is 0 bytes
 * (colour, face) or 18 bytes (AprilTag: rotation_cdeg i16 + 4 corners i16 x8). */
typedef struct {
  uint16_t id;
  int16_t x, y;
  uint16_t w, h;
  uint8_t confidence; /* 255 = not applicable */
  uint8_t extra_len;
  uint8_t extra[MXP_RESULT_EXTRA_MAX];
} mxp_result_t;
typedef struct {
  uint32_t frame_id;
  uint8_t function_id;
  uint8_t count; /* 0..MXP_MAX_RESULTS */
  mxp_result_t r[MXP_MAX_RESULTS];
} mxp_results_t;
/* GET_RESULTS response / PUSH_RESULTS payload. pack returns 0 on bad input or small cap. */
size_t mxp_pack_results(const mxp_results_t *in, uint8_t *out, size_t cap);
/* Rejects count > 8, extra_len > 18, overruns and trailing bytes. */
bool mxp_unpack_results(const uint8_t *p, uint16_t len, mxp_results_t *out);

/* Config v1 (18 bytes). dirty is read-only: written by pack, ignored (set 0) by unpack. */
typedef struct {
  uint8_t tolerance;
  uint16_t min_area;
  uint8_t color_max, color_names, tag_names, face_threshold, face_max;
  uint32_t uart_baud;
  uint8_t i2c_address, led, dirty;
} mxp_config_t;
size_t mxp_pack_config(const mxp_config_t *c, uint8_t out[MXP_CONFIG_SIZE]);
/* Requires len == 18 and config_version == 1; clamps out-of-range values (protocol.md 7). */
bool mxp_unpack_config(const uint8_t *p, uint16_t len, mxp_config_t *out);

/* LEARN request; x, y is the top-left corner. The optional trailing id (u16,
 * payload 11 bytes) asks to learn into that ID; has_id is 0 for the 9-byte form. */
typedef struct {
  uint8_t function_id;
  uint16_t x, y, w, h;
  uint8_t has_id;
  uint16_t id;
} mxp_learn_req_t;
/* Accepts 9 bytes (has_id 0, id 0) or 11 bytes (has_id 1); any other length is false. */
bool mxp_unpack_learn(const uint8_t *p, uint16_t len, mxp_learn_req_t *out);

/* Strict UTF-8 check (rejects overlong forms, surrogates, > U+10FFFF and
 * truncated sequences). s may be NULL when n is 0. */
bool mxp_utf8_valid(const uint8_t *s, size_t n);

/* SET_NAME request. name is NOT NUL-terminated; name_len 0 clears the name. */
typedef struct {
  uint8_t function_id;
  uint16_t id;
  uint8_t name_len;
  char name[MXP_NAME_MAX];
} mxp_set_name_t;
/* Rejects name_len > 48, length mismatch and invalid UTF-8 (overlong, surrogates, > U+10FFFF). */
bool mxp_unpack_set_name(const uint8_t *p, uint16_t len, mxp_set_name_t *out);

/* GET_LEARNED response items. */
typedef struct {
  uint16_t id;
  uint8_t rgb[3];
  uint8_t name_len;
  char name[MXP_NAME_MAX];
} mxp_learned_t;
size_t mxp_pack_learned(const mxp_learned_t *items, uint8_t n, uint8_t *out, size_t cap);

/* ---- M6 (protocol.md 9) ---- */

/* GET_FUNC_CONFIG response / SET_FUNC_CONFIG request and response:
 * function_id u8, layout_version u8, 8-byte body (line v1 and track v1). */
#define MXP_FUNC_CONFIG_SIZE 10
#define MXP_LINE_CONFIG_VERSION 1
#define MXP_TRACK_CONFIG_VERSION 1

enum { MXP_LINE_MODE_DARK = 0, MXP_LINE_MODE_LIGHT = 1, MXP_LINE_MODE_LEARNED = 2 };

/* Line config v1. threshold 0 = automatic (Otsu). */
typedef struct {
  uint8_t mode, threshold, roi_pct, bands, min_width, max_width, color_tolerance;
} mxp_line_config_t;
/* Object tracking config v1. reacquire and scale are 0/1. */
typedef struct {
  uint8_t threshold, adapt, reacquire, scale;
} mxp_track_config_t;

void mxp_line_config_defaults(mxp_line_config_t *c);   /* 0, 0, 60, 8, 4, 80, 30 */
void mxp_track_config_defaults(mxp_track_config_t *c); /* 50, 10, 1, 1 */
/* pack writes function_id, layout_version, the body and zero reserved bytes; returns 10. */
size_t mxp_pack_line_config(const mxp_line_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]);
size_t mxp_pack_track_config(const mxp_track_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]);
/* Require len 10, the right function_id and layout_version 1; clamp out-of-range
 * values (protocol.md 9) and ignore reserved bytes. */
bool mxp_unpack_line_config(const uint8_t *p, uint16_t len, mxp_line_config_t *out);
bool mxp_unpack_track_config(const uint8_t *p, uint16_t len, mxp_track_config_t *out);

/* Line ResultRecord extra (14 bytes): angle_cdeg, offset_x, x0, y0 (bottom end),
 * x1, y1 (top end) as i16, then flags u8 and reserved u8 = 0. */
#define MXP_LINE_EXTRA_SIZE 14
#define MXP_LINE_FLAG_BRANCH 0x01
#define MXP_LINE_FLAG_CROSS 0x02
typedef struct {
  int16_t angle_cdeg, offset_x, x0, y0, x1, y1;
  uint8_t flags;
} mxp_line_extra_t;
/* Writes r->extra and sets r->extra_len = 14; other record fields are untouched. */
void mxp_pack_line_extra(const mxp_line_extra_t *e, mxp_result_t *r);
/* false when r->extra_len != 14. */
bool mxp_unpack_line_extra(const mxp_result_t *r, mxp_line_extra_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MXCAM_PROTO_H */
