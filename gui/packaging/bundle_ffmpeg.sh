#!/bin/sh
# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

# Copy a verified runtime produced by build_ffmpeg.sh into an already installed
# viewer tree. The paths mirror the lookup order in main.cpp.

set -eu

if [ "$#" -ne 3 ]; then
    echo "Usage: $0 FFMPEG_RUNTIME PACKAGE_ROOT windows|linux|macos" >&2
    exit 2
fi

runtime_directory=$1
package_root=$2
platform=$3

case "$platform" in
    windows)
        binary_source="$runtime_directory/ffmpeg.exe"
        binary_directory="$package_root/tools"
        license_directory="$package_root/licenses/ffmpeg"
        ;;
    linux)
        binary_source="$runtime_directory/ffmpeg"
        binary_directory="$package_root/usr/libexec/p2000m-vid2vga-viewer"
        license_directory="$package_root/usr/licenses/ffmpeg"
        ;;
    macos)
        binary_source="$runtime_directory/ffmpeg"
        resources="$package_root/P2000M VID2VGA Viewer.app/Contents/Resources"
        binary_directory="$resources/tools"
        license_directory="$resources/licenses/ffmpeg"
        ;;
    *)
        echo "Unsupported package platform: $platform" >&2
        exit 2
        ;;
esac

if [ ! -x "$binary_source" ] || \
   [ ! -s "$runtime_directory/BUILD_INFO.txt" ]; then
    echo "The verified FFmpeg runtime is incomplete." >&2
    exit 1
fi

mkdir -p "$binary_directory" "$license_directory"
cp "$binary_source" "$binary_directory/"
chmod +x "$binary_directory/$(basename "$binary_source")"
cp "$runtime_directory/BUILD_INFO.txt" "$license_directory/"
cp -R "$runtime_directory/licenses/." "$license_directory/"
mkdir -p "$license_directory/source"
cp "$runtime_directory/source/"* "$license_directory/source/"

test -x "$binary_directory/$(basename "$binary_source")"
test -s "$license_directory/BUILD_INFO.txt"
