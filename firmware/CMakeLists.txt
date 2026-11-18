cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(PROJECT_VER "0.1.0")

# Short git hash of the repo root, computed at configure time.
# Falls back to "unknown" (e.g. no git, or "dubious ownership" in CI containers).
execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}/.." rev-parse --short=7 HEAD
    OUTPUT_VARIABLE MXCAM_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE MXCAM_GIT_RESULT)
if(NOT MXCAM_GIT_RESULT EQUAL 0 OR MXCAM_GIT_HASH STREQUAL "")
    set(MXCAM_GIT_HASH "unknown")
endif()

# Project-wide compile definitions (visible to all components).
foreach(_def
        "MXCAM_FW_VERSION_MAJOR=0"
        "MXCAM_FW_VERSION_MINOR=1"
        "MXCAM_FW_VERSION_PATCH=0"
        "MXCAM_FW_GIT=\"${MXCAM_GIT_HASH}\"")
    idf_build_set_property(COMPILE_DEFINITIONS "${_def}" APPEND)
endforeach()

project(mxcam)
