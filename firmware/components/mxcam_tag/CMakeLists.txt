# Thin wrapper over the vendored apriltag component (ADR-0008); pure C apart
# from esp_timer, so it also builds in the host test.
idf_component_register(SRCS "mxcam_tag.c"
                       INCLUDE_DIRS "."
                       REQUIRES mxcam_protocol apriltag
                       PRIV_REQUIRES esp_timer)
