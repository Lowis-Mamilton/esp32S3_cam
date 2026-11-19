#ifndef MXCAM_STORE_H
#define MXCAM_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mxcam_color.h"
#include "mxcam_proto.h"

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
#define MXST_TAG_ID_MAX 586

typedef struct {
  uint8_t fn;
  uint16_t id;
  uint8_t len;                 /* explicit byte length of name (UTF-8) */
  char name[MXP_NAME_MAX + 1]; /* also NUL-terminated */
} mxs_name_t;

typedef struct {
  mxp_config_t cfg;
  mxc_learned_t colors[MXST_MAX_COLORS];
  uint8_t n_colors;
  mxs_name_t names[MXST_MAX_NAMES];
  uint8_t n_names;
} mxs_data_t;

/* Blob layout (all little-endian):
 *   "MXST"(4) | version u8 = 1 | config (18, dirty forced 0) |
 *   n_colors u8, n x { id u8, rgb[3] } |
 *   n_names u8, n x { fn u8, id u16, len u8, bytes[len] } |
 *   CRC16 (mxp_crc16) over everything before it, u16.
 * Worst case: 4+1+18+1+7*4+1+32*(4+48)+2 = 1719 bytes. */
#define MXST_BLOB_MAX                                                                  \
  (4 + 1 + MXP_CONFIG_SIZE + 1 + MXST_MAX_COLORS * 4 + 1 + MXST_MAX_NAMES * (4 + MXP_NAME_MAX) + 2)

/* Defaults = protocol.md 7 config defaults, 0 colours, 0 names. */
void mxst_defaults(mxs_data_t *d);

/* Returns bytes written, or 0 when cap is too small or d has invalid content. */
size_t mxst_serialize(const mxs_data_t *d, uint8_t *out, size_t cap);

/* Rejects bad magic/version/CRC/truncation/trailing bytes/out-of-range values.
 * *out is modified only on success. */
bool mxst_deserialize(const uint8_t *p, size_t len, mxs_data_t *out);

/* Name table. len 0 removes; an existing (fn,id) is replaced. false when the
 * table is full, fn/id invalid (fn 1/2/3; colour id 1-7; tag id 0-586), or
 * len > MXP_NAME_MAX. */
bool mxst_set_name(mxs_data_t *d, uint8_t fn, uint16_t id, const char *utf8, uint8_t len);
/* NULL when absent; *len_out (optional) receives the byte length. */
const char *mxst_get_name(const mxs_data_t *d, uint8_t fn, uint16_t id, uint8_t *len_out);
/* Forget one learned colour (and its name); id 0xFF forgets all colours and
 * all colour names. */
void mxst_forget_color(mxs_data_t *d, uint8_t id);

#ifdef ESP_PLATFORM
/* NVS (namespace "mxcam", single blob key "data"). Each call initialises NVS
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
