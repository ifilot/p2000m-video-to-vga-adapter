# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

set(FIRMWARE_TARGET p2000m_vid2vga_firmware)

add_executable(${FIRMWARE_TARGET}
    firmware.c
    p2000m_capture.c
)

target_include_directories(${FIRMWARE_TARGET} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

set_target_properties(${FIRMWARE_TARGET} PROPERTIES
    OUTPUT_NAME "p2000m-vid2vga-firmware"
)

pico_generate_pio_header(${FIRMWARE_TARGET}
    ${CMAKE_CURRENT_LIST_DIR}/p2000m_capture.pio
)

target_link_libraries(${FIRMWARE_TARGET} PRIVATE
    pico_stdlib
    pico_flash
    pico_multicore
    pico_scanvideo_dpi
    tinyusb_device
    hardware_dma
    hardware_flash
    hardware_pio
)

target_compile_definitions(${FIRMWARE_TARGET} PRIVATE
    P2000M_VID2VGA_VERSION="${PROJECT_VERSION}"

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

    # The application initializes TinyUSB, while pico_stdio_usb's low-priority
    # IRQ worker services endpoint completions during long framebuffer decodes.
    PICO_STDIO_USB_ENABLE_TINYUSB_INIT=0
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=1
    PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=1
    CFG_TUD_CDC_RX_BUFSIZE=256
    CFG_TUD_CDC_TX_BUFSIZE=4096
)

set_source_files_properties(
    p2000m_capture.c
    firmware.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic"
)

pico_enable_stdio_uart(${FIRMWARE_TARGET} 0)
pico_enable_stdio_usb(${FIRMWARE_TARGET} 1)
pico_add_extra_outputs(${FIRMWARE_TARGET})
