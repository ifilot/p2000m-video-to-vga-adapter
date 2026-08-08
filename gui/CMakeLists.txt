# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)

project(p2000m_vid2vga_viewer VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

qt_add_executable(p2000m-vid2vga-viewer WIN32
    main.cpp
)

target_compile_definitions(p2000m-vid2vga-viewer PRIVATE
    NOMINMAX
    P2000M_VIEWER_VERSION="${PROJECT_VERSION}"
    WIN32_LEAN_AND_MEAN
)

target_compile_options(p2000m-vid2vga-viewer PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(p2000m-vid2vga-viewer PRIVATE
    Qt6::Widgets
    setupapi
)

install(TARGETS p2000m-vid2vga-viewer
    RUNTIME DESTINATION .
)
