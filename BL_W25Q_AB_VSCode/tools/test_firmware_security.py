#!/usr/bin/env python3
"""Negative tests for the signed and AES-GCM encrypted XBIN trust boundary."""

from __future__ import annotations

import argparse
import hashlib
import struct
import zlib
from pathlib import Path

from validate_images import decrypt_xbin_payload, encryption_header, validate_xbin


DATA_OFFSET = 4096


def update_header_crc(image: bytearray) -> None:
    """Recalculate the non-security header CRC after an intentional mutation."""
    struct.pack_into("<I", image, 252, zlib.crc32(image[:252]) & 0xFFFFFFFF)


def update_encryption_crc(image: bytearray) -> None:
    """Recalculate the non-security CRC of the 64-byte encryption descriptor."""
    struct.pack_into("<I", image, 316,
                     zlib.crc32(image[256:316]) & 0xFFFFFFFF)


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


def validate_transition_layout(image: bytearray, application: bytes) -> None:
    """Confirm the signed inner header remains v2 and old loaders fail safely."""
    magic = struct.unpack_from("<I", image, 0)[0]
    data_type, data_offset, data_address, data_length, data_crc = struct.unpack_from(
        "<IIIII", image, 32
    )
    header_crc = struct.unpack_from("<I", image, 252)[0]
    payload = bytes(image[data_offset:])

    if (magic != 0x4D414749 or data_type != 1 or data_offset != DATA_OFFSET or
            data_address != 0x08010000 or data_length != len(payload) or
            (zlib.crc32(application) & 0xFFFFFFFF) != data_crc or
            (zlib.crc32(image[:252]) & 0xFFFFFFFF) != header_crc or
            0 not in image[96:160]):
        raise AssertionError("XBIN v3 inner signed header no longer follows v2")
    if zlib.crc32(payload) & 0xFFFFFFFF == data_crc:
        raise AssertionError("encrypted payload would be mistaken for plaintext")
    print("PASS signed v2 inner header; legacy loader rejects ciphertext safely")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("xbin", type=Path)
    parser.add_argument("public_key", type=Path)
    parser.add_argument("--openssl", required=True, type=Path)
    parser.add_argument("--aes-tool", required=True, type=Path)
    parser.add_argument("--encryption-key", required=True, type=Path)
    parser.add_argument("--export-forged", type=Path,
                        help="write the recomputed-CRC/SHA forgery for device fault injection")
    args = parser.parse_args()

    original = bytearray(args.xbin.read_bytes())
    if encryption_header(original) is None:
        raise AssertionError("security test requires an encrypted XBIN v3")
    application = decrypt_xbin_payload(
        bytes(original), args.aes_tool, args.encryption_key
    )
    validate_xbin(bytes(original), application, args.openssl, args.public_key,
                  args.aes_tool, args.encryption_key)
    validate_transition_layout(original, application)

    corrupted = bytearray(original)
    corrupted[DATA_OFFSET + 100] ^= 0x01
    expect_rejected("ciphertext corruption", corrupted, args.openssl,
                    args.public_key, args.aes_tool, args.encryption_key,
                    application)

    forged_payload = bytearray(corrupted)
    forged_data = bytes(forged_payload[DATA_OFFSET:])
    struct.pack_into("<I", forged_payload, 48,
                     zlib.crc32(forged_data) & 0xFFFFFFFF)
    forged_payload[52:84] = hashlib.sha256(forged_data).digest()
    update_header_crc(forged_payload)
    expect_rejected("recomputed CRC and SHA without private key", forged_payload,
                    args.openssl, args.public_key, args.aes_tool,
                    args.encryption_key, application)
    if args.export_forged:
        args.export_forged.parent.mkdir(parents=True, exist_ok=True)
        args.export_forged.write_bytes(forged_payload)
        print(f"Exported unsigned forgery: {args.export_forged}")

    forged_metadata = bytearray(original)
    forged_metadata[96] ^= 0x01
    update_header_crc(forged_metadata)
    expect_rejected("metadata modification", forged_metadata,
                    args.openssl, args.public_key, args.aes_tool,
                    args.encryption_key, application)

    forged_security_version = bytearray(original)
    current_security_version = struct.unpack_from("<I", forged_security_version, 24)[0]
    struct.pack_into("<I", forged_security_version, 24,
                     current_security_version ^ 1)
    update_header_crc(forged_security_version)
    expect_rejected("security version modification", forged_security_version,
                    args.openssl, args.public_key, args.aes_tool,
                    args.encryption_key, application)

    forged_release_sequence = bytearray(original)
    current_release_sequence = struct.unpack_from("<I", forged_release_sequence, 28)[0]
    changed_release_sequence = (
        current_release_sequence - 1
        if current_release_sequence == 0xFFFFFFFF
        else current_release_sequence + 1
    )
    struct.pack_into("<I", forged_release_sequence, 28,
                     changed_release_sequence)
    update_header_crc(forged_release_sequence)
    expect_rejected("release sequence modification", forged_release_sequence,
                    args.openssl, args.public_key, args.aes_tool,
                    args.encryption_key, application)

    forged_signature = bytearray(original)
    forged_signature[160] ^= 0x01
    update_header_crc(forged_signature)
    expect_rejected("signature modification", forged_signature,
                    args.openssl, args.public_key, args.aes_tool,
                    args.encryption_key, application)

    forged_nonce = bytearray(original)
    forged_nonce[276] ^= 0x01
    update_encryption_crc(forged_nonce)
    expect_rejected("AES-GCM nonce modification", forged_nonce,
                    args.openssl, args.public_key, args.aes_tool,
                    args.encryption_key, application)

    forged_encryption_key_id = bytearray(original)
    current_encryption_key_id = struct.unpack_from(
        "<I", forged_encryption_key_id, 272
    )[0]
    struct.pack_into("<I", forged_encryption_key_id, 272,
                     current_encryption_key_id ^ 1)
    update_encryption_crc(forged_encryption_key_id)
    expect_rejected("AES-GCM key ID modification", forged_encryption_key_id,
                    args.openssl, args.public_key, args.aes_tool,
                    args.encryption_key, application)

    forged_tag = bytearray(original)
    forged_tag[288] ^= 0x01
    update_encryption_crc(forged_tag)
    expect_rejected("AES-GCM tag modification", forged_tag,
                    args.openssl, args.public_key, args.aes_tool,
                    args.encryption_key, application)

    print("All signed/encrypted XBIN negative tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
