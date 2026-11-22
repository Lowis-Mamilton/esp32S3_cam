# Pure C component (no ESP-IDF headers except an optional EXT_RAM_BSS_ATTR) so it also builds in host tests.
idf_component_register(SRCS "mxcam_motion.c"
                       INCLUDE_DIRS "."
                       REQUIRES mxcam_protocol mxcam_gray)
