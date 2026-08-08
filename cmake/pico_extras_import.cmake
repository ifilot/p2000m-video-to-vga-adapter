# Bundled pico-extras import helper, based on the sdk-2.3.0 import file.
#
# SPDX-FileCopyrightText: 2020 Raspberry Pi (Trading) Ltd.
# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: BSD-3-Clause
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

# Include this file before pico_sdk_init(). A caller-provided PICO_EXTRAS_PATH
# takes precedence; otherwise the matching pinned pico-extras tag is fetched.

if(DEFINED ENV{PICO_EXTRAS_PATH} AND NOT PICO_EXTRAS_PATH)
    set(PICO_EXTRAS_PATH "$ENV{PICO_EXTRAS_PATH}")
endif()

if(DEFINED ENV{PICO_EXTRAS_FETCH_FROM_GIT} AND
   NOT DEFINED PICO_EXTRAS_FETCH_FROM_GIT)
    set(PICO_EXTRAS_FETCH_FROM_GIT
        "$ENV{PICO_EXTRAS_FETCH_FROM_GIT}")
endif()

if(DEFINED ENV{PICO_EXTRAS_FETCH_FROM_GIT_PATH} AND
   NOT PICO_EXTRAS_FETCH_FROM_GIT_PATH)
    set(PICO_EXTRAS_FETCH_FROM_GIT_PATH
        "$ENV{PICO_EXTRAS_FETCH_FROM_GIT_PATH}")
endif()

set(PICO_EXTRAS_PATH "${PICO_EXTRAS_PATH}" CACHE PATH
    "Path to pico-extras")
set(PICO_EXTRAS_FETCH_FROM_GIT "${PICO_EXTRAS_FETCH_FROM_GIT}" CACHE BOOL
    "Fetch pico-extras from Git when PICO_EXTRAS_PATH is unset")
set(PICO_EXTRAS_FETCH_FROM_GIT_PATH "${PICO_EXTRAS_FETCH_FROM_GIT_PATH}"
    CACHE PATH "Directory in which CMake downloads pico-extras")
set(PICO_EXTRAS_FETCH_FROM_GIT_TAG "${PICO_EXTRAS_FETCH_FROM_GIT_TAG}"
    CACHE STRING "pico-extras Git tag or commit to fetch")

if(NOT PICO_EXTRAS_PATH)
    if(NOT PICO_EXTRAS_FETCH_FROM_GIT)
        message(FATAL_ERROR
            "PICO_EXTRAS_PATH is unset and automatic pico-extras download is disabled.")
    endif()

    include(FetchContent)
    set(_pico_saved_fetchcontent_base_dir "${FETCHCONTENT_BASE_DIR}")
    if(PICO_EXTRAS_FETCH_FROM_GIT_PATH)
        get_filename_component(FETCHCONTENT_BASE_DIR
            "${PICO_EXTRAS_FETCH_FROM_GIT_PATH}" REALPATH
            BASE_DIR "${CMAKE_SOURCE_DIR}")
    endif()

    message(STATUS
        "Downloading pico-extras ${PICO_EXTRAS_FETCH_FROM_GIT_TAG}")
    FetchContent_Populate(
        pico_extras
        QUIET
        GIT_REPOSITORY https://github.com/raspberrypi/pico-extras.git
        GIT_TAG "${PICO_EXTRAS_FETCH_FROM_GIT_TAG}"
        GIT_SHALLOW TRUE
        SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/pico_extras-src"
        BINARY_DIR "${FETCHCONTENT_BASE_DIR}/pico_extras-build"
        SUBBUILD_DIR "${FETCHCONTENT_BASE_DIR}/pico_extras-subbuild"
    )
    set(PICO_EXTRAS_PATH "${pico_extras_SOURCE_DIR}")
    set(FETCHCONTENT_BASE_DIR "${_pico_saved_fetchcontent_base_dir}")
    unset(_pico_saved_fetchcontent_base_dir)
endif()

get_filename_component(PICO_EXTRAS_PATH "${PICO_EXTRAS_PATH}" REALPATH
    BASE_DIR "${CMAKE_BINARY_DIR}")
if(NOT EXISTS "${PICO_EXTRAS_PATH}/CMakeLists.txt")
    message(FATAL_ERROR
        "${PICO_EXTRAS_PATH} does not contain a pico-extras checkout.")
endif()

set(PICO_EXTRAS_PATH "${PICO_EXTRAS_PATH}" CACHE PATH
    "Path to pico-extras" FORCE)
add_subdirectory("${PICO_EXTRAS_PATH}" pico_extras)
