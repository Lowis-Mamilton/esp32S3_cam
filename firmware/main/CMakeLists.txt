idf_component_register(SRCS "main.c" "pipeline.c" "encoder.c"
                       REQUIRES esp_psram esp_timer freertos log mxcam_protocol mxcam_camera mxcam_display mxcam_overlay mxcam_service mxcam_link mxcam_store esp_hw_support)
