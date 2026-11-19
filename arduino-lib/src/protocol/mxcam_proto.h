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
  MXP_CMD_GET_RESULT_DATA = 0x21, /* M7 */
  MXP_CMD_LEARN = 0x30,
  MXP_CMD_FORGET = 0x31,
  MXP_CMD_SET_NAME = 0x32,
  MXP_CMD_GET_LEARNED = 0x33,
  MXP_CMD_GET_LEARNED_DATA = 0x35, /* M7 */
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
  MXP_FUNC_TRACK = 5,
  MXP_FUNC_QR = 6,
  MXP_FUNC_GESTURE = 8,
  MXP_FUNC_SHAPE = 11,
  MXP_FUNC_MOTION = 12
};

/* Gesture ids (function 8, ResultRecord id): 1..5 = fingers shown;
 * 0 = a hand whose gesture is not recognised (no_gesture / no_hand / below
 * gesture_threshold). */
enum {
  MXP_GESTURE_UNKNOWN = 0,
  MXP_GESTURE_ONE = 1,
  MXP_GESTURE_TWO = 2,
  MXP_GESTURE_THREE = 3,
  MXP_GESTURE_FOUR = 4,
  MXP_GESTURE_FIVE = 5,
  MXP_GESTURE_LIKE = 6,
  MXP_GESTURE_OK = 7,
  MXP_GESTURE_CALL = 8,
  MXP_GESTURE_DISLIKE = 9
};
#define MXP_GESTURE_MAX 9

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
 * (colour, face) or 18 bytes (AprilTag and QR: rotation_cdeg i16 + 4 corners i16 x8). */
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
#define MXP_GESTURE_CONFIG_VERSION 1

enum { MXP_LINE_MODE_DARK = 0, MXP_LINE_MODE_LIGHT = 1, MXP_LINE_MODE_LEARNED = 2 };

/* Line config v1. threshold 0 = automatic (Otsu). */
typedef struct {
  uint8_t mode, threshold, roi_pct, bands, min_width, max_width, color_tolerance;
} mxp_line_config_t;
/* Object tracking config v1. reacquire and scale are 0/1. */
typedef struct {
  uint8_t threshold, adapt, reacquire, scale;
} mxp_track_config_t;
/* Gesture config v1 (function 8, protocol.md 10). */
typedef struct {
  uint8_t hand_threshold, gesture_threshold, max_hands;
} mxp_gesture_config_t;

void mxp_line_config_defaults(mxp_line_config_t *c);   /* 0, 0, 60, 8, 4, 80, 30 */
void mxp_track_config_defaults(mxp_track_config_t *c); /* 50, 10, 1, 1 */
void mxp_gesture_config_defaults(mxp_gesture_config_t *c); /* 40, 60, 1 (hand_threshold provisional) */
/* pack writes function_id, layout_version, the body and zero reserved bytes; returns 10. */
size_t mxp_pack_line_config(const mxp_line_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]);
size_t mxp_pack_track_config(const mxp_track_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]);
/* Require len 10, the right function_id and layout_version 1; clamp out-of-range
 * values (protocol.md 9) and ignore reserved bytes. */
bool mxp_unpack_line_config(const uint8_t *p, uint16_t len, mxp_line_config_t *out);
bool mxp_unpack_track_config(const uint8_t *p, uint16_t len, mxp_track_config_t *out);
size_t mxp_pack_gesture_config(const mxp_gesture_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]);
/* Gesture clamps: hand_threshold 10-95, gesture_threshold 0-100, max_hands 1-2. */
bool mxp_unpack_gesture_config(const uint8_t *p, uint16_t len, mxp_gesture_config_t *out);

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

/* ---- M7 (protocol.md 11) ---- */

/* Shape ids (function 11, ResultRecord id). Hexagons and rounder count as circles;
 * other polygons are not reported. Ids above 4 are reserved. */
enum { MXP_SHAPE_CIRCLE = 1, MXP_SHAPE_TRIANGLE = 2, MXP_SHAPE_SQUARE = 3, MXP_SHAPE_RECT = 4 };
#define MXP_SHAPE_MAX 4
#define MXP_SHAPE_CONFIG_VERSION 1
#define MXP_MOTION_CONFIG_VERSION 1

