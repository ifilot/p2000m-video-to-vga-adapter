cmake_minimum_required(VERSION 3.13)

# Keep the SDK and extras versions reproducible across local and CI builds.
set(P2000M_PICO_SDK_VERSION "2.3.0")

# This firmware uses RP2350-specific APIs and is only supported on Pico 2. Set
# both cache values explicitly so a plain `cmake ..` cannot silently configure
# for the SDK's legacy Pico/RP2040 default, even in a previously failed build.
set(PICO_BOARD "pico2" CACHE STRING "Target Raspberry Pi Pico board" FORCE)
set(PICO_PLATFORM "rp2350-arm-s" CACHE STRING
    "Target Raspberry Pi silicon platform" FORCE)

# CMake can fetch source dependencies, but it still needs an executable Arm
# cross-compiler. Check this before downloading anything and provide a focused
# error instead of letting compiler discovery fail from inside pico-extras.
if(NOT PICO_TOOLCHAIN_PATH AND DEFINED ENV{PICO_TOOLCHAIN_PATH})
    set(PICO_TOOLCHAIN_PATH "$ENV{PICO_TOOLCHAIN_PATH}")
endif()
find_program(P2000M_ARM_GCC
    NAMES arm-none-eabi-gcc
    HINTS "${PICO_TOOLCHAIN_PATH}" "${PICO_TOOLCHAIN_PATH}/bin"
)
if(NOT P2000M_ARM_GCC)
    message(FATAL_ERROR
        "Arm GNU compiler 'arm-none-eabi-gcc' was not found. Install the "
        "Arm GNU embedded toolchain or set PICO_TOOLCHAIN_PATH to its root.")
endif()

# Explicit paths still take precedence. Without them, the bundled import
# helpers fetch both dependencies into the selected CMake build directory.
if(NOT PICO_SDK_PATH)
    set(PICO_SDK_FETCH_FROM_GIT ON CACHE BOOL
        "Automatically download the Raspberry Pi Pico SDK")
    set(PICO_SDK_FETCH_FROM_GIT_TAG "${P2000M_PICO_SDK_VERSION}"
        CACHE STRING "Pico SDK version used by this firmware")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake")

if(NOT PICO_EXTRAS_PATH)
    set(PICO_EXTRAS_FETCH_FROM_GIT ON CACHE BOOL
        "Automatically download pico-extras")
    set(PICO_EXTRAS_FETCH_FROM_GIT_TAG "sdk-${P2000M_PICO_SDK_VERSION}"
        CACHE STRING "pico-extras version used by this firmware")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/cmake/pico_extras_import.cmake")

project(p2000m_vid2vga_firmware VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_subdirectory(src)
