# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{ADF_PATH})
    message(FATAL_ERROR "ADF_PATH environment variable not set; required for ESP-ADF audio playback")
endif()
if(NOT EXISTS $ENV{ADF_PATH}/components)
    message(FATAL_ERROR "ADF_PATH components directory not found at '$ENV{ADF_PATH}/components'")
endif()

set(ADF_PATH $ENV{ADF_PATH})
list(APPEND EXTRA_COMPONENT_DIRS ${ADF_PATH}/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(coinbox)
