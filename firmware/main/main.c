#include <stdint.h>
#include <string.h>

#include "encoder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mxcam_camera.h"
#include "mxcam_display.h"
#include "mxcam_face.h"
#include "mxcam_git.h" /* generated at every build: MXCAM_FW_GIT, MXCAM_FW_GIT_DESC */
#include "mxcam_link.h"
#include "mxcam_overlay.h"
#include "mxcam_proto.h"
#include "mxcam_service.h"
#include "mxcam_store.h"
#include "mxcam_tag.h"
#include "pipeline.h"
#include "status_led.h"
#include "store_worker.h"

static const char *TAG = "mxcam";

static mxs_state_t s_state;
static mxs_data_t s_data;

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
    info->max_results = MXP_MAX_RESULTS;
    info->function_count = 0; /* filled by mxs_set_capabilities from what initialised */
}

/* Camera failed but the panel works: show why (the pipeline is not running). */
static void show_fatal(const char *line1, const char *line2)
{
    uint16_t *px = heap_caps_calloc((size_t)MXCAM_LCD_W * MXCAM_LCD_H, sizeof(uint16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!px) return;
    mxo_canvas_t c = { px, MXCAM_LCD_W, MXCAM_LCD_H };
    /* The panel mirrors left-right (ADR-0010): draw text mirrored. */
    mxo_draw_text_mirrored(&c, 8, 100, line1, MXO_WHITE, MXO_RED);
    if (line2) mxo_draw_text_mirrored(&c, 8, 116, line2, MXO_WHITE, MXO_BLACK);
    mxcam_display_draw(px);
    /* mxcam_display_draw returns once the last strip is queued; keep px. */
}

void app_main(void)
{
    ESP_LOGI(TAG, "MXCam firmware %d.%d.%d (%s)", MXCAM_FW_VERSION_MAJOR, MXCAM_FW_VERSION_MINOR,
             MXCAM_FW_VERSION_PATCH, MXCAM_FW_GIT_DESC);
    ESP_LOGI(TAG, "PSRAM size: %u bytes", (unsigned)esp_psram_get_size());

    /* Store first: the config (LED brightness, detection params) applies from
     * the first frame. UART baud / I2C address are only stored (used in M4,
     * after save + reboot). */
    esp_err_t le = mxst_load(&s_data);
    if (le == ESP_OK)
        ESP_LOGI(TAG, "store: %u colours, %u names, tolerance %u, led %u", s_data.n_colors, s_data.n_names,
                 s_data.cfg.tolerance, s_data.cfg.led);
    else
        ESP_LOGW(TAG, "store load: %s (using defaults)", esp_err_to_name(le));

    status_led_init();
    status_led_set(STATUS_LED_IDLE, s_data.cfg.led);

    const char *boot_error = NULL; /* shown on the TFT; ASCII only */
    bool display_ok = mxcam_display_init() == ESP_OK;
    if (!display_ok) {
        ESP_LOGE(TAG, "display init failed");
        boot_error = "DISPLAY INIT FAILED";
    }
    esp_err_t cam_err = mxcam_camera_init();
    bool camera_ok = cam_err == ESP_OK;
    if (!camera_ok) {
        ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(cam_err));
        boot_error = cam_err == ESP_ERR_INVALID_STATE ? "CAMERA MIRROR FAILED" : "CAMERA INIT FAILED";
    }
    /* Not a boot error: colour still works; AprilTag is just left out of
     * GET_INFO (SET_FUNCTION 2 -> BAD_PARAM). LED stays blue, log only. */
    bool tag_ok = mxt_init();
    if (!tag_ok) ESP_LOGE(TAG, "AprilTag init failed (no memory?): function 2 disabled");
    /* Face models (~80 ms, ~425 KB PSRAM). Must run HERE, before the encoder,
     * link, pipeline and store tasks are created: while the models load,
     * mxf_init steers every plain malloc to PSRAM, for any task that
     * allocates during that ~80 ms window, including IDF system and driver
     * tasks that are already running (e.g. the camera driver). Only when the
     * pipeline can run; a failure just leaves function 3 out of GET_INFO. */
    bool face_ok = false;
    if (display_ok && camera_ok) {
        int64_t tf = esp_timer_get_time();
        face_ok = mxf_init();
        if (face_ok)
            ESP_LOGI(TAG, "face detection ready in %u ms", (unsigned)((esp_timer_get_time() - tf) / 1000));
        else
            ESP_LOGE(TAG, "face detection init failed (no memory?): function 3 disabled");
    }

    mxp_info_t info;
    build_info(&info);
    mxs_init(&s_state, &info, &s_data);
    /* GET_INFO lists only functions that can run; the link is not started yet,
     * so no lock is needed here. */
    uint8_t caps[3], n_caps = 0;
    if (display_ok && camera_ok) {
        caps[n_caps++] = MXP_FUNC_COLOR;
        if (tag_ok) caps[n_caps++] = MXP_FUNC_APRILTAG;
        if (face_ok) caps[n_caps++] = MXP_FUNC_FACE;
    }
    mxs_set_capabilities(&s_state, caps, n_caps);
    if (encoder_start() != ESP_OK) {
        ESP_LOGE(TAG, "encoder start failed (no FRAME pushes)");
        if (!boot_error) boot_error = "JPEG ENCODER FAILED";
    }
    /* The link starts whatever failed above, so GET_INFO still answers. */
    if (mxcam_link_start(&s_state) != ESP_OK) {
        ESP_LOGE(TAG, "USB link start failed");
        if (!boot_error) boot_error = "USB LINK FAILED";
    }

    bool run_pipeline = display_ok && camera_ok;
    /* Before the pipeline starts: from then on only the pipeline drives the LED. */
    if (boot_error) status_led_set(STATUS_LED_ERROR, s_data.cfg.led);
    if (run_pipeline && pipeline_start(&s_state, boot_error) != ESP_OK) {
        ESP_LOGE(TAG, "pipeline start failed");
        run_pipeline = false;
        boot_error = "PIPELINE START FAILED";
        status_led_set(STATUS_LED_ERROR, s_data.cfg.led);
        mxcam_link_state_lock();
        mxs_set_capabilities(&s_state, NULL, 0); /* nothing can run: no LEARN waits forever */
        mxcam_link_state_unlock();
    }
    /* Without the pipeline the worker also runs the stream/host timeout. */
    if (store_worker_start(&s_state, !run_pipeline) != ESP_OK) ESP_LOGE(TAG, "store worker start failed");
    if (!run_pipeline && display_ok) show_fatal(boot_error, "USB still answers GET_INFO");
    ESP_LOGI(TAG, "boot done: internal heap free %u, PSRAM free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
