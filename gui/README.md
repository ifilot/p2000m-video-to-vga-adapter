<!--
SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
SPDX-License-Identifier: CC-BY-4.0
-->

# P2000M VID2VGA Viewer

The Qt 6 viewer automatically enumerates Raspberry Pi Pico USB CDC ports on
Windows, Linux, and macOS, verifies the VID2VGA firmware, enters binary screen
mode, and displays complete CRC-checked frames. Disconnecting or closing the
application returns the adapter to its normal console mode. The application
version is visible in the window title, **Help > About**, Qt application metadata, and the Windows
executable properties.

The viewer uses continuous streaming with current firmware to avoid a USB
command round trip between frames. It automatically falls back to per-frame
credits when it recognizes an earlier version-one firmware announcement. Raw
streaming is the default. **Adapter > Stream Encoding** can switch a live
connection to experimental PackBits mode without reconnecting; compressed
records have their expanded CRC-32 validated and fall back to raw whenever
compression is ineffective.

The right-hand **Live performance** panel plots approximately 60 seconds of
rolling history for USB throughput, received and painted frame rates, source
sequence step, payload size, rendering and PackBits times, paint time,
firmware preparation and encoding times, and the rolling CRC-error rate. Raw
mode hides the payload-size, PackBits-unpacking, and firmware-encoding graphs
because those compression metrics do not apply. The status bar is reserved
for operational information: the active COM port, source frame number,
cumulative CRC errors for the current capture, and recording state.

The **Adapter > Configure Adapter** dialog controls foreground, background,
border color and style, vertical scaling, and sampling-phase trim. **Apply**
changes the running configuration; **Apply & Save** also stores it in the
adapter's redundant flash settings slots. The viewer briefly pauses screen
traffic, uses the firmware's normal USB console commands, and resumes the
binary stream on the same COM port.

The menu bar also provides connection controls, lossless framebuffer
screenshots, and MP4 recordings under **File**, plus application and license
information under **Help > About**. Recordings contain the reconstructed
640 x 480 monitor image and are encoded by a separate FFmpeg process, keeping
video compression away from the serial and painting paths. **View > Scaling
Filter** switches between smooth anti-aliased resizing and sharp
nearest-neighbor pixels. Pixel-perfect integer scaling avoids uneven pixel
widths. **View > CRT Phosphor Afterglow** optionally retains decaying light
from earlier frames; its brightness half-life is tunable from 10 to 1000 ms
and both settings persist between viewer sessions. The effect is limited to
presentation, so recordings and lossless framebuffer screenshots retain the
unfiltered adapter image. **F11** enters a clean full-screen presentation and
**Escape** leaves it. The monitor-derived application icon is used by the
executable, main window, dialogs, and taskbar.

The application uses SetupAPI and direct COM access on Windows, preserving the
high-throughput path tested with the adapter. Linux and macOS use a compact
native POSIX serial backend. Qt Serial Port is therefore not required on any
platform; the only Qt dependency is Qt 6 Base.

Build from an MSYS2 UCRT64 shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-qt6-base
cmake -S gui -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-gui
```

Every distributed viewer package includes a private, smoke-tested FFmpeg/x264
runtime for H.264 recording. The viewer selects that copy before considering a
developer installation on `PATH`. Exact corresponding source archives,
license texts, checksums, and configure arguments are included under
`licenses/ffmpeg`; no separate FFmpeg installation is required.

See [PACKAGING.md](PACKAGING.md) for Linux and macOS builds, package formats,
the default-branch package checks, and the exact-commit tag release process.

The following creates a development directory containing the executable and
its Qt runtime DLLs. It is not a distributable package until the pinned FFmpeg
runtime has also been built and bundled as described in [PACKAGING.md](PACKAGING.md):

```sh
mkdir -p build-gui/package
cp build-gui/p2000m-vid2vga-viewer.exe build-gui/package/
windeployqt6 --release --no-translations \
  build-gui/package/p2000m-vid2vga-viewer.exe
```

MSYS2's Qt packages use several shared UCRT64 libraries which
`windeployqt6` does not copy. Add their recursively discovered dependencies
when the package must run without an MSYS2 `PATH`:

```sh
for binary in $(find build-gui/package -type f \
    \( -name '*.exe' -o -name '*.dll' \)); do
  ldd "$binary"
done | awk '$3 ~ /^\/ucrt64\/bin\// { print $3 }' | sort -u | \
  xargs -r -I{} cp "{}" build-gui/package/
```
