# Sources only in builds with NimBLE (sdkconfig.ble, ADR-0022); otherwise this
# is a header-only component and nothing calls it.
set(srcs)
if(CONFIG_BT_NIMBLE_ENABLED)
    list(APPEND srcs "mxcam_ble.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES mxcam_link
                       PRIV_REQUIRES bt esp_timer freertos log esp_hw_support)
