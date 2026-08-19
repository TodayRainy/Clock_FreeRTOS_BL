#!/usr/bin/env python3
"""Validate the address contract shared by the Bootloader, app and XBIN."""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

from firmware_crypto import public_key_id, public_point, verify_header


BOOT_BASE = 0x08000000
BOOT_LIMIT = 0x0800C000
APP_BASE = 0x08010000
APP_LIMIT = 0x08080000
SRAM_BASE = 0x20000000
SRAM_LIMIT = 0x20020000
XBIN_HEADER_SIZE = 4096
XBIN_MAGIC = 0x4D414749
XBIN_FORMAT_VERSION = 2
XBIN_STRUCT_SIZE = 256


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


def validate_xbin(xbin: bytes, app: bytes, openssl: Path, public_key: Path) -> None:
    """Validate XBIN v2 layout, both digests and its ECDSA-P256 signature."""
    if len(xbin) < XBIN_HEADER_SIZE:
        raise ValueError("XBIN: file is shorter than its 4096-byte header")

    (magic, format_version, header_size, hash_algorithm,
     signature_algorithm, key_id) = struct.unpack_from("<IIIIII", xbin, 0)
    security_version = struct.unpack_from("<I", xbin, 24)[0]
    data_type, data_offset, data_address, data_length, data_crc = struct.unpack_from(
        "<IIIII", xbin, 32
    )
    image_sha256 = xbin[52:84]
    this_address, header_crc = struct.unpack_from("<II", xbin, 248)
    version = xbin[96:160].split(b"\0", 1)[0].decode("ascii")
    signature = xbin[160:224]

    if magic != XBIN_MAGIC:
        raise ValueError(f"XBIN: bad magic 0x{magic:08X}")
    if (format_version != XBIN_FORMAT_VERSION or header_size != XBIN_STRUCT_SIZE or
            hash_algorithm != 1 or signature_algorithm != 1):
        raise ValueError("XBIN: unsupported signed image format or algorithm")
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
    if hashlib.sha256(app).digest() != image_sha256:
        raise ValueError("XBIN: application SHA-256 mismatch")

    point = public_point(openssl, public_key)
    if key_id != public_key_id(point):
        raise ValueError("XBIN: key ID does not match the configured public key")
    if not verify_header(openssl, public_key, xbin[:XBIN_STRUCT_SIZE], signature):
        raise ValueError("XBIN: ECDSA-P256 signature mismatch")

    print(
        f"XBIN: version={version}, security={security_version}, payload={data_length} bytes, "
        f"key=0x{key_id:08X}, data CRC=0x{data_crc:08X}, "
        f"SHA-256={image_sha256.hex().upper()}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bootloader", type=Path)
    parser.add_argument("application", type=Path)
    parser.add_argument("xbin", type=Path)
    parser.add_argument("public_key", type=Path)
    parser.add_argument("--openssl", required=True, type=Path)
    args = parser.parse_args()

    try:
        boot = args.bootloader.read_bytes()
        app = args.application.read_bytes()
        xbin = args.xbin.read_bytes()
        validate_vector(boot, BOOT_BASE, BOOT_LIMIT, "Bootloader")
        validate_vector(app, APP_BASE, APP_LIMIT, "WeatherClock")
        validate_xbin(xbin, app, args.openssl, args.public_key)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"Validation failed: {error}", file=sys.stderr)
        return 1

    print("All image checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
