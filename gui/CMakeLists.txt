# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)

project(p2000m_vid2vga_viewer VERSION 0.4.0 LANGUAGES CXX)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/package-version.txt"
    "${PROJECT_VERSION}\n")

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/packaging/ffmpeg.env"
    P2000M_FFMPEG_VERSION_LINE REGEX "^FFMPEG_VERSION=")
string(REPLACE "FFMPEG_VERSION=" ""
    P2000M_FFMPEG_VERSION "${P2000M_FFMPEG_VERSION_LINE}")
if(NOT P2000M_FFMPEG_VERSION)
    message(FATAL_ERROR "Could not read the pinned FFmpeg version")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

set(P2000M_VIEWER_SOURCES
    main.cpp
    metric_graph_widget.cpp
    metric_graph_widget.h
    phosphor_afterglow.cpp
    phosphor_afterglow.h
    signal_loss_screen.cpp
    signal_loss_screen.h
    resources.qrc
)

if(WIN32)
    enable_language(RC)
    configure_file(
        version.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/version.rc
        @ONLY
    )
    list(APPEND P2000M_VIEWER_SOURCES
        ${CMAKE_CURRENT_BINARY_DIR}/version.rc
    )
else()
    list(APPEND P2000M_VIEWER_SOURCES
        adapter_serial_port.cpp
        adapter_serial_port.h
    )
endif()

qt_add_executable(p2000m-vid2vga-viewer
    ${P2000M_VIEWER_SOURCES}
)

target_include_directories(p2000m-vid2vga-viewer PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../src
)

target_compile_definitions(p2000m-vid2vga-viewer PRIVATE
    P2000M_VIEWER_VERSION="${PROJECT_VERSION}"
    P2000M_FFMPEG_VERSION="${P2000M_FFMPEG_VERSION}"
)

set_target_properties(p2000m-vid2vga-viewer PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "P2000M VID2VGA Viewer"
    MACOSX_BUNDLE_GUI_IDENTIFIER "nl.ivofilot.p2000m-vid2vga-viewer"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)

if(APPLE)
    set(P2000M_MACOS_ICON "" CACHE FILEPATH
        "Path to the generated P2000M viewer .icns file")
    if(NOT P2000M_MACOS_ICON OR NOT EXISTS "${P2000M_MACOS_ICON}")
        message(FATAL_ERROR
            "A macOS package requires P2000M_MACOS_ICON. Generate it with "
            "gui/packaging/create_macos_icon.sh before configuring.")
    endif()
    get_filename_component(P2000M_MACOS_ICON_NAME
        "${P2000M_MACOS_ICON}" NAME)
    target_sources(p2000m-vid2vga-viewer PRIVATE "${P2000M_MACOS_ICON}")
    set_source_files_properties("${P2000M_MACOS_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
    )
    set_target_properties(p2000m-vid2vga-viewer PROPERTIES
        OUTPUT_NAME "P2000M VID2VGA Viewer"
        MACOSX_BUNDLE_ICON_FILE "${P2000M_MACOS_ICON_NAME}"
    )
endif()

if(MSVC)
    set(P2000M_WARNING_OPTIONS /W4)
else()
    set(P2000M_WARNING_OPTIONS -Wall -Wextra -Wpedantic)
endif()

target_compile_options(p2000m-vid2vga-viewer PRIVATE
    ${P2000M_WARNING_OPTIONS}
)

target_link_libraries(p2000m-vid2vga-viewer PRIVATE
    Qt6::Widgets
)

if(WIN32)
    set(P2000M_VIEWER_RUNTIME_DESTINATION .)
    target_compile_definitions(p2000m-vid2vga-viewer PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(p2000m-vid2vga-viewer PRIVATE setupapi)
else()
    set(P2000M_VIEWER_RUNTIME_DESTINATION bin)
endif()

install(TARGETS p2000m-vid2vga-viewer
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${P2000M_VIEWER_RUNTIME_DESTINATION}
)

if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
    install(FILES packaging/nl.ivofilot.p2000m-vid2vga-viewer.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
    install(FILES assets/p2000m-vid2vga-viewer-icon.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
        RENAME nl.ivofilot.p2000m-vid2vga-viewer.png
    )
    install(FILES packaging/nl.ivofilot.p2000m-vid2vga-viewer.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
    )
endif()

set(P2000M_VIEWER_LICENSE_FILES
    ${CMAKE_CURRENT_LIST_DIR}/../LICENSE.md
    ${CMAKE_CURRENT_LIST_DIR}/../THIRD_PARTY_NOTICES.md
    ${CMAKE_CURRENT_LIST_DIR}/../LICENSES/GPL-3.0-or-later.txt
    ${CMAKE_CURRENT_LIST_DIR}/../LICENSES/LGPL-3.0-only.txt
)

if(APPLE)
    set(P2000M_VIEWER_LICENSE_DESTINATION
        "P2000M VID2VGA Viewer.app/Contents/Resources/licenses")
else()
    set(P2000M_VIEWER_LICENSE_DESTINATION licenses)
endif()
install(FILES ${P2000M_VIEWER_LICENSE_FILES}
    DESTINATION ${P2000M_VIEWER_LICENSE_DESTINATION})

if(EXISTS "${QT6_INSTALL_PREFIX}/LICENSES")
    install(DIRECTORY "${QT6_INSTALL_PREFIX}/LICENSES/"
        DESTINATION "${P2000M_VIEWER_LICENSE_DESTINATION}/qt")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(p2000m-packbits-test
        ../tests/packbits_test.cpp
    )
    target_include_directories(p2000m-packbits-test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../src
    )
    target_compile_options(p2000m-packbits-test PRIVATE
        ${P2000M_WARNING_OPTIONS}
    )
    add_test(NAME packbits-round-trip COMMAND p2000m-packbits-test)

    add_executable(p2000m-phosphor-noise-test
        ../tests/phosphor_noise_test.cpp
    )
    target_include_directories(p2000m-phosphor-noise-test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../src
    )
    target_compile_options(p2000m-phosphor-noise-test PRIVATE
        ${P2000M_WARNING_OPTIONS}
    )
    add_test(NAME phosphor-noise COMMAND p2000m-phosphor-noise-test)

    add_executable(p2000m-phosphor-afterglow-test
        ../tests/phosphor_afterglow_test.cpp
        phosphor_afterglow.cpp
    )
    target_include_directories(p2000m-phosphor-afterglow-test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
    )
    target_compile_options(p2000m-phosphor-afterglow-test PRIVATE
        ${P2000M_WARNING_OPTIONS}
    )
    target_link_libraries(p2000m-phosphor-afterglow-test PRIVATE
        Qt6::Gui
    )
    add_test(NAME phosphor-afterglow COMMAND p2000m-phosphor-afterglow-test)

    add_executable(p2000m-signal-loss-screen-test
        ../tests/signal_loss_screen_test.cpp
        signal_loss_screen.cpp
    )
    target_include_directories(p2000m-signal-loss-screen-test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
    )
    target_compile_options(p2000m-signal-loss-screen-test PRIVATE
        ${P2000M_WARNING_OPTIONS}
    )
    target_link_libraries(p2000m-signal-loss-screen-test PRIVATE
        Qt6::Gui
    )
    add_test(NAME signal-loss-screen COMMAND p2000m-signal-loss-screen-test)
endif()
