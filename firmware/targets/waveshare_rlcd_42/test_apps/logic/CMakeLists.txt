cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../../../components/usage_core"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../components/usage_panel"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(waveshare_rlcd_42_logic_tests)
