#!/usr/bin/env python3
"""Validate the address contract shared by the Bootloader, app and XBIN."""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path


BOOT_BASE = 0x08000000
BOOT_LIMIT = 0x0800C000
APP_BASE = 0x08010000
APP_LIMIT = 0x08080000
SRAM_BASE = 0x20000000
SRAM_LIMIT = 0x20020000
XBIN_HEADER_SIZE = 4096
XBIN_MAGIC = 0x4D414749


def validate_vector(image: bytes, base: int, limit: int, name: str) -> None:
    """Check size, initial MSP and Thumb reset vector of one raw image."""
    if len(image) < 8:
        raise ValueError(f"{name}: image is shorter than the vector table")
    if base + len(image) > limit:
        raise ValueError(f"{name}: image ends at 0x{base + len(image):08X}, past 0x{limit:08X}")

    initial_sp, reset_handler = struct.unpack_from("<II", image)
    reset_address = reset_handler & ~1
    if not SRAM_BASE <= initial_sp <= SRAM_LIMIT or initial_sp % 8:
        raise ValueError(f"{name}: invalid initial MSP 0x{initial_sp:08X}")
    if not reset_handler & 1 or not base <= reset_address < base + len(image):
        raise ValueError(f"{name}: invalid reset vector 0x{reset_handler:08X}")

    print(
        f"{name}: {len(image)} bytes, MSP=0x{initial_sp:08X}, "
        f"Reset=0x{reset_handler:08X}, end=0x{base + len(image):08X}"
    )


def validate_xbin(xbin: bytes, app: bytes) -> None:
    """Validate header layout, header CRC and application CRC of an XBIN."""
    if len(xbin) < XBIN_HEADER_SIZE:
        raise ValueError("XBIN: file is shorter than its 4096-byte header")

    magic, data_type, data_offset, data_address, data_length, data_crc = struct.unpack_from(
        "<I28xIIIII", xbin, 0
    )
    this_address, header_crc = struct.unpack_from("<II", xbin, 248)
    version = xbin[96:224].split(b"\0", 1)[0].decode("ascii")

    if magic != XBIN_MAGIC:
        raise ValueError(f"XBIN: bad magic 0x{magic:08X}")
    if data_type != 1 or data_offset != XBIN_HEADER_SIZE or data_address != APP_BASE:
        raise ValueError("XBIN: type, data offset or application address violates the contract")
    if this_address != BOOT_LIMIT:
        raise ValueError(f"XBIN: header address is 0x{this_address:08X}, expected 0x{BOOT_LIMIT:08X}")
    if data_length != len(app) or len(xbin) != data_offset + data_length:
        raise ValueError("XBIN: data length does not match the application binary")
    if xbin[data_offset:] != app:
        raise ValueError("XBIN: payload differs from the application binary")
    if zlib.crc32(xbin[:252]) & 0xFFFFFFFF != header_crc:
        raise ValueError("XBIN: header CRC32 mismatch")
    if zlib.crc32(app) & 0xFFFFFFFF != data_crc:
        raise ValueError("XBIN: application CRC32 mismatch")

    print(
        f"XBIN: version={version}, payload={data_length} bytes, "
        f"data CRC=0x{data_crc:08X}, header CRC=0x{header_crc:08X}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bootloader", type=Path)
    parser.add_argument("application", type=Path)
    parser.add_argument("xbin", type=Path)
    args = parser.parse_args()

    try:
        boot = args.bootloader.read_bytes()
        app = args.application.read_bytes()
        xbin = args.xbin.read_bytes()
        validate_vector(boot, BOOT_BASE, BOOT_LIMIT, "Bootloader")
        validate_vector(app, APP_BASE, APP_LIMIT, "WeatherClock")
        validate_xbin(xbin, app)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"Validation failed: {error}", file=sys.stderr)
        return 1

    print("All image checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
