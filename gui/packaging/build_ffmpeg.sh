#!/bin/sh
# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

# Build the small, private FFmpeg runtime used by the viewer. Both source
# archives are preserved beside the result to satisfy source-redistribution
# obligations without relying on a future third-party download.

set -eu

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 OUTPUT_DIRECTORY WORK_DIRECTORY" >&2
    exit 2
fi

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=ffmpeg.env
. "$script_directory/ffmpeg.env"

output_directory=$1
work_directory=$2
source_directory="$work_directory/source"
build_directory="$work_directory/build"
x264_prefix="$work_directory/x264-prefix"
ffmpeg_prefix="$work_directory/ffmpeg-prefix"

mkdir -p "$output_directory" "$source_directory" "$build_directory" \
    "$x264_prefix" "$ffmpeg_prefix"

ffmpeg_archive="$source_directory/ffmpeg-$FFMPEG_VERSION.tar.xz"
x264_archive="$source_directory/x264-$X264_REVISION.tar.bz2"

download() {
    url=$1
    destination=$2
    if [ ! -s "$destination" ]; then
        temporary="$destination.part"
        rm -f "$temporary"
        if curl --fail --location --retry 5 --retry-all-errors \
            --retry-delay 2 --output "$temporary" "$url"; then
            mv "$temporary" "$destination"
        else
            rm -f "$temporary"
            return 1
        fi
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
        echo "SHA-256 mismatch for $archive" >&2
        echo "Expected: $expected" >&2
        echo "Actual:   $actual" >&2
        exit 1
    fi
}

download "$FFMPEG_SOURCE_URL" "$ffmpeg_archive"
download "$X264_SOURCE_URL" "$x264_archive"
verify_archive "$ffmpeg_archive" "$FFMPEG_SOURCE_SHA256"
verify_archive "$x264_archive" "$X264_SOURCE_SHA256"

tar -xf "$ffmpeg_archive" -C "$source_directory"
tar -xf "$x264_archive" -C "$source_directory"
ffmpeg_source="$source_directory/ffmpeg-$FFMPEG_VERSION"
x264_source="$source_directory/x264-$X264_REVISION"

if [ ! -x "$ffmpeg_source/configure" ] || \
   [ ! -x "$x264_source/configure" ]; then
    echo "A dependency source archive has an unexpected layout." >&2
    exit 1
fi

jobs=2
if command -v nproc >/dev/null 2>&1; then
    jobs=$(nproc)
elif command -v sysctl >/dev/null 2>&1; then
    jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 2)
elif [ -n "${NUMBER_OF_PROCESSORS:-}" ]; then
    jobs=$NUMBER_OF_PROCESSORS
fi

platform_cflags=
platform_ldflags=
case "$(uname -s)" in
    Darwin)
        platform_cflags="-mmacosx-version-min=12.0"
        platform_ldflags="-mmacosx-version-min=12.0"
        ;;
    MINGW*|MSYS*)
        # Keep the private encoder independent of MSYS2 runtime DLLs.
        platform_ldflags="-static"
        ;;
esac

x264_configuration="--prefix=$x264_prefix --enable-static --disable-cli --disable-opencl --bit-depth=8"
disable_x86asm=false
if ! command -v nasm >/dev/null 2>&1 && \
   [ "$(uname -m)" = "x86_64" ]; then
    x264_configuration="$x264_configuration --disable-asm"
    disable_x86asm=true
fi

mkdir -p "$build_directory/x264"
(
    cd "$build_directory/x264"
    CFLAGS="$platform_cflags"
    LDFLAGS="$platform_ldflags"
    export CFLAGS LDFLAGS
    # Word splitting is intentional: the configuration is also recorded
    # verbatim in BUILD_INFO.txt below.
    # shellcheck disable=SC2086
    "$x264_source/configure" $x264_configuration
    make -j"$jobs"
    make install
)

