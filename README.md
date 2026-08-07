# P2000M VID2VGA adapter

This repository contains the adapter PCB and Raspberry Pi Pico 2 firmware for
converting the Philips P2000M raw monochrome video output to VGA.

Firmware version **v0.1.0** captures the conditioned P2000M signals on GPIO16-18,
recovers the source dot grid in software, and presents a tear-free 640 x 288
source image in a 640 x 480, 60 Hz VGA raster. The asynchronous 50.095 Hz
source is repeated as needed at VGA frame boundaries.

By default, the 288 source lines are displayed one-to-one between 96-line top
and bottom margins. An optional fit mode expands them to all 480 VGA lines using
symmetric nearest-neighbour 5:3 scaling. Each three-line source group becomes
five output lines in a 2,1,2 repetition pattern, preserving hard text edges
without introducing gray interpolation pixels.

## Building

The build requires Raspberry Pi Pico SDK 2.x, `pico-extras`, CMake, Ninja, and
an Arm GNU embedded toolchain. Set `PICO_SDK_PATH` and `PICO_EXTRAS_PATH` to
their respective checkouts, then configure for Pico 2:

```sh
cmake -S . -B build -G Ninja \
  -DPICO_BOARD=pico2 \
  -DPICO_SDK_PATH=/path/to/pico-sdk \
  -DPICO_EXTRAS_PATH=/path/to/pico-extras
cmake --build build
```

Flash the single generated image:

```text
build/src/p2000m-vid2vga-firmware.uf2
```

## Capture and resampling

The PCB's Schmitt trigger inverts the source, so a cleared captured bit means
foreground and a set bit means background. PIO1 samples VIDEO uniformly at
63 MHz and anchors each line independently to HSYNC. Each packed word contains
28 useful samples and represents 30 PIO ticks; the software model explicitly
accounts for the two loop-branch gaps.

Three raw buffers decouple continuous DMA capture from software. The resampler
derives the horizontal dot period from the measured frame period, searches five
candidate phases, and decodes each output pixel from an early/centre/late
three-sample window. A double-buffered map allows automatic tuning updates
without mixing mappings inside a decoded frame.

Core 0 converts the newest raw frame into one-bit pixels. Three decoded buffers
ensure that one complete pending frame remains available while another is being
displayed and the third is filled. Core 1 changes decoded-frame ownership only
at the start of a VGA frame, preventing tearing and handoff starvation. All 640
source pixels are output; an additional black pixel in the front porch returns
the RGB DAC to black before synchronization.

## USB controls

Connect a terminal to the Pico USB CDC serial port. Commands are
case-insensitive and execute when Enter is pressed. Command mode echoes input,
supports Backspace/Delete, displays a `vid2vga>` prompt, and produces no
unsolicited statistics.

- `status` or `s`: print capture, decoder, resampler, and VGA statistics.
- `version` or `v`: print the semantic firmware version.
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
