# Pure C component (no ESP-IDF headers) so it also builds in host tests.
idf_component_register(SRCS "mxcam_overlay.c"
                       INCLUDE_DIRS ".")