ffmpeg_configuration="--prefix=$ffmpeg_prefix --disable-shared --enable-static --disable-doc --disable-debug --disable-autodetect --disable-network --disable-ffplay --disable-ffprobe --disable-everything --enable-ffmpeg --enable-avcodec --enable-avformat --enable-avfilter --enable-swscale --enable-avutil --enable-protocol=file,pipe --enable-demuxer=rawvideo --enable-decoder=rawvideo --enable-encoder=libx264 --enable-muxer=mp4 --enable-filter=buffer,buffersink,format,scale --enable-libx264 --enable-gpl --enable-version3 --pkg-config-flags=--static --extra-cflags=-I$x264_prefix/include --extra-ldflags=-L$x264_prefix/lib"
if [ "$disable_x86asm" = true ]; then
    ffmpeg_configuration="$ffmpeg_configuration --disable-x86asm"
fi

mkdir -p "$build_directory/ffmpeg"
(
    cd "$build_directory/ffmpeg"
    PKG_CONFIG_PATH="$x264_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    CFLAGS="$platform_cflags"
    LDFLAGS="$platform_ldflags"
    export PKG_CONFIG_PATH CFLAGS LDFLAGS
    # shellcheck disable=SC2086
    "$ffmpeg_source/configure" $ffmpeg_configuration
    make -j"$jobs"
    make install
)

if [ -x "$ffmpeg_prefix/bin/ffmpeg.exe" ]; then
    ffmpeg_binary="$ffmpeg_prefix/bin/ffmpeg.exe"
    packaged_binary="$output_directory/ffmpeg.exe"
else
    ffmpeg_binary="$ffmpeg_prefix/bin/ffmpeg"
    packaged_binary="$output_directory/ffmpeg"
fi
cp "$ffmpeg_binary" "$packaged_binary"
chmod +x "$packaged_binary"

mkdir -p "$output_directory/licenses" "$output_directory/source"
cp "$ffmpeg_source/LICENSE.md" "$output_directory/licenses/FFMPEG-LICENSE.md"
cp "$ffmpeg_source/COPYING.GPLv3" \
    "$output_directory/licenses/FFMPEG-COPYING.GPLv3"
cp "$x264_source/COPYING" "$output_directory/licenses/X264-COPYING"
cp "$ffmpeg_archive" "$output_directory/source/"
cp "$x264_archive" "$output_directory/source/"

{
    echo "P2000M VID2VGA Viewer bundled recording runtime"
    echo
    echo "FFmpeg version: $FFMPEG_VERSION"
    echo "FFmpeg source: $FFMPEG_SOURCE_URL"
    echo "FFmpeg source SHA-256: $FFMPEG_SOURCE_SHA256"
    echo "x264 revision: $X264_REVISION"
    echo "x264 source: $X264_SOURCE_URL"
    echo "x264 source SHA-256: $X264_SOURCE_SHA256"
    echo
    echo "x264 configure arguments:"
    echo "$x264_configuration"
    echo "CFLAGS=$platform_cflags"
    echo "LDFLAGS=$platform_ldflags"
    echo
    echo "FFmpeg configure arguments:"
    echo "$ffmpeg_configuration"
    echo "CFLAGS=$platform_cflags"
    echo "LDFLAGS=$platform_ldflags"
    echo
    echo "Runtime identification:"
    "$packaged_binary" -version
} > "$output_directory/BUILD_INFO.txt"

# Exercise exactly the demuxer, decoder, filter, encoder, muxer, and protocols
# that the viewer uses. This also prevents an over-aggressive minimal build
# from producing a package that starts but cannot record.
frame="$work_directory/smoke-frame.bgra"
recording="$work_directory/smoke-recording.mp4"
dd if=/dev/zero of="$frame" bs=1228800 count=1 2>/dev/null
"$packaged_binary" -hide_banner -loglevel error -nostdin -y \
    -f rawvideo -pixel_format bgra -video_size 640x480 -framerate 25.047 \
    -i "$frame" -frames:v 1 -an -c:v libx264 -preset veryfast -crf 18 \
    -pix_fmt yuv420p -movflags +faststart "$recording"
test -s "$recording"

echo "Built and verified FFmpeg $FFMPEG_VERSION with x264 $X264_REVISION."
