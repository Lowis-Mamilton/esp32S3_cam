idf_component_register(SRCS "mxcam_display.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd esp_lcd_ili9341
                       PRIV_REQUIRES driver freertos log)
