# Changelog

All notable changes to the P2000M VID2VGA firmware are documented here. The
format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and releases use [Semantic Versioning](https://semver.org/).

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

[0.1.0]: https://github.com/ifilot/p2000m-video-to-vga-adapter/releases/tag/v0.1.0
