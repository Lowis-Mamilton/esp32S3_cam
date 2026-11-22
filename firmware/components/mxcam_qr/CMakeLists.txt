# C API over Espressif's quirc port (QR code detection + decoding, ADR-0020).
# The post-processing (mxcam_qr_post.c) is pure C and also builds in the
# host test.
idf_component_register(SRCS "mxcam_qr.c" "mxcam_qr_post.c"
                       INCLUDE_DIRS "."
                       REQUIRES mxcam_protocol
                       PRIV_REQUIRES mxcam_gray espressif__quirc esp_timer heap log)
