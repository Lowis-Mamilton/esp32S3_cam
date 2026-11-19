# mxcam_store.c is pure C (host-tested); store_nvs.c is ESP-only.
idf_component_register(SRCS "mxcam_store.c" "store_nvs.c"
                       INCLUDE_DIRS "."
                       REQUIRES mxcam_protocol mxcam_color mxcam_track
                       PRIV_REQUIRES nvs_flash log)
