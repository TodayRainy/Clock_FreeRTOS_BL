#!/usr/bin/env python3
"""Negative tests for the signed XBIN v2 trust boundary."""

from __future__ import annotations

import argparse
import hashlib
import struct
import zlib
from pathlib import Path

from validate_images import validate_xbin


DATA_OFFSET = 4096


def update_header_crc(image: bytearray) -> None:
    """Recalculate the non-security header CRC after an intentional mutation."""
    struct.pack_into("<I", image, 252, zlib.crc32(image[:252]) & 0xFFFFFFFF)


def expect_rejected(name: str, image: bytearray, openssl: Path,
                    public_key: Path) -> None:
    """Require one intentionally forged image to fail normal validation."""
    try:
        validate_xbin(bytes(image), bytes(image[DATA_OFFSET:]), openssl, public_key)
    except ValueError as error:
        print(f"PASS {name}: {error}")
        return
    raise AssertionError(f"{name}: forged image was accepted")


def validate_legacy_migration_layout(image: bytearray) -> None:
    """Confirm that the previous v2.1 Bootloader can install this package once.

    The old loader ignores the new fields at offsets 4..31, but still expects
    the data descriptor at offset 32 and both CRC32 values at their old places.
    This compatibility is intentionally one-way: the new loader rejects old
    unsigned packages after migration.
    """
    magic = struct.unpack_from("<I", image, 0)[0]
    data_type, data_offset, data_address, data_length, data_crc = struct.unpack_from(
        "<IIIII", image, 32
    )
    header_crc = struct.unpack_from("<I", image, 252)[0]
    payload = bytes(image[data_offset:])

    if (magic != 0x4D414749 or data_type != 1 or data_offset != DATA_OFFSET or
            data_address != 0x08010000 or data_length != len(payload) or
            (zlib.crc32(payload) & 0xFFFFFFFF) != data_crc or
            (zlib.crc32(image[:252]) & 0xFFFFFFFF) != header_crc or
            0 not in image[96:160]):
        raise AssertionError("XBIN v2 no longer satisfies the v2.1 migration layout")
    print("PASS legacy v2.1 one-time migration layout")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("xbin", type=Path)
    parser.add_argument("public_key", type=Path)
    parser.add_argument("--openssl", required=True, type=Path)
    parser.add_argument("--export-forged", type=Path,
                        help="write the recomputed-CRC/SHA forgery for device fault injection")
    args = parser.parse_args()

    original = bytearray(args.xbin.read_bytes())
    payload = bytes(original[DATA_OFFSET:])
    validate_xbin(bytes(original), payload, args.openssl, args.public_key)
    validate_legacy_migration_layout(original)

    corrupted = bytearray(original)
    corrupted[DATA_OFFSET + 100] ^= 0x01
    expect_rejected("payload corruption", corrupted, args.openssl, args.public_key)

    forged_payload = bytearray(corrupted)
    forged_data = bytes(forged_payload[DATA_OFFSET:])
    struct.pack_into("<I", forged_payload, 48,
                     zlib.crc32(forged_data) & 0xFFFFFFFF)
    forged_payload[52:84] = hashlib.sha256(forged_data).digest()
    update_header_crc(forged_payload)
    expect_rejected("recomputed CRC and SHA without private key", forged_payload,
                    args.openssl, args.public_key)
    if args.export_forged:
        args.export_forged.parent.mkdir(parents=True, exist_ok=True)
        args.export_forged.write_bytes(forged_payload)
        print(f"Exported unsigned forgery: {args.export_forged}")

    forged_metadata = bytearray(original)
    forged_metadata[96] ^= 0x01
    update_header_crc(forged_metadata)
    expect_rejected("metadata modification", forged_metadata,
                    args.openssl, args.public_key)

    forged_security_version = bytearray(original)
    current_security_version = struct.unpack_from("<I", forged_security_version, 24)[0]
    struct.pack_into("<I", forged_security_version, 24,
                     current_security_version ^ 1)
    update_header_crc(forged_security_version)
    expect_rejected("security version modification", forged_security_version,
                    args.openssl, args.public_key)

    forged_signature = bytearray(original)
    forged_signature[160] ^= 0x01
    update_header_crc(forged_signature)
    expect_rejected("signature modification", forged_signature,
                    args.openssl, args.public_key)

    print("All signed XBIN negative tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
