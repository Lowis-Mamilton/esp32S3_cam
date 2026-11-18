#include <stdint.h>
#include <string.h>
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "mxcam_proto.h"
#include "mxcam_camera.h"
#include "mxcam_display.h"
#include "mxcam_link.h"
#include "mxcam_service.h"
#include "encoder.h"
#include "pipeline.h"

static const char *TAG = "mxcam";

static mxs_state_t s_state;

static void build_info(mxp_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->protocol_version = MXP_VERSION;
    info->fw_major = MXCAM_FW_VERSION_MAJOR;
    info->fw_minor = MXCAM_FW_VERSION_MINOR;
    info->fw_patch = MXCAM_FW_VERSION_PATCH;
    size_t g = strlen(MXCAM_FW_GIT);
    memcpy(info->fw_git, MXCAM_FW_GIT, g > sizeof(info->fw_git) ? sizeof(info->fw_git) : g);
    esp_read_mac(info->serial, ESP_MAC_WIFI_STA);
    info->frame_width = MXCAM_LCD_W;
    info->frame_height = MXCAM_LCD_H;
    info->max_results = 8;
    info->function_count = 0; /* M1: only "none" */
}

void app_main(void)
{
    ESP_LOGI(TAG, "MXCam firmware %d.%d.%d (%s)", MXCAM_FW_VERSION_MAJOR,
             MXCAM_FW_VERSION_MINOR, MXCAM_FW_VERSION_PATCH, MXCAM_FW_GIT);
    ESP_LOGI(TAG, "PSRAM size: %u bytes", (unsigned)esp_psram_get_size());

    /* Link check for the mxcam_protocol component. */
    static const uint8_t probe[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    ESP_LOGI(TAG, "crc16 probe: 0x%04x", (unsigned)mxp_crc16(probe, sizeof(probe)));

    if (mxcam_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    if (mxcam_camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed");
        return;
    }

    mxp_info_t info;
    build_info(&info);
    mxs_init(&s_state, &info);
    if (encoder_start() != ESP_OK) {
        ESP_LOGE(TAG, "encoder start failed");
        return;
    }
    if (mxcam_link_start(&s_state) != ESP_OK) {
        ESP_LOGE(TAG, "USB link start failed");
        return;
    }
    pipeline_start(&s_state);
}
