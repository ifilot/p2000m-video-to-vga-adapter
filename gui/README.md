<!--
SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
SPDX-License-Identifier: CC-BY-4.0
-->

# P2000M VID2VGA Viewer

The Qt 6 viewer automatically enumerates Raspberry Pi Pico USB CDC ports on
Windows, verifies the VID2VGA firmware, enters binary screen mode, and displays
complete CRC-checked frames. Disconnecting or closing the application returns
the adapter to its normal console mode. The application version is visible in
the window title, **Help > About**, Qt application metadata, and the Windows
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
widths, while **F11** enters a clean full-screen presentation and **Escape**
leaves it. The monitor-derived application icon is used by the executable,
main window, dialogs, and taskbar.

The application deliberately uses the Windows communications API rather than
Qt Serial Port. This keeps the only Qt dependency at `qt6-base`, which is part
of a standard MSYS2 UCRT64/MinGW Qt installation.

Build from an MSYS2 UCRT64 shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-qt6-base \
  mingw-w64-ucrt-x86_64-ffmpeg
cmake -S gui -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-gui
```

FFmpeg is optional and is not bundled with the viewer. **File > Start
Recording** looks for `ffmpeg.exe` beside the viewer, on `PATH`, and in the
standard MSYS2 UCRT64 and MinGW64 locations. The MSYS2 package above installs
an FFmpeg build with the `libx264` encoder used by the viewer. In an MSYS2
MinGW64 shell, install `mingw-w64-x86_64-ffmpeg` instead. Screenshots and
normal live viewing do not require FFmpeg.

To create a directory containing the executable and its Qt runtime DLLs:

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
