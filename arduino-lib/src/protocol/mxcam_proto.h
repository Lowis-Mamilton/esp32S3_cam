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
#define MXP_RESPONSE(cmd) ((uint8_t)((cmd) | 0x80))

enum {
  MXP_CMD_GET_INFO = 0x01,
  MXP_CMD_PING = 0x02,
  MXP_CMD_SET_FUNCTION = 0x10,
  MXP_CMD_GET_FUNCTION = 0x11,
  MXP_CMD_STREAM_START = 0x50,
  MXP_CMD_STREAM_STOP = 0x51,
  MXP_CMD_PUSH_FRAME = 0x60,
  MXP_CMD_ERROR = 0xFF
};

enum {
  MXP_ERR_UNKNOWN_CMD = 0x01,
  MXP_ERR_BAD_LENGTH = 0x02,
  MXP_ERR_BAD_PARAM = 0x03,
  MXP_ERR_NOT_SUPPORTED = 0x04,
  MXP_ERR_BUSY = 0x05,
  MXP_ERR_INTERNAL = 0x06
};

enum {
  MXP_FUNC_NONE = 0,
  MXP_FUNC_COLOR = 1,
  MXP_FUNC_APRILTAG = 2,
  MXP_FUNC_FACE = 3
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

/* GET_INFO response payload (23 + function_count bytes). */
typedef struct {
  uint8_t protocol_version, fw_major, fw_minor, fw_patch;
  char fw_git[7];
  uint8_t serial[6];
  uint16_t frame_width, frame_height;
  uint8_t max_results, function_count, function_ids[8];
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

#ifdef __cplusplus
}
#endif

#endif /* MXCAM_PROTO_H */
