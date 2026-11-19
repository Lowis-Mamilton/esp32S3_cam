idf_component_register(SRCS "main.c" "pipeline.c" "encoder.c" "status_led.c" "store_worker.c"
                       REQUIRES esp_psram esp_timer freertos log heap mxcam_protocol mxcam_camera mxcam_display
                                mxcam_overlay mxcam_service mxcam_link mxcam_store mxcam_color mxcam_tag
                                mxcam_face esp_hw_support)

# Face detection on-board benchmark (pipeline.c face_bench), local builds only:
#   idf.py -DMXCAM_FACE_BENCH=ON build   (and -DMXCAM_FACE_BENCH=OFF to go back;
#   the value stays in the CMake cache). Never enabled in committed builds/CI.
# Optional MXCAM_FACE_BENCH_IMAGE: a file of whole 320x240 RGB565 big-endian
# frames (camera byte order) that the benchmark also runs on, so it can be
# checked without a face in front of the camera.
option(MXCAM_FACE_BENCH "Log a face detection benchmark once per second" OFF)
set(MXCAM_FACE_BENCH_IMAGE "" CACHE FILEPATH "Test frames for the face benchmark (RGB565BE 320x240)")
if(MXCAM_FACE_BENCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MXCAM_FACE_BENCH)
    if(MXCAM_FACE_BENCH_IMAGE)
        target_add_binary_data(${COMPONENT_LIB} "${MXCAM_FACE_BENCH_IMAGE}" BINARY RENAME_TO face_bench_img)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MXCAM_FACE_BENCH_IMAGE)
    endif()
endif()

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
