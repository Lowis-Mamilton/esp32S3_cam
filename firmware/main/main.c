#include <stdint.h>
#include "esp_log.h"
#include "esp_psram.h"
#include "mxcam_proto.h"

static const char *TAG = "mxcam";

void app_main(void)
{
    ESP_LOGI(TAG, "MXCam firmware %d.%d.%d (%s)", MXCAM_FW_VERSION_MAJOR,
             MXCAM_FW_VERSION_MINOR, MXCAM_FW_VERSION_PATCH, MXCAM_FW_GIT);
    ESP_LOGI(TAG, "PSRAM size: %u bytes", (unsigned)esp_psram_get_size());

    /* Link check for the mxcam_protocol component. */
    static const uint8_t probe[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    ESP_LOGI(TAG, "crc16 probe: 0x%04x", (unsigned)mxp_crc16(probe, sizeof(probe)));
}
