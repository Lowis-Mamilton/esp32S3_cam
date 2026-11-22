#ifndef MXCAM_STORE_H
#define MXCAM_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mxcam_color.h"
#include "mxcam_proto.h"
#include "mxcam_track.h"

#ifdef ESP_PLATFORM
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent config + learned data. The pure part (defaults / serialize /
 * deserialize / name helpers) lives in mxcam_store.c and is host-tested; the NVS
 * part (load/save/erase) lives in store_nvs.c and is ESP-only. */

#define MXST_MAX_COLORS 7
#define MXST_MAX_NAMES 32
#define MXST_FN_COLOR 1
#define MXST_FN_TAG 2
#define MXST_FN_FACE 3
#define MXST_FN_LINE 4
#define MXST_FN_TRACK 5
#define MXST_FN_QR 6
#define MXST_FN_GESTURE 8
#define MXST_FN_SHAPE 11
#define MXST_FN_MOTION 12
#define MXST_TAG_ID_MAX 586
#define MXST_GESTURE_ID_MAX 9 /* gesture names: ids 1-9 (protocol.md 10) */
#define MXST_MAX_QR 8         /* learned QR codes: ids 1-8 (protocol.md 11) */
#define MXST_MAX_FCFG 16      /* func config records a blob may hold */
#define MXST_MAX_LRN 8        /* learned records a blob may hold */
/* Learned record bodies (v3, layout version 1): line colour rgb[3]; object
 * tw, th u8, box_w, box_h u16, cx, cy i16, rgb[3], hist[32] u16, then the
 * tw*th template bytes; QR table n u8, n x { id u8, total_len u16, len u8,
 * bytes[len] }. */
#define MXST_LINE_REC_LEN 3
#define MXST_OBJ_REC_FIXED (2 + 4 + 4 + 3 + 2 * MXK_HIST_BINS) /* 77 */
#define MXST_OBJ_REC_MAX (MXST_OBJ_REC_FIXED + MXK_TMPL_MAX * MXK_TMPL_MAX)
#define MXST_QR_REC_MAX (1 + MXST_MAX_QR * (4 + MXP_RESULT_DATA_MAX)) /* 1057 */

typedef struct {
  uint8_t fn;
  uint16_t id;
  uint8_t len;                 /* explicit byte length of name (UTF-8) */
  char name[MXP_NAME_MAX + 1]; /* also NUL-terminated */
} mxs_name_t;

/* One learned QR code (LEARN 6): the content's first len bytes, len =
 * min(total_len, 128) >= 1; bytes beyond len are 0. Two codes are the same
 * when total_len and the stored bytes are equal (protocol.md 11). */
typedef struct {
  uint8_t id, len;
  uint16_t total_len;
  uint8_t bytes[MXP_RESULT_DATA_MAX];
} mxst_qr_t;

typedef struct {
  mxp_config_t cfg;
  mxc_learned_t colors[MXST_MAX_COLORS];
  uint8_t n_colors;
  mxs_name_t names[MXST_MAX_NAMES];
  uint8_t n_names;
  mxp_gesture_config_t gesture_cfg; /* function 8 (GET/SET_FUNC_CONFIG) */
  mxp_line_config_t line_cfg;       /* function 4 (GET/SET_FUNC_CONFIG) */
  mxp_track_config_t track_cfg;     /* function 5 (GET/SET_FUNC_CONFIG) */
  uint8_t line_has_color;           /* 0/1: a line colour was learned (LEARN 4) */
  uint8_t line_rgb[3];
  uint8_t has_object; /* 0/1: an object was learned (LEARN 5) */
  mxk_model_t object; /* template bytes beyond tw*th are 0 */
  mxp_shape_config_t shape_cfg;   /* function 11 (GET/SET_FUNC_CONFIG) */
  mxp_motion_config_t motion_cfg; /* function 12 (GET/SET_FUNC_CONFIG) */
  uint8_t n_qr;                   /* learned QR codes (LEARN 6) */
  mxst_qr_t qr[MXST_MAX_QR];      /* sorted by id; unused entries are 0 */
} mxs_data_t;

