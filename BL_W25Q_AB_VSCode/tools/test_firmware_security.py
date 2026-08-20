#!/usr/bin/env python3
"""Negative tests for the unified XBIN Formal trust boundary."""

from __future__ import annotations

import argparse
import hashlib
import struct
import zlib
from pathlib import Path

from validate_images import decrypt_xbin_payload, validate_xbin


DATA_OFFSET = 4096
HEADER_CRC_OFFSET = 316


def update_header_crc(image: bytearray) -> None:
    """Recalculate the non-security Header CRC after a deliberate mutation."""
    struct.pack_into(
        "<I", image, HEADER_CRC_OFFSET,
        zlib.crc32(image[:HEADER_CRC_OFFSET]) & 0xFFFFFFFF
    )


def expect_rejected(name: str, image: bytearray, openssl: Path,
                    public_key: Path, aes_tool: Path,
                    encryption_key: Path, application: bytes) -> None:
    """Require one intentionally forged image to fail normal validation."""
    try:
        validate_xbin(bytes(image), application, openssl, public_key,
                      aes_tool, encryption_key)
    except ValueError as error:
        print(f"PASS {name}: {error}")
        return
    raise AssertionError(f"{name}: forged image was accepted")


def validate_formal_layout(image: bytearray, application: bytes) -> None:
    """Confirm there is one 320-byte Formal Header and mandatory ciphertext."""
    (magic, revision, size, data_type, data_offset, data_address,
     data_length, data_crc) = struct.unpack_from("<IIIIIIII", image, 0)
    header_crc = struct.unpack_from("<I", image, HEADER_CRC_OFFSET)[0]
    ciphertext = bytes(image[data_offset:])

    if (magic != 0x4D524F46 or revision != 1 or size != 320 or
            data_type != 1 or data_offset != DATA_OFFSET or
            data_address != 0x08010000 or data_length != len(ciphertext) or
            (zlib.crc32(application) & 0xFFFFFFFF) != data_crc or
            (zlib.crc32(image[:HEADER_CRC_OFFSET]) & 0xFFFFFFFF) != header_crc or
            0 not in image[124:188]):
        raise AssertionError("XBIN does not follow the Formal revision 1 layout")
    if ciphertext == application or zlib.crc32(ciphertext) & 0xFFFFFFFF == data_crc:
        raise AssertionError("Formal payload would be mistaken for plaintext")
    print("PASS unified 320-byte Formal Header with mandatory ciphertext")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("xbin", type=Path)
    parser.add_argument("public_key", type=Path)
    parser.add_argument("--openssl", required=True, type=Path)
    parser.add_argument("--aes-tool", required=True, type=Path)
    parser.add_argument("--encryption-key", required=True, type=Path)
    parser.add_argument("--export-forged", type=Path)
    args = parser.parse_args()

    original = bytearray(args.xbin.read_bytes())
    application = decrypt_xbin_payload(
        bytes(original), args.aes_tool, args.encryption_key
    )
    validate_xbin(bytes(original), application, args.openssl, args.public_key,
                  args.aes_tool, args.encryption_key)
    validate_formal_layout(original, application)

    corrupted = bytearray(original)
    corrupted[DATA_OFFSET + 100] ^= 0x01
    expect_rejected("ciphertext corruption", corrupted, args.openssl,
                    args.public_key, args.aes_tool, args.encryption_key,
                    application)

    forged_payload = bytearray(corrupted)
    attacker_view = bytes(forged_payload[DATA_OFFSET:])
    struct.pack_into("<I", forged_payload, 28,
                     zlib.crc32(attacker_view) & 0xFFFFFFFF)
    forged_payload[64:96] = hashlib.sha256(attacker_view).digest()
    update_header_crc(forged_payload)
    expect_rejected("recomputed CRC and SHA without private key", forged_payload,
                    args.openssl, args.public_key, args.aes_tool,
                    args.encryption_key, application)
    if args.export_forged:
        args.export_forged.parent.mkdir(parents=True, exist_ok=True)
        args.export_forged.write_bytes(forged_payload)

    mutations = [
        ("metadata modification", 124),
        ("security version modification", 32),
        ("release sequence modification", 36),
        ("signature modification", 188),
        ("AES-GCM nonce modification", 96),
        ("AES key ID modification", 56),
        ("AES-GCM tag modification", 108),
    ]
    for name, offset in mutations:
        forged = bytearray(original)
        forged[offset] ^= 0x01
        update_header_crc(forged)
        expect_rejected(name, forged, args.openssl, args.public_key,
                        args.aes_tool, args.encryption_key, application)

    print("All XBIN Formal negative tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
