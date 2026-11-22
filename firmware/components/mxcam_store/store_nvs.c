/* ESP-only: NVS persistence for mxcam_store. Not part of the host build. */
#include "esp_log.h"
#include "mxcam_store.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "mxst";
#define NS "mxcam"
#define KEY "data"

/* One blob buffer (MXST_BLOB_MAX, ~4 KB since M7's QR table) for load and
 * save: they never run at the same time (boot code, then the save worker; see
 * mxcam_store.h). Static, so neither caller's stack holds it. */
static uint8_t s_buf[MXST_BLOB_MAX];

static esp_err_t ensure_init(void) {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(e));
    e = nvs_flash_erase();
    if (e == ESP_OK) e = nvs_flash_init();
  }
  return e;
}

esp_err_t mxst_load(mxs_data_t *d) {
  mxst_defaults(d);
  esp_err_t e = ensure_init();
  if (e != ESP_OK) return e;
  nvs_handle_t h;
  e = nvs_open(NS, NVS_READONLY, &h);
  if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK; /* first boot: namespace not created yet */
  if (e != ESP_OK) return e;
  size_t len = sizeof(s_buf);
  e = nvs_get_blob(h, KEY, s_buf, &len);
  nvs_close(h);
  if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK; /* first boot */
  if (e == ESP_ERR_NVS_INVALID_LENGTH) {
    ESP_LOGW(TAG, "stored blob too large, using defaults");
    return ESP_ERR_INVALID_CRC;
  }
  if (e != ESP_OK) return e;
  /* d holds the defaults; deserialize changes it only on success */
  if (!mxst_deserialize(s_buf, len, d)) {
    ESP_LOGW(TAG, "stored blob corrupt, using defaults");
    return ESP_ERR_INVALID_CRC;
  }
  return ESP_OK;
}

esp_err_t mxst_save(const mxs_data_t *d) {
  size_t n = mxst_serialize(d, s_buf, sizeof(s_buf));
  if (n == 0) return ESP_ERR_INVALID_ARG;
  esp_err_t e = ensure_init();
  if (e != ESP_OK) return e;
  nvs_handle_t h;
  e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  /* One key, one set + commit: NVS applies a per-key set atomically, so a power
   * cut leaves the previous blob or the new one. */
  e = nvs_set_blob(h, KEY, s_buf, n);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e;
}

esp_err_t mxst_erase(void) {
  esp_err_t e = ensure_init();
  if (e != ESP_OK) return e;
  nvs_handle_t h;
  e = nvs_open(NS, NVS_READWRITE, &h);
  if (e != ESP_OK) return e;
  e = nvs_erase_key(h, KEY);
  if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e;
}
