<!--
SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
SPDX-License-Identifier: CC-BY-4.0
-->

# P2000M VID2VGA Viewer

The Qt 6 viewer automatically enumerates Raspberry Pi Pico USB CDC ports on
Windows, verifies the VID2VGA firmware, enters binary screen mode, and displays
complete CRC-checked frames. Disconnecting or closing the application returns
the adapter to its normal console mode.

The **Adapter > Configure Adapter** dialog controls foreground, background,
border color and style, vertical scaling, and sampling-phase trim. **Apply**
changes the running configuration; **Apply & Save** also stores it in the
adapter's redundant flash settings slots. The viewer briefly pauses screen
traffic, uses the firmware's normal USB console commands, and resumes the
binary stream on the same COM port.

The menu bar also provides connection controls and lossless framebuffer
screenshots under **File**, plus application and license information under
**Help > About**. **View > Scaling Filter** switches between smooth
anti-aliased resizing and sharp nearest-neighbor pixels. Pixel-perfect integer
scaling avoids uneven pixel widths, while **F11** enters a clean full-screen
presentation and **Escape** leaves it.

The application deliberately uses the Windows communications API rather than
Qt Serial Port. This keeps the only Qt dependency at `qt6-base`, which is part
of a standard MSYS2 UCRT64/MinGW Qt installation.

Build from an MSYS2 UCRT64 shell:

```sh
cmake -S gui -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-gui
```

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
