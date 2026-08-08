#!/bin/sh
# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

# Validate a freshly built or restored FFmpeg runtime, including the exact
# recording path used by the viewer.

set -eu

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 RUNTIME_DIRECTORY WORK_DIRECTORY" >&2
    exit 2
fi

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=ffmpeg.env
. "$script_directory/ffmpeg.env"

runtime_directory=$1
work_directory=$2

if [ -x "$runtime_directory/ffmpeg.exe" ]; then
    ffmpeg_binary="$runtime_directory/ffmpeg.exe"
else
    ffmpeg_binary="$runtime_directory/ffmpeg"
fi

require_file() {
    required_file=$1
    if [ ! -s "$required_file" ]; then
        echo "The FFmpeg runtime is missing $required_file." >&2
        exit 1
    fi
}

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

verify_archive() {
    archive=$1
    expected=$2
    actual=$(sha256 "$archive")
    if [ "$actual" != "$expected" ]; then
        echo "SHA-256 mismatch for cached source archive $archive" >&2
        exit 1
    fi
}

ffmpeg_archive="$runtime_directory/source/ffmpeg-$FFMPEG_VERSION.tar.xz"
x264_archive="$runtime_directory/source/x264-$X264_REVISION.tar.bz2"
require_file "$ffmpeg_binary"
require_file "$runtime_directory/BUILD_INFO.txt"
require_file "$runtime_directory/licenses/FFMPEG-LICENSE.md"
require_file "$runtime_directory/licenses/FFMPEG-COPYING.GPLv3"
require_file "$runtime_directory/licenses/X264-COPYING"
require_file "$ffmpeg_archive"
require_file "$x264_archive"

verify_archive "$ffmpeg_archive" "$FFMPEG_SOURCE_SHA256"
verify_archive "$x264_archive" "$X264_SOURCE_SHA256"

grep -Fq "FFmpeg version: $FFMPEG_VERSION" "$runtime_directory/BUILD_INFO.txt"
grep -Fq "x264 revision: $X264_REVISION" "$runtime_directory/BUILD_INFO.txt"

mkdir -p "$work_directory"
frame="$work_directory/smoke-frame.bgra"
recording="$work_directory/smoke-recording.mp4"
dd if=/dev/zero of="$frame" bs=1228800 count=1 2>/dev/null
"$ffmpeg_binary" -hide_banner -loglevel error -nostdin -y \
    -f rawvideo -pixel_format bgra -video_size 640x480 -framerate 25.047 \
    -i "$frame" -frames:v 1 -an -c:v libx264 -preset veryfast -crf 18 \
    -pix_fmt yuv420p -movflags +faststart "$recording"
test -s "$recording"

echo "Verified FFmpeg $FFMPEG_VERSION with x264 $X264_REVISION."
