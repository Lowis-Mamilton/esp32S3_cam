#include "esp_log.h"
#include "esp_psram.h"

static const char *TAG = "mxcam";

void app_main(void)
{
    ESP_LOGI(TAG, "MXCam firmware boot");
    ESP_LOGI(TAG, "PSRAM size: %u bytes", (unsigned)esp_psram_get_size());
}
