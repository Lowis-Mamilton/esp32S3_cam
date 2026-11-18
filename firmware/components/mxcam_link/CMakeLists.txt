idf_component_register(SRCS "mxcam_link.c"
                       INCLUDE_DIRS "."
                       REQUIRES mxcam_service mxcam_protocol
                       PRIV_REQUIRES esp_driver_usb_serial_jtag esp_timer freertos log)