/* Blob layout v3 (all little-endian):
 *   "MXST"(4) | version u8 = 3 | config (18, dirty forced 0) |
 *   n_colors u8, n x { id u8, rgb[3] } |
 *   n_names u8, n x { fn u8, id u16, len u8, bytes[len] } |
 *   n_fcfg u8, n x { the SET_FUNC_CONFIG packet: function_id u8,
 *                    layout_version u8, body[8] } |
 *   n_lrn u8, n x { function_id u8, layout_version u8, len u16, body[len] } |
 *   CRC16 (mxp_crc16) over everything before it, u16.
 * v1 (version 1) has neither list, v2 (M8a) only the n_fcfg list; both are
 * still read (missing parts get their defaults / nothing learned).
 * Func config list: a record of a function this firmware does not know, or
 * of a layout version it cannot read, is skipped (that function keeps its
 * default); a known function_id twice or n_fcfg > MXST_MAX_FCFG rejects the
 * whole blob. Learned list (layout version 1): line (4) body = rgb[3]; object
 * (5) body = MXST_OBJ_REC_FIXED bytes + tw*th template bytes; QR (6) body =
 * the QR table (see above; ids 1-8 unique, len = min(total_len, 128) >= 1,
 * entries exactly fill the body; read in any order, kept sorted by id).
 * Unknown functions / layout versions are skipped (nothing learned for that
 * function); a known function twice, n_lrn > MXST_MAX_LRN, a wrong body
 * length, an invalid model (mxk_model_valid) or an invalid QR table rejects
 * the whole blob. Names of QR ids that end up not learned (e.g. a QR record
 * of a layout this firmware cannot read) are dropped, not rejected.
 * serialize writes version 3 with the func configs of line, track, gesture,
 * shape and motion, then a line record if a colour is learned, an object
 * record if an object is learned and a QR record if a code is learned.
 * Adding a function's config or learned data later does not need a new blob
 * version (M7 added shape, motion and QR this way: M6 firmware skips them).
 * Worst case: 1719 (v1) + 1 + 16*10 + 1 + (4 + 3) + (4 + 77 + 32*32) +
 * (4 + 1 + 8*(4 + 128)) = 4054 bytes (a blob from newer firmware with more
 * learned records can be larger and is then not loaded). */
#define MXST_BLOB_MAX                                                                  \
  (4 + 1 + MXP_CONFIG_SIZE + 1 + MXST_MAX_COLORS * 4 + 1 + MXST_MAX_NAMES * (4 + MXP_NAME_MAX) + \
   1 + MXST_MAX_FCFG * MXP_FUNC_CONFIG_SIZE + 1 + 4 + MXST_LINE_REC_LEN + 4 + MXST_OBJ_REC_MAX + \
   4 + MXST_QR_REC_MAX + 2)

/* Defaults = protocol.md 7 config defaults, 0 colours, 0 names, func configs
 * at their defaults (mxp_line/track/gesture/shape/motion_config_defaults), no
 * line colour, no object, no QR codes. */
void mxst_defaults(mxs_data_t *d);

/* Returns bytes written, or 0 when cap is too small or d has invalid content
 * (including func config values outside their protocol ranges). */
size_t mxst_serialize(const mxs_data_t *d, uint8_t *out, size_t cap);

/* Reads v1, v2 and v3. Rejects bad magic/version/CRC/truncation/trailing
 * bytes/out-of-range values (see the record list rules above).
 * *out is modified only on success. */
bool mxst_deserialize(const uint8_t *p, size_t len, mxs_data_t *out);

/* Name table. len 0 removes; an existing (fn,id) is replaced. false when the
 * table is full, fn/id invalid (colour 1: id 1-7; AprilTag 2: id 0-586;
 * object 5: id 1; QR 6: a learned id; gesture 8: id 1-9; shape 11: id 1-4;
 * face 3, line 4, motion 12 and the rest: never), len > MXP_NAME_MAX or the
 * bytes are not valid UTF-8 (mxp_utf8_valid). */
