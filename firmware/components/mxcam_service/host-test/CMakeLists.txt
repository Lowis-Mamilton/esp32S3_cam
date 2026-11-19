cmake_minimum_required(VERSION 3.16)
project(mxcam_service_host_test C)
enable_testing()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(PROTO_SRC "${REPO_ROOT}/arduino-lib/src/protocol")
set(SVC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(ST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../mxcam_store")
set(COL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../mxcam_color")
set(TRK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../mxcam_track")
set(GRAY_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../mxcam_gray")

# store_nvs.c is ESP-only and intentionally not built here.
add_executable(test_service test_service.c "${SVC_SRC}/mxcam_service.c" "${ST_SRC}/mxcam_store.c"
               "${COL_SRC}/mxcam_color.c" "${TRK_SRC}/mxcam_track.c" "${GRAY_SRC}/mxcam_gray.c"
               "${PROTO_SRC}/mxcam_proto.c")
target_include_directories(test_service PRIVATE "${SVC_SRC}" "${ST_SRC}" "${COL_SRC}" "${TRK_SRC}" "${GRAY_SRC}"
                           "${PROTO_SRC}")
set_target_properties(test_service PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(test_service PRIVATE -std=c99 -Wall -Wextra -Werror)
if(NOT MSVC)
  target_link_libraries(test_service PRIVATE m)
endif()
add_test(NAME test_service COMMAND test_service)
