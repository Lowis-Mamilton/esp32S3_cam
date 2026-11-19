# C API over Espressif's hand_detect + hand_gesture_recognition / esp-dl (C++).
# The post-processing (mxcam_gesture_post.c) is pure C and also builds in the
# host test.
idf_component_register(SRCS "mxcam_gesture.cpp" "mxcam_gesture_post.c"
                       INCLUDE_DIRS "."
                       REQUIRES mxcam_protocol
                       PRIV_REQUIRES espressif__hand_detect espressif__hand_gesture_recognition esp_timer heap log)
