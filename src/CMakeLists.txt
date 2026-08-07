add_executable(p2000m_vga_diagnostic
    main.c
)

target_link_libraries(p2000m_vga_diagnostic PRIVATE
    pico_stdlib
    pico_scanvideo_dpi
)

target_compile_definitions(p2000m_vga_diagnostic PRIVATE
    PICO_SCANVIDEO_COLOR_PIN_BASE=0
    PICO_SCANVIDEO_COLOR_PIN_COUNT=12
    PICO_SCANVIDEO_SYNC_PIN_BASE=12

    PICO_SCANVIDEO_DPI_PIXEL_RSHIFT=0
    PICO_SCANVIDEO_DPI_PIXEL_GSHIFT=4
    PICO_SCANVIDEO_DPI_PIXEL_BSHIFT=8
    PICO_SCANVIDEO_DPI_PIXEL_RCOUNT=4
    PICO_SCANVIDEO_DPI_PIXEL_GCOUNT=4
    PICO_SCANVIDEO_DPI_PIXEL_BCOUNT=4

    PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS=65
    PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT=4
    PICO_SCANVIDEO_RP2350_OVERLAY_SDK_SPINLOCKS=1
)

set_source_files_properties(main.c PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic"
)

pico_enable_stdio_uart(p2000m_vga_diagnostic 0)
pico_enable_stdio_usb(p2000m_vga_diagnostic 0)
pico_add_extra_outputs(p2000m_vga_diagnostic)

add_executable(p2000m_capture_diagnostic
    capture_diagnostic.c
    p2000m_capture.c
)

pico_generate_pio_header(p2000m_capture_diagnostic
    ${CMAKE_CURRENT_LIST_DIR}/p2000m_capture.pio
)

target_link_libraries(p2000m_capture_diagnostic PRIVATE
    pico_stdlib
    hardware_dma
    hardware_pio
)

pico_enable_stdio_uart(p2000m_capture_diagnostic 0)
pico_enable_stdio_usb(p2000m_capture_diagnostic 1)
pico_add_extra_outputs(p2000m_capture_diagnostic)

add_executable(p2000m_live_converter
    live_converter.c
    p2000m_capture.c
)

pico_generate_pio_header(p2000m_live_converter
    ${CMAKE_CURRENT_LIST_DIR}/p2000m_capture.pio
)

target_link_libraries(p2000m_live_converter PRIVATE
    pico_stdlib
    pico_multicore
    pico_scanvideo_dpi
    hardware_dma
    hardware_pio
)

target_compile_definitions(p2000m_live_converter PRIVATE
    PICO_SCANVIDEO_COLOR_PIN_BASE=0
    PICO_SCANVIDEO_COLOR_PIN_COUNT=12
    PICO_SCANVIDEO_SYNC_PIN_BASE=12

    PICO_SCANVIDEO_DPI_PIXEL_RSHIFT=0
    PICO_SCANVIDEO_DPI_PIXEL_GSHIFT=4
    PICO_SCANVIDEO_DPI_PIXEL_BSHIFT=8
    PICO_SCANVIDEO_DPI_PIXEL_RCOUNT=4
    PICO_SCANVIDEO_DPI_PIXEL_GCOUNT=4
    PICO_SCANVIDEO_DPI_PIXEL_BCOUNT=4

    PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS=323
    PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT=4
    PICO_SCANVIDEO_RP2350_OVERLAY_SDK_SPINLOCKS=1
)

set_source_files_properties(
    capture_diagnostic.c
    p2000m_capture.c
    live_converter.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic"
)

pico_enable_stdio_uart(p2000m_live_converter 0)
pico_enable_stdio_usb(p2000m_live_converter 1)
pico_add_extra_outputs(p2000m_live_converter)
