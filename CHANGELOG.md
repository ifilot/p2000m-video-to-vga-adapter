<!--
SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
SPDX-License-Identifier: CC-BY-4.0
-->

# Changelog

All notable changes to the P2000M VID2VGA firmware are documented here. The
format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and releases use [Semantic Versioning](https://semver.org/).

## [0.3.0] - 2026-08-08

### Added

- A continuous, backpressure-safe binary USB screen mode which delivers
  complete CRC-checked,
  640 x 288 one-bit frames at up to half the P2000M source rate without taking
  ownership away from VGA scanout.
- A Qt 6 Windows viewer which discovers Pico CDC ports, verifies the VID2VGA
  firmware, enters screen mode automatically, reproduces VGA colors and
  geometry, and restores console mode when disconnected.
- Adapter configuration, connection, and About menus in the Windows viewer,
  including color pickers, border and scaling controls, phase trim, and
  optional persistent saving through the existing USB console.
- Smooth or sharp display filtering, pixel-perfect integer scaling, full-screen
  presentation, and lossless framebuffer screenshots in the Windows viewer.
- Lower-latency GUI processing to improve full-frame streaming throughput.
- Backward-compatible viewer negotiation for older per-frame-credit firmware.
- Independently recoverable PackBits screen records with automatic raw fallback,
  expanded-data CRC validation, codec round-trip tests, and live compression
  reporting in the viewer.
- An explicitly experimental 252 MHz, 1.30 V RP2350 configuration which retains
  exact 63 MHz capture and 25.2 MHz VGA clocks.
- Raw streaming as the viewer default, live raw/PackBits selection, aggregated
  PackBits USB writes, a faster table-driven framebuffer CRC, and end-to-end
  USB/render/paint/firmware timing telemetry.
- Visible viewer version information in the title, About dialog, Qt metadata,
  and Windows executable properties.
- A monitor-derived application icon for the Windows executable, taskbar,
  viewer window, and dialogs.
- A right-hand live performance panel with rolling history graphs for all
  transport, frame, rendering, firmware timing, and integrity metrics.
- FFmpeg-backed H.264 MP4 screen recording with bounded buffering, recording
  controls, progress reporting, and safe finalization on disconnect or exit.
- Mode-aware performance graphs which hide compression-only metrics in raw
  mode, plus rolling CRC errors per minute and a cumulative status-bar count.

### Changed

- Standardized source comments around Doxygen documentation blocks and concise
  implementation-rationale comments.
- Reserved the viewer status bar for the COM port, source frame number,
  cumulative CRC count, and operational recording messages instead of
  detailed performance statistics.
- Enabled Qt resource compilation so the application icon is available to
  windows and the taskbar, and displayed it prominently in the About dialog.
- Documented the cooling block used for sustained 252 MHz, 25 FPS operation and
  recommended equivalent RP2350 cooling for prolonged overclocked use.

## [0.2.0] - 2026-08-08

### Added

- A buildable P2000M screen-test cartridge with row and column rulers, a
  scrolling title, an elapsed-time display, and automatic cartridge-header
  signing and checksum generation.
- Four M3 mounting holes to the schematic and PCB design, together with updated
  board and schematic renders.
- Explicit per-file licensing, third-party notices, and SPDX metadata for the
  firmware, hardware, documentation, and bundled license texts.
- A `license` USB command and startup notice reporting the firmware's copyright,
  license, warranty, and source location.
- An independently colored border with selectable solid or dotted rendering,
  configurable and persistent through the USB console.

### Changed

- Tagged firmware releases now include the applicable license texts and
  third-party notices alongside the UF2 image.
- Expanded the documentation of the adapter hardware, P2000M source timing,
  signal conditioning, capture pipeline, pixel reconstruction, and VGA output.

### Fixed

- Corrected the DIN-5 connector mapping for the `VIDEO`, `HSYNC`, `VSYNC`, and
  ground signals in the schematic and PCB.
- Aligned capture to the trailing edge of `VSYNC` and skipped the two blank
  hardware rows, preventing a blank first character row from replacing the
  final visible row.

## [0.1.0] - 2026-08-07

### Added

- PIO and DMA capture of the active-low P2000M `VIDEO`, `HSYNC`, and `VSYNC`
  signals on Raspberry Pi Pico 2 GPIO16 through GPIO18.
- Software recovery of the source line and dot timing, with automatic sampling
  phase tuning, three-point pixel sampling, and a persistent manual phase trim.
- Tear-free 640 x 480 VGA output with the 640 x 288 source displayed in native
  one-to-one mode by default.
- Optional nearest-neighbour 5:3 vertical scaling to fill all 480 VGA lines.
- Triple buffering for both raw capture and decoded display frames, including
  asynchronous 50 Hz input to 60 Hz VGA frame repetition.
- Configurable foreground and background RGB colors, named color presets, and
  an optional foreground-colored screen-locking border.
- Interactive USB CDC console with input echo, line editing, quiet command
  mode, optional statistics logging, capture diagnostics, and firmware version
  reporting.
- Explicit flash persistence for colors, border, scaling mode, and manual
  sampling trim using redundant versioned records with CRC-32 validation.
- A single production firmware target and UF2 image named
  `p2000m-vid2vga-firmware`.
- GitHub Actions compilation checks and automatic tagged-release publishing of
  the Pico 2 UF2 binary.
- Automatic, version-pinned Pico SDK and `pico-extras` downloads during CMake
  configuration, with optional local-path overrides for offline builds.
- Pico 2/RP2350 project defaults and an early, actionable error when the Arm
  cross-compiler is not installed.

[0.3.0]: https://github.com/ifilot/p2000m-video-to-vga-adapter/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/ifilot/p2000m-video-to-vga-adapter/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/ifilot/p2000m-video-to-vga-adapter/releases/tag/v0.1.0