bool mxst_set_name(mxs_data_t *d, uint8_t fn, uint16_t id, const char *utf8, uint8_t len);
/* NULL when absent; *len_out (optional) receives the byte length. */
const char *mxst_get_name(const mxs_data_t *d, uint8_t fn, uint16_t id, uint8_t *len_out);
/* Forget one learned colour (and its name); id 0xFF forgets all colours and
 * all colour names. */
void mxst_forget_color(mxs_data_t *d, uint8_t id);
/* Remove the name of (fn, id); id 0xFFFF removes every name of fn. */
void mxst_forget_names(mxs_data_t *d, uint8_t fn, uint16_t id);

/* LEARN bookkeeping for a sampled colour. If it matches a learned colour
 * (mxc_same_color with `tolerance`; the lowest such id wins) that id's rgb is
 * replaced; otherwise it is added with the lowest free id 1-7 (colours stay
 * sorted by id). Returns the id, MXST_LEARN_FULL when 7 are learned and none
 * matches, or MXST_LEARN_NO_COLOR when rgb itself is too grey to ever match. */
#define MXST_LEARN_NO_COLOR 0
#define MXST_LEARN_FULL (-1)
int mxst_learn_color(mxs_data_t *d, const uint8_t rgb[3], uint8_t tolerance);

/* Line colour (LEARN / FORGET 4). rgb: stores it and sets line_cfg.mode to 2
 * (learned colour); false and nothing changed when rgb is too grey to ever
 * match (same rule as mxst_learn_color). NULL: clears the colour and, if the
 * mode is 2, sets it back to 0 (mode 1 is kept); always true. */
bool mxst_set_line_color(mxs_data_t *d, const uint8_t *rgb);
/* Object (LEARN / FORGET 5). m: replaces the object (template bytes beyond
 * tw*th zeroed); false and nothing changed when !mxk_model_valid(m). NULL:
 * clears it; always true. Both remove the name of (5, 1). */
bool mxst_set_object(mxs_data_t *d, const mxk_model_t *m);

/* QR table (LEARN / FORGET 6, protocol.md 11). bytes = the code's first len
 * bytes, len = min(total_len, 128). A learned code (same total_len and bytes)
 * -> its id, nothing changed; a new one -> the lowest free id 1-8 (the table
 * stays sorted by id); MXST_LEARN_FULL when 8 are learned; MXST_LEARN_NO_COLOR
 * (nothing learned) when bytes is NULL, len is 0 or len != min(total_len, 128). */
int mxst_learn_qr(mxs_data_t *d, const uint8_t *bytes, uint8_t len, uint16_t total_len);
/* The id of the learned code with this content (same rule), 0 = not learned. */
uint16_t mxst_qr_lookup(const mxs_data_t *d, const uint8_t *bytes, uint8_t len, uint16_t total_len);
/* NULL when id is not learned. */
const mxst_qr_t *mxst_qr_get(const mxs_data_t *d, uint16_t id);
/* Forget a learned code and its name; 0xFFFF forgets all codes and all QR
 * names. An absent id is a no-op. */
void mxst_forget_qr(mxs_data_t *d, uint16_t id);

#ifdef ESP_PLATFORM
/* Not reentrant (one static 4 KB blob buffer shared by load and save): call
 * load/save/erase from one task at a time (boot code, then the save worker).
 * Stack: load deserialises through an mxs_data_t (~4 KB) on the stack, so
 * give the caller >= 6 KB; save/erase need ~3 KB (NVS internals).
 *
 * NVS (namespace "mxcam", single blob key "data"). Each call initialises NVS
 * lazily (nvs_flash_init, erasing on NO_FREE_PAGES/NEW_VERSION_FOUND), so no
 * prior init is required. The whole data is one nvs_set_blob + nvs_commit; NVS
 * makes a per-key set atomic, so a power cut leaves the old or the new blob,
 * never a mix.
 * load: missing key -> defaults + ESP_OK; corrupt blob -> defaults + warning +
 * ESP_ERR_INVALID_CRC. Other errors: defaults + that error. */
esp_err_t mxst_load(mxs_data_t *d);
esp_err_t mxst_save(const mxs_data_t *d);
esp_err_t mxst_erase(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
