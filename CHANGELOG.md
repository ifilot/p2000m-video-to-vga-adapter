<!--
SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
SPDX-License-Identifier: CC-BY-4.0
-->

# Changelog

All notable changes to the P2000M VID2VGA project are documented here. The
format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and releases use [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.5.0] - 2026-08-14

### Added

- Simultaneous VGA and PAL-compatible monochrome 625/50 composite output on
  GPIO14-15, using the RP2350's dedicated PIO2 block, true 2:1 interlace, a
  14 MHz sample clock, and four small scanline buffers arranged as two chained
  DMA transfers.
- Independent decoded-frame holds for VGA, PAL, and USB consumers, with PAL
  field-boundary frame selection and composite underrun statistics.
- Cooperative core-1 VGA/PAL scheduling, packed PAL pixel expansion, and an
  SRAM-resident PAL hot path for stable simultaneous output.
- Independent, persistable VGA and PAL output enable controls in the firmware
  console and viewer configuration dialog; USB capture remains available when
  both physical outputs are disabled.
- Backward-compatible viewer parsing for both legacy and output-aware firmware
  settings records.
- A monochrome `SIGNAL LOST` composite status card which retains valid PAL
  synchronization and disappears automatically when stable capture resumes.
- Application artwork based on the physical adapter PCB, including a 512 px
  desktop icon and a multi-resolution Windows executable icon.

### Changed

- Standardized on the Pico SDK default-descriptor TinyUSB linkage instead of
  simultaneously requesting the application-descriptor library variant.

## [0.4.0] - 2026-08-12

### Added

- Optional `low`, `medium`, and `high` phosphor-grain levels which dim a
  source-frame-synchronous random selection of foreground pixels by one RGB444
  DAC step without modifying the captured one-bit framebuffer.
- Persistent phosphor-grain configuration through the USB console and the
  viewer's adapter dialog, with automatic migration of older saved settings.
- Viewer reproduction of the firmware grain pattern using spare screen-record
  style bits, while retaining compatibility with earlier firmware.
- Viewer foreground presets for Matrix green, retro amber, and warm off-white,
  while retaining the unrestricted custom color picker.

## [0.3.2] - 2026-08-12

### Added

- A remembered save-dialog option to add a 12 px black border around
  screenshots and MP4 recordings.
- A permanent 12 px rounded bezel around the viewer's live monitor viewport.
- Viewer-side 100 ms source-signal loss detection with the same status card as
  the physical VGA output and automatic recovery when valid frames resume.
- A third screen-test cartridge view showing all 256 alphanumeric and graphics
  glyphs from the P2000M character ROM in a labelled 16 by 16 map.

### Fixed

- Replaced the Windows Qt IFW offline installer with an upgrade-aware NSIS
  package whose uninstaller remains usable and which accepts the existing
  installation directory when installing a newer release.

## [0.3.1] - 2026-08-08

### Added

- An optional viewer-side CRT phosphor afterglow effect with a persistent
  enable switch and a tunable 10–1000 ms brightness half-life.

### Fixed

- Corrected the Qt Installer Framework component identifier so the Windows
  installer can load its shortcut-creation script and proceed past Welcome.

### Changed

- Cached each platform's verified FFmpeg/x264 runtime in CI, while retaining
  source-archive checksum checks and a real MP4 encoding test on every run.

## [0.3.0] - 2026-08-08

### Added

- A synchronization watchdog that replaces a frozen source image with a
  dedicated `SIGNAL LOST` VGA screen when HSYNC/VSYNC-driven capture stops and
  restores live video after stable input timing returns; the screen identifies
  the adapter, compiled firmware version, and synchronization inputs awaited.
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
- Native Linux and macOS USB CDC backends while preserving the established
  Windows SetupAPI and direct-COM implementation.
- Automated Windows ZIP, Linux AppImage, and Intel/Apple-silicon macOS DMG
  builds on every default-branch push and pull request.
- A graphical Windows offline installer with Start-menu and desktop shortcuts,
  license presentation, and an uninstall/maintenance tool; the portable ZIP
  remains available alongside it.
- A pinned, package-private FFmpeg 8.1.2 and x264 H.264 recording runtime in
  every desktop package, including verified corresponding source archives,
  license texts, checksums, build configuration, and an encoding smoke test.
- Exact-commit release promotion which refuses tags unless both firmware and
  every desktop package were already tested successfully on `master`.

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
  scrolling title, an elapsed-time display, a Space-selectable Matrix-style
  character-rain mode, and automatic cartridge-header signing and checksum
  generation.
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

[Unreleased]: https://github.com/ifilot/p2000m-video-to-vga-adapter/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/ifilot/p2000m-video-to-vga-adapter/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/ifilot/p2000m-video-to-vga-adapter/compare/v0.3.2...v0.4.0
[0.3.2]: https://github.com/ifilot/p2000m-video-to-vga-adapter/compare/v0.3.1...v0.3.2
[0.3.1]: https://github.com/ifilot/p2000m-video-to-vga-adapter/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/ifilot/p2000m-video-to-vga-adapter/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/ifilot/p2000m-video-to-vga-adapter/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/ifilot/p2000m-video-to-vga-adapter/releases/tag/v0.1.0
