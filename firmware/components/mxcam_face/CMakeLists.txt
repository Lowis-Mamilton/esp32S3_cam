# C API over Espressif's human_face_detect / esp-dl (C++). The post-processing
# (mxcam_face_post.c) is pure C and also builds in the host test.
idf_component_register(SRCS "mxcam_face.cpp" "mxcam_face_post.c"
                       INCLUDE_DIRS "."
                       REQUIRES mxcam_protocol
                       PRIV_REQUIRES espressif__human_face_detect esp_timer heap log)
