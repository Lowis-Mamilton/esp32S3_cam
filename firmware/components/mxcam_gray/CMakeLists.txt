# Pure C component (no ESP-IDF headers) so it also builds in host tests
# (covered by mxcam_line/host-test).
idf_component_register(SRCS "mxcam_gray.c"
                       INCLUDE_DIRS ".")
