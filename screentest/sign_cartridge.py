#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
# SPDX-License-Identifier: GPL-3.0-or-later

"""Fill the P2000 cartridge byte-count and checksum header fields."""

from __future__ import annotations

import argparse
from pathlib import Path


ROM_SIZE = 0x4000
BANK_SIZE = 0x2000
HEADER_SIZE = 5
SIGNATURE_MASK = 0xF5
SIGNATURE_VALUE = 0x54


def sign_cartridge(image: bytes) -> tuple[bytes, int, int]:
    """Return a signed 16 KiB image, the covered byte count, and checksum."""
    if len(image) != ROM_SIZE:
        raise ValueError(
            f"expected a {ROM_SIZE}-byte cartridge image, got {len(image)} bytes"
        )

    signed = bytearray(image)
    signature = signed[0]
    if signature & SIGNATURE_MASK != SIGNATURE_VALUE or signature & 0x01:
        raise ValueError(f"invalid P2000 cartridge signature 0x{signature:02x}")
    if not signature & 0x08:
        raise ValueError("signature requests a second-bank checksum header")

    byte_count = BANK_SIZE - HEADER_SIZE
    signed[1:3] = byte_count.to_bytes(2, "little")
    signed[3:5] = b"\x00\x00"

    checksum = (-sum(signed[HEADER_SIZE:BANK_SIZE])) & 0xFFFF
    signed[3:5] = checksum.to_bytes(2, "little")

    validation_sum = checksum + sum(signed[HEADER_SIZE:BANK_SIZE])
    if validation_sum & 0xFFFF:
        raise AssertionError("internal checksum calculation error")

    return bytes(signed), byte_count, checksum


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Sign the first 8 KiB bank of a P2000 cartridge image."
    )
    parser.add_argument("input", type=Path, help="unsigned 16 KiB ROM image")
    parser.add_argument("output", type=Path, help="signed 16 KiB ROM image")
    args = parser.parse_args()

    signed, byte_count, checksum = sign_cartridge(args.input.read_bytes())
    args.output.write_bytes(signed)
    print(
        f"Signed {args.output}: count=0x{byte_count:04x}, "
        f"checksum=0x{checksum:04x}"
    )


if __name__ == "__main__":
    main()
