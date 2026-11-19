idf_component_register(SRCS "main.c" "pipeline.c" "encoder.c" "status_led.c" "store_worker.c"
                       REQUIRES esp_psram esp_timer freertos log heap mxcam_protocol mxcam_camera mxcam_display
                                mxcam_overlay mxcam_service mxcam_link mxcam_store mxcam_color mxcam_tag
                                esp_hw_support)

# Git identity, regenerated at every build (not only at configure time).
set(MXCAM_GIT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/mxcam_git.h")
add_custom_target(mxcam_git_header
    COMMAND ${CMAKE_COMMAND} -DREPO=${CMAKE_CURRENT_LIST_DIR}/../.. -DOUT=${MXCAM_GIT_HEADER}
            -P ${CMAKE_CURRENT_LIST_DIR}/gen_git_header.cmake
    BYPRODUCTS ${MXCAM_GIT_HEADER}
    COMMENT "Updating mxcam_git.h"
    VERBATIM)
add_dependencies(${COMPONENT_LIB} mxcam_git_header)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
