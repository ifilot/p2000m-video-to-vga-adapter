cmake_minimum_required(VERSION 3.13)

if(NOT PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
endif()

if(NOT PICO_EXTRAS_PATH AND DEFINED ENV{PICO_EXTRAS_PATH})
    set(PICO_EXTRAS_PATH "$ENV{PICO_EXTRAS_PATH}")
endif()

if(NOT EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    message(FATAL_ERROR
        "Pico SDK not found. Set PICO_SDK_PATH to a Pico SDK 2.x checkout.")
endif()

if(NOT EXISTS "${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake")
    message(FATAL_ERROR
        "pico-extras not found. Set PICO_EXTRAS_PATH to a pico-extras checkout.")
endif()

include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
include("${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake")

project(p2000m_video_to_vga C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_subdirectory(src)
