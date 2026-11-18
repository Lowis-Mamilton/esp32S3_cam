idf_component_register(SRCS "mxcam_camera.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp32-camera
                       PRIV_REQUIRES log)
