cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(PROJECT_VER "0.1.0")

# Project-wide compile definitions (visible to all components). The git hash is
# generated at every build into mxcam_git.h (main/CMakeLists.txt).
foreach(_def
        "MXCAM_FW_VERSION_MAJOR=0"
        "MXCAM_FW_VERSION_MINOR=1"
        "MXCAM_FW_VERSION_PATCH=0")
    idf_build_set_property(COMPILE_DEFINITIONS "${_def}" APPEND)
endforeach()

project(mxcam)
