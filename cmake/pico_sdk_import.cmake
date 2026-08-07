# Bundled Pico SDK import helper, based on the SDK 2.3.0 import file.
#
# Copyright 2020 Raspberry Pi (Trading) Ltd.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
# 3. Neither the name of the copyright holder nor the names of its contributors
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE,
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

# This file must be included before project() so the SDK can select the Arm
# cross-compilation toolchain. A caller-provided PICO_SDK_PATH takes precedence;
# otherwise the pinned SDK tag is downloaded into the CMake dependency area.

if(DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
endif()

if(DEFINED ENV{PICO_SDK_FETCH_FROM_GIT} AND
   NOT DEFINED PICO_SDK_FETCH_FROM_GIT)
    set(PICO_SDK_FETCH_FROM_GIT "$ENV{PICO_SDK_FETCH_FROM_GIT}")
endif()

if(DEFINED ENV{PICO_SDK_FETCH_FROM_GIT_PATH} AND
   NOT PICO_SDK_FETCH_FROM_GIT_PATH)
    set(PICO_SDK_FETCH_FROM_GIT_PATH
        "$ENV{PICO_SDK_FETCH_FROM_GIT_PATH}")
endif()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH
    "Path to the Raspberry Pi Pico SDK")
set(PICO_SDK_FETCH_FROM_GIT "${PICO_SDK_FETCH_FROM_GIT}" CACHE BOOL
    "Fetch the Pico SDK from Git when PICO_SDK_PATH is unset")
set(PICO_SDK_FETCH_FROM_GIT_PATH "${PICO_SDK_FETCH_FROM_GIT_PATH}"
    CACHE PATH "Directory in which CMake downloads the Pico SDK")
set(PICO_SDK_FETCH_FROM_GIT_TAG "${PICO_SDK_FETCH_FROM_GIT_TAG}"
    CACHE STRING "Pico SDK Git tag or commit to fetch")

if(NOT PICO_SDK_PATH)
    if(NOT PICO_SDK_FETCH_FROM_GIT)
        message(FATAL_ERROR
            "PICO_SDK_PATH is unset and automatic Pico SDK download is disabled.")
    endif()

    include(FetchContent)
    set(_pico_saved_fetchcontent_base_dir "${FETCHCONTENT_BASE_DIR}")
    if(PICO_SDK_FETCH_FROM_GIT_PATH)
        get_filename_component(FETCHCONTENT_BASE_DIR
            "${PICO_SDK_FETCH_FROM_GIT_PATH}" REALPATH
            BASE_DIR "${CMAKE_SOURCE_DIR}")
    endif()

    message(STATUS
        "Downloading Raspberry Pi Pico SDK ${PICO_SDK_FETCH_FROM_GIT_TAG}")
    FetchContent_Populate(
        pico_sdk
        QUIET
        GIT_REPOSITORY https://github.com/raspberrypi/pico-sdk.git
        GIT_TAG "${PICO_SDK_FETCH_FROM_GIT_TAG}"
        GIT_SHALLOW TRUE
        GIT_SUBMODULES_RECURSE FALSE
        SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/pico_sdk-src"
        BINARY_DIR "${FETCHCONTENT_BASE_DIR}/pico_sdk-build"
        SUBBUILD_DIR "${FETCHCONTENT_BASE_DIR}/pico_sdk-subbuild"
    )
    set(PICO_SDK_PATH "${pico_sdk_SOURCE_DIR}")
    set(FETCHCONTENT_BASE_DIR "${_pico_saved_fetchcontent_base_dir}")
    unset(_pico_saved_fetchcontent_base_dir)
endif()

get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH
    BASE_DIR "${CMAKE_BINARY_DIR}")
if(NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR
        "${PICO_SDK_PATH} does not contain a Raspberry Pi Pico SDK checkout.")
endif()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH
    "Path to the Raspberry Pi Pico SDK" FORCE)
include("${PICO_SDK_PATH}/pico_sdk_init.cmake")
