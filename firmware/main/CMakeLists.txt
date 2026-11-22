idf_component_register(SRCS "main.c" "pipeline.c" "encoder.c" "status_led.c" "store_worker.c" "gesture_bench.c"
                            "tracking.c"
                       REQUIRES esp_psram esp_timer freertos log heap mxcam_protocol mxcam_camera mxcam_display
                                mxcam_overlay mxcam_service mxcam_link mxcam_store mxcam_color mxcam_tag
                                mxcam_face mxcam_gesture mxcam_line mxcam_track mxcam_gray mxcam_ble esp_hw_support)

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

# Gesture recognition on-board benchmark (gesture_bench.c), local builds only:
#   idf.py -DMXCAM_GESTURE_BENCH=ON build   (and -DMXCAM_GESTURE_BENCH=OFF to go
#   back; the value stays in the CMake cache). Never enabled in committed
#   builds/CI. Optional MXCAM_GESTURE_BENCH_IMAGE: a file of whole 320x240
#   RGB565 big-endian frames (camera byte order) that the benchmark also runs
#   on, so it can be checked without a hand in front of the camera.
#   With -DMXCAM_GESTURE_BENCH_FEED=ON (needs the image) the test frames
#   replace the camera frame on a schedule instead (pipeline speed with known
#   hands; gesture_bench.h).
option(MXCAM_GESTURE_BENCH "Log a gesture recognition benchmark once per second" OFF)
option(MXCAM_GESTURE_BENCH_FEED "Feed the gesture test frames into the pipeline instead" OFF)
set(MXCAM_GESTURE_BENCH_IMAGE "" CACHE FILEPATH "Test frames for the gesture benchmark (RGB565BE 320x240)")
if(MXCAM_GESTURE_BENCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MXCAM_GESTURE_BENCH)
    if(MXCAM_GESTURE_BENCH_IMAGE)
        target_add_binary_data(${COMPONENT_LIB} "${MXCAM_GESTURE_BENCH_IMAGE}" BINARY RENAME_TO gesture_bench_img)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MXCAM_GESTURE_BENCH_IMAGE)
        if(MXCAM_GESTURE_BENCH_FEED)
            target_compile_definitions(${COMPONENT_LIB} PRIVATE MXCAM_GESTURE_BENCH_FEED)
        endif()
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

# Hand landmark prototype phase B (handlm_bench.cpp), local builds only:
#   idf.py -DMXCAM_HANDLM_BENCH=ON build   (and =OFF to go back; the value stays
#   in the CMake cache). Models + test data go to the `storage` partition with
#   tools/handlm/pack_storage.py + esptool, not into the app image. Never
#   enabled in committed builds/CI. docs/research/hand-landmarks-proto-b.md
#   -DMXCAM_HANDLM_BENCH_EARLY=ON loads the first storage model at boot BEFORE
#   the face / gesture models (afterwards no PSRAM block is big enough for the
#   int16-depthwise variants); -DMXCAM_HANDLM_BENCH_ONLY=<n> benches only
#   storage entry n (once).
# Source and libraries are added here, not in idf_component_register: the
# requirement scan does not see -D cache values, and default builds must not
# depend on them.
option(MXCAM_HANDLM_BENCH "Hand landmark on-device benchmark (models from the storage partition)" OFF)
option(MXCAM_HANDLM_BENCH_EARLY "Load the landmark model before the face/gesture models" OFF)
if(MXCAM_HANDLM_BENCH)
    target_sources(${COMPONENT_LIB} PRIVATE "handlm_bench.cpp")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MXCAM_HANDLM_BENCH)
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::espressif__esp-dl idf::esp_partition idf::spi_flash)
    if(MXCAM_HANDLM_BENCH_EARLY)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MXCAM_HANDLM_BENCH_EARLY)
    endif()
    # -DMXCAM_HANDLM_BENCH_ONLY=<n>: only storage entry n (with _EARLY: loaded first at boot).
    set(MXCAM_HANDLM_BENCH_ONLY "" CACHE STRING "Only bench this storage directory entry")
    if(NOT MXCAM_HANDLM_BENCH_ONLY STREQUAL "")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MXCAM_HANDLM_BENCH_ONLY=${MXCAM_HANDLM_BENCH_ONLY})
    endif()
endif()
