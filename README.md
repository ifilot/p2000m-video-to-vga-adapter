<!--
SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
SPDX-License-Identifier: CC-BY-4.0
-->

# P2000M VID2VGA adapter

[![Firmware](https://github.com/ifilot/p2000m-video-to-vga-adapter/actions/workflows/firmware.yml/badge.svg)](https://github.com/ifilot/p2000m-video-to-vga-adapter/actions/workflows/firmware.yml)

This repository contains the adapter PCB and Raspberry Pi Pico 2 firmware for
converting the Philips P2000M raw monochrome video output to VGA.

The adapter captures the conditioned P2000M signals on GPIO16-18,
recovers the source dot grid in software, and presents a tear-free 640 x 288
source image in a 640 x 480, 60 Hz VGA raster. The asynchronous 50.095 Hz
source is repeated as needed at VGA frame boundaries.

By default, the 288 source lines are displayed one-to-one between 96-line top
and bottom margins. An optional fit mode expands them to all 480 VGA lines using
symmetric nearest-neighbour 5:3 scaling. Each three-line source group becomes
five output lines in a 2,1,2 repetition pattern, preserving hard text edges
without introducing gray interpolation pixels.

## Hardware

The adapter conditions the P2000M video and synchronization signals for the
Pico 2, then converts the Pico's 12-bit RGB output and synchronization signals
to a standard VGA connection.

[![3D rendering of the assembled P2000M VID2VGA adapter PCB](images/p2000m-to-vga-adapter.png)](images/p2000m-to-vga-adapter.png)

*3D rendering of the assembled adapter. The board design is in
[`pcb/p2000m-to-vga-adapter.kicad_pcb`](pcb/p2000m-to-vga-adapter.kicad_pcb).*

[![Circuit schematic for the P2000M VID2VGA adapter](images/p2000m-to-vga-adapter-schematic.png)](pcb/p2000m-to-vga-adapter.pdf)

*Circuit schematic. Select the image to open the PDF; the editable source is
[`pcb/p2000m-to-vga-adapter.kicad_sch`](pcb/p2000m-to-vga-adapter.kicad_sch).*

## Building

The build requires Git, CMake, Ninja, and an Arm GNU embedded toolchain. The
first configuration automatically downloads the pinned Pico SDK 2.3.0 and
matching `pico-extras` release into the build directory; later configurations
reuse those checkouts.

On Debian or Ubuntu, install the host tools and cross-compiler with:

```sh
sudo apt update
sudo apt install git cmake ninja-build build-essential \
  gcc-arm-none-eabi libnewlib-arm-none-eabi \
  libstdc++-arm-none-eabi-newlib
```

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Pico 2 and its `rp2350-arm-s` platform are project defaults, so
`-DPICO_BOARD=pico2` is no longer necessary.

For an offline or custom SDK installation, explicitly pass
`-DPICO_SDK_PATH=/path/to/pico-sdk` and
`-DPICO_EXTRAS_PATH=/path/to/pico-extras`; these overrides take precedence over
automatic downloading.

Flash the single generated image:

```text
build/src/p2000m-vid2vga-firmware.uf2
```

## Capture and resampling

The P2000M output is not a stream of ready-made VGA pixels. It provides one
monochrome video signal together with horizontal and vertical synchronization
signals. The Pico measures the video signal several times for every original
picture dot, then uses the synchronization signals to reconstruct a 640 x 288
pixel image.

The P2000M and the VGA display do not run at the same rate. The source produces
about 50 frames per second, while VGA needs 60. The firmware therefore works
with complete frames: it displays the newest complete source frame when one is
available and otherwise repeats the previous one. It never changes source
frames partway down the screen, avoiding *tearing*—an image made from parts of
two different source frames.

For each source line, the firmware starts measuring from its horizontal
synchronization pulse. It also tracks the P2000M's actual timing and
automatically adjusts where it reads each of the 640 dots. Three closely spaced
measurements are checked for every reconstructed pixel; if any of them contains
foreground, that pixel is treated as foreground. This helps preserve narrow
parts of characters when the two devices' clocks do not line up exactly.

### Implementation details

- The input-conditioning Schmitt triggers invert the signals, so a low captured
  VIDEO level represents foreground and a high level represents background.
- PIO1 samples VIDEO at 63 MHz. Every source line is aligned independently to
  HSYNC, preventing small timing errors from accumulating across the frame.
- Each DMA word holds 28 video samples taken over 30 PIO clock ticks. The
  resampler maps the two branch ticks without samples to the nearest real
  samples.
- The horizontal dot period is calculated from the measured source-frame
  period. Automatic tuning tests five nearby sampling phases and periodically
  selects the best one.
- A double-buffered pixel map applies timing updates between decoded frames, so
  one frame always uses one consistent set of sampling positions.
- Three raw-frame buffers let DMA continue capturing while core 0 processes the
  newest complete frame. Three more buffers hold the reconstructed one-bit
  frames while core 1 generates VGA output.
- Core 1 accepts a reconstructed frame only at the beginning of a VGA frame.
  All 640 source pixels are output, followed by a black level that resets the
  RGB output before horizontal synchronization.

## USB controls

Connect a terminal to the Pico USB CDC serial port. Commands are
case-insensitive and execute when Enter is pressed. Command mode echoes input,
supports Backspace/Delete, displays a `vid2vga>` prompt, and produces no
unsolicited statistics.

- `status` or `s`: print capture, decoder, resampler, and VGA statistics.
- `version` or `v`: print the semantic firmware version.
- `license`: print the firmware copyright, license, warranty, and source
  location.
- `log`: stream those statistics every two seconds. Press Enter, Escape, or
  `q` to stop the stream and return to the command prompt.
- `settings`: print all active settings and whether they are factory defaults,
  modified in RAM, or saved in flash.
- `border on`, `border off`, or `border toggle`: control a one-pixel rectangle
  around the 640 x 288 source image. It uses the foreground color.
- `scale fit`: expand 288 source lines to the full 480-line VGA height.
- `scale native`: show the original 288 lines between 96-line margins.
- `fg <color>`: set the foreground/text color.
- `bg <color>`: set the background color, including the top and bottom margins.
- `colors`: list the named presets.
- `defaults`: restore white on black, border off, and native 1:1 scaling.
- `save`: explicitly save the current colors, border, scaling mode, and manual
  phase trim. These settings are restored after reset or power cycling.
- `factory-reset`: erase the saved configuration and immediately restore the
  factory display style and zero manual phase trim.
- `tune` or `j`: run automatic phase tuning and print all candidate scores.
- `phase +`, `phase -`, or `phase auto`: adjust or clear the manual phase trim.
- `geometry` or `g`: print coverage totals for all 80 x 24 character cells.
- `help`, `h`, or `?`: print the command reference.

Colors may be selected by name (`black`, `white`, `green`, `amber`, `cyan`,
`magenta`, `red`, `blue`, `yellow`, or `gray`) or entered as `RRGGBB`,
`#RRGGBB`, or `0xRRGGBB`. The VGA output hardware is RGB444, so the upper four
bits of each entered eight-bit channel determine the physical output level.

Settings are only written when `save` is entered, avoiding unnecessary flash
wear while experimenting. Two versioned, checksummed flash records are used in
alternation, so an interrupted save leaves the previous valid configuration
available. Saving or erasing briefly pauses both cores and may cause a momentary
VGA blink. Automatic phase selection is always recalculated from the live
signal; only the user's manual phase trim is persistent.

Automatic tuning runs about once every five seconds. The `stale_replaced`
counter normally increases when newer raw frames supersede frames that no
consumer needed; it is not itself a capture failure.

## Licensing

This is a multi-license project:

- The firmware, build configuration, CI workflow, and P2000M screen-test
  program are licensed under the
  [GNU GPL version 3 or later](LICENSES/GPL-3.0-or-later.txt).
- The PCB, schematic, custom footprints, and generated hardware views are
  licensed under the
  [CERN Open Hardware Licence Version 2—Strongly Reciprocal](LICENSES/CERN-OHL-S-2.0.txt).
- This README and the changelog are licensed under
  [Creative Commons Attribution 4.0](LICENSES/CC-BY-4.0.txt).

See [LICENSE.md](LICENSE.md) for the exact file scopes and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for components incorporated
from the Raspberry Pi Pico SDK, `pico-extras`, and TinyUSB.
