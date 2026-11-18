idf_component_register(SRCS "main.c" "pipeline.c"
                       REQUIRES esp_psram esp_timer freertos log mxcam_protocol mxcam_camera mxcam_display mxcam_overlay)