/* Shape config v1: mode 0 = dark shapes on light, 1 = light on dark; threshold 0 = automatic (Otsu). */
typedef struct {
  uint8_t mode, threshold, min_size, max_results;
} mxp_shape_config_t;
/* Motion config v1: threshold is the grey-level difference. */
typedef struct {
  uint8_t threshold, min_size, max_results;
} mxp_motion_config_t;
void mxp_shape_config_defaults(mxp_shape_config_t *c);   /* 0, 0, 32, 4 */
void mxp_motion_config_defaults(mxp_motion_config_t *c); /* 25, 24, 4 */
/* pack writes function_id, layout_version, the body and zero reserved bytes; returns 10. */
size_t mxp_pack_shape_config(const mxp_shape_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]);
size_t mxp_pack_motion_config(const mxp_motion_config_t *c, uint8_t out[MXP_FUNC_CONFIG_SIZE]);
/* Require len 10, the right function_id and layout_version 1; reserved bytes are ignored.
 * Shape clamps: mode > 1 -> 0, min_size 16-240, max_results 1-8 (threshold as is).
 * Motion clamps: threshold 5-100, min_size 8-240, max_results 1-8. */
bool mxp_unpack_shape_config(const uint8_t *p, uint16_t len, mxp_shape_config_t *out);
bool mxp_unpack_motion_config(const uint8_t *p, uint16_t len, mxp_motion_config_t *out);

/* AprilTag (2) and QR (6) ResultRecord extra (18 bytes): rotation_cdeg i16, then the
 * four corners x0, y0 .. x3, y3 as i16 (not clipped to the frame). */
#define MXP_CORNERS_EXTRA_SIZE 18
typedef struct {
  int16_t rotation_cdeg;
  int16_t x[4], y[4];
} mxp_corners_extra_t;
/* Writes r->extra and sets r->extra_len = 18; other record fields are untouched. */
void mxp_pack_corners_extra(const mxp_corners_extra_t *e, mxp_result_t *r);
/* false when r->extra_len != 18. */
bool mxp_unpack_corners_extra(const mxp_result_t *r, mxp_corners_extra_t *out);

/* GET_RESULT_DATA (0x21) / GET_LEARNED_DATA (0x35): attached data such as QR content,
 * at most 128 bytes; longer data is cut and flagged TRUNCATED, total_len is the full length. */
#define MXP_RESULT_DATA_MAX 128
#define MXP_DATA_FLAG_TRUNCATED 0x01
typedef struct {
  uint32_t frame_id;
  uint8_t index, flags;
  uint16_t total_len;
  uint8_t len;
  uint8_t data[MXP_RESULT_DATA_MAX];
} mxp_result_data_t;
typedef struct {
  uint8_t function_id;
  uint16_t id;
  uint8_t flags;
  uint16_t total_len;
  uint8_t len;
  uint8_t data[MXP_RESULT_DATA_MAX];
} mxp_learned_data_t;
/* GET_RESULT_DATA request: index u8 (len must be 1). */
bool mxp_unpack_get_result_data(const uint8_t *p, uint16_t len, uint8_t *index);
/* 0xA1 payload: frame_id u32, index u8, flags u8, total_len u16, len u8, data[len].
 * pack returns 9 + len, or 0 when len > 128 or cap is too small. */
size_t mxp_pack_result_data(const mxp_result_data_t *d, uint8_t *out, size_t cap);
/* Requires len <= 128 and payload length == 9 + len. */
bool mxp_unpack_result_data(const uint8_t *p, uint16_t len, mxp_result_data_t *out);
/* GET_LEARNED_DATA request: function_id u8, id u16 (len must be 3). */
bool mxp_unpack_get_learned_data(const uint8_t *p, uint16_t len, uint8_t *function_id, uint16_t *id);
/* 0xB5 payload: function_id u8, id u16, flags u8, total_len u16, len u8, data[len].
 * pack returns 7 + len, or 0 when len > 128 or cap is too small. */
size_t mxp_pack_learned_data(const mxp_learned_data_t *d, uint8_t *out, size_t cap);
/* Requires len <= 128 and payload length == 7 + len. */
bool mxp_unpack_learned_data(const uint8_t *p, uint16_t len, mxp_learned_data_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MXCAM_PROTO_H */
