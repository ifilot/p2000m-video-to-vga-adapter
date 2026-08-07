# P2000M video to VGA adapter

This repository contains the adapter PCB and Raspberry Pi Pico 2 firmware for
converting the Philips P2000M raw monochrome video output to VGA.

The firmware provides two separate hardware diagnostics plus a live bridge.
The VGA diagnostic generates a 640 x 480, 60 Hz test image containing
full-intensity color bars, separate 16-level red/green/blue ramps, a
checkerboard, and a pink geometry border. The capture diagnostic acquires the
P2000M input into memory and exposes timing, preview, and raw-frame data over
USB. The live bridge combines both paths into a tear-free VGA image.

## Building the diagnostic firmware

The build requires Raspberry Pi Pico SDK 2.x, `pico-extras`, CMake, and an Arm
GNU embedded toolchain. Set `PICO_SDK_PATH` and `PICO_EXTRAS_PATH` to their
respective checkouts, then configure specifically for Pico 2:

```sh
cmake -S . -B build -G Ninja \
  -DPICO_BOARD=pico2 \
  -DPICO_SDK_PATH=/path/to/pico-sdk \
  -DPICO_EXTRAS_PATH=/path/to/pico-extras
cmake --build build
```

Flash `build/src/p2000m_vga_diagnostic.uf2` to the Pico 2. With the adapter
connected to a VGA display, the test image should remain stable and fill a
640 x 480 raster. The three ramps exercise every bit of each 4-bit resistor
DAC; the checkerboard exposes pixel-clock or scanline instability. The pink
border sits inside a four-pixel black blanking guard and should be visible on
all four sides after the monitor's auto-adjustment.

## P2000M capture diagnostic

`build/src/p2000m_capture_diagnostic.uf2` captures the conditioned P2000M input
on GPIO16-18 without producing VGA. Connect to the Pico 2 USB CDC serial port
with a terminal. The firmware reports capture statistics every two seconds and
accepts these commands:

- `s`: print the latest frame period and capture counters.
- `p`: render an 80 x 36 text preview of the most recent frame.
- `d`: dump the packed oversampled 288-line capture as hexadecimal.
- `j`: run phase tuning immediately and print all candidate scores.
- `+`, `-`, `0`: adjust or clear the manual resampler trim.
- `h` or `?`: print command help.

The hex dump is ordered left-to-right, top-to-bottom, most-significant bit
first. Because the PCB's Schmitt trigger inverts the P2000M video input, a zero
bit represents a white source sample and a one bit represents black. Each
visible scanline is anchored independently to HSYNC and oversampled using a
uniform 63 MHz PIO clock. A line contains 114 words; every word holds 28 useful
samples in bits 27 through 0 and four padding bits. Two known PIO branch gaps
occur per word and are included in the resampling model.

Software derives the source dot period from the measured frame period and maps
all 640 output pixels onto that recovered grid. It searches five candidate
phase positions automatically and samples a three-tick window around the best
position. The `stale_replaced` statistic counts completed raw frames superseded
before a consumer used them; it normally increases and does not indicate a
capture failure. Pico SDK's TinyUSB submodule must be initialized when building
this USB-enabled target.

## Live capture-to-VGA bridge

`build/src/p2000m_live_converter.uf2` combines capture and VGA output. It shows
the latest complete 640 x 288 P2000M frame one-to-one in the center of a 640 x
480, 60 Hz VGA raster, with 96 black lines above and below. Core 0 converts the
latest oversampled source frame into one-bit pixels; core 1 prepares VGA
scanlines. Decoded-frame ownership changes only at the start of a VGA frame, so
the asynchronous 50.095 Hz input is repeated as needed without tearing.

Each active scanline outputs all 640 captured pixels followed by an explicit
black reset pixel in the VGA front porch. This prevents the final source color
from leaking into horizontal blanking without sacrificing the last column.

The USB CDC port reports live statistics every two seconds. `swaps` should
increase at approximately the input frame rate and `repeats` at approximately
the 10 Hz difference between VGA and the P2000M. Use `s` for an immediate report
or `h` for command help. Automatic tuning runs in the background about once per
second. The `j` command forces a tuning pass and reports the white-sample score
for every candidate phase. `+` and `-` add a manual trim of one 63 MHz tick
(15.87 ns, roughly 0.19 source dot); the supported range is -4 through +4. `0`
clears the manual trim without disabling automatic line-period and phase
recovery. The `g` command reports white-pixel counts for all 24 source character
rows and 80 source character columns, including explicit top, bottom, left, and
right boundary totals.
