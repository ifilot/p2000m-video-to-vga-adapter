#!/bin/sh
# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 INPUT.png OUTPUT.icns" >&2
    exit 2
fi

input=$1
output=$2
work_directory=$(mktemp -d "${TMPDIR:-/tmp}/p2000m-icon.XXXXXX")
trap 'rm -rf "$work_directory"' EXIT HUP INT TERM
iconset="$work_directory/p2000m-viewer.iconset"
mkdir -p "$iconset"

for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$input" \
        --out "$iconset/icon_${size}x${size}.png" >/dev/null
    double_size=$((size * 2))
    sips -z "$double_size" "$double_size" "$input" \
        --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done

mkdir -p "$(dirname "$output")"
iconutil --convert icns --output "$output" "$iconset"
test -s "$output"
