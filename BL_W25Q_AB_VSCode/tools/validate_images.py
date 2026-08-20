#!/usr/bin/env python3
"""Validate the Bootloader, WeatherClock and unified XBIN Formal contract."""

from __future__ import annotations

import argparse
import hashlib
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

from firmware_crypto import (
    encryption_aad,
    encryption_key_id,
    ensure_encryption_key,
    public_key_id,
    public_point,
    verify_header,
)


BOOT_BASE = 0x08000000
BOOT_LIMIT = 0x0800C000
APP_BASE = 0x08010000
APP_LIMIT = 0x08080000
SRAM_BASE = 0x20000000
SRAM_LIMIT = 0x20020000
XBIN_HEADER_AREA_SIZE = 4096
XBIN_MAGIC = 0x4D524F46  # "FORM"
XBIN_FORMAT_REVISION = 1
XBIN_HEADER_SIZE = 320
SIGNATURE_OFFSET = 188
SIGNATURE_SIZE = 64
HEADER_CRC_OFFSET = 316


def decrypt_xbin_payload(xbin: bytes, aes_tool: Path,
                         encryption_key: Path) -> bytes:
    """Authenticate and decrypt the mandatory Formal payload."""
    header = xbin[:XBIN_HEADER_SIZE]
    data_offset = struct.unpack_from("<I", header, 16)[0]
    payload = xbin[data_offset:]

    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        nonce_file = work / "nonce.bin"
        aad_file = work / "aad.bin"
        input_file = work / "cipher.bin"
        output_file = work / "plain.bin"
        tag_file = work / "tag.bin"
        nonce_file.write_bytes(header[96:108])
        aad_file.write_bytes(encryption_aad(header))
        input_file.write_bytes(payload)
        tag_file.write_bytes(header[108:124])
        result = subprocess.run(
            [str(aes_tool), "decrypt", str(encryption_key), str(nonce_file),
             str(aad_file), str(input_file), str(output_file), str(tag_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode:
            message = result.stderr.decode(errors="replace").strip()
            raise ValueError(f"XBIN: AES-GCM authentication failed ({message})")
        return output_file.read_bytes()


def validate_vector(image: bytes, base: int, limit: int, name: str) -> None:
    """Check size, initial MSP and Thumb reset vector of one raw image."""
    if len(image) < 8:
        raise ValueError(f"{name}: image is shorter than the vector table")
    if base + len(image) > limit:
        raise ValueError(
            f"{name}: image ends at 0x{base + len(image):08X}, "
            f"past 0x{limit:08X}"
        )

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


def validate_xbin(xbin: bytes, app: bytes, openssl: Path, public_key: Path,
                  aes_tool: Path, encryption_key: Path) -> None:
    """Validate one signed and encrypted XBIN Formal image."""
    if len(xbin) < XBIN_HEADER_AREA_SIZE:
        raise ValueError("XBIN: file is shorter than its 4096-byte header area")

    header = xbin[:XBIN_HEADER_SIZE]
    (magic, revision, header_size, data_type, data_offset, data_address,
     data_length, data_crc, security_version, release_sequence,
     hash_algorithm, signature_algorithm, encryption_algorithm,
     signing_key_id, aes_key_id, reserved0) = struct.unpack_from(
         "<IIIIIIIIIIIIIIII", header, 0
     )
    image_sha256 = header[64:96]
    version = header[124:188].split(b"\0", 1)[0].decode("ascii")
    signature = header[SIGNATURE_OFFSET:SIGNATURE_OFFSET + SIGNATURE_SIZE]
    header_crc = struct.unpack_from("<I", header, HEADER_CRC_OFFSET)[0]
    plaintext = decrypt_xbin_payload(xbin, aes_tool, encryption_key)

    if magic != XBIN_MAGIC:
        raise ValueError(f"XBIN: bad Formal magic 0x{magic:08X}")
    if revision != XBIN_FORMAT_REVISION or header_size != XBIN_HEADER_SIZE:
        raise ValueError("XBIN: unsupported Formal format revision")
    if (data_type != 1 or data_offset != XBIN_HEADER_AREA_SIZE or
            data_address != APP_BASE):
        raise ValueError("XBIN: type, data offset or application address is invalid")
    if (hash_algorithm != 1 or signature_algorithm != 1 or
            encryption_algorithm != 1):
        raise ValueError("XBIN: Formal requires SHA-256, ECDSA-P256 and AES-GCM")
    if (signing_key_id == 0 or aes_key_id == 0 or reserved0 != 0 or
            header[252:316] != bytes(64)):
        raise ValueError("XBIN: key IDs or reserved fields are invalid")
    if not version:
        raise ValueError("XBIN: version string is empty")
    if data_length != len(app) or len(xbin) != data_offset + data_length:
        raise ValueError("XBIN: data length does not match the application binary")
    if plaintext != app:
        raise ValueError("XBIN: decrypted payload differs from the application binary")
    if zlib.crc32(header[:HEADER_CRC_OFFSET]) & 0xFFFFFFFF != header_crc:
        raise ValueError("XBIN: Header CRC32 mismatch")
    if zlib.crc32(plaintext) & 0xFFFFFFFF != data_crc:
        raise ValueError("XBIN: application CRC32 mismatch")
    if hashlib.sha256(plaintext).digest() != image_sha256:
        raise ValueError("XBIN: application SHA-256 mismatch")

    point = public_point(openssl, public_key)
    if signing_key_id != public_key_id(point):
        raise ValueError("XBIN: signing key ID does not match the public key")
    aes_key = ensure_encryption_key(encryption_key, create=False)
    if aes_key_id != encryption_key_id(aes_key):
        raise ValueError("XBIN: encryption key ID does not match the AES key")
    if not verify_header(openssl, public_key, header, signature):
        raise ValueError("XBIN: ECDSA-P256 signature mismatch")

    print(
        f"XBIN Formal r{revision}: version={version}, release={release_sequence}, "
        f"security={security_version}, payload={data_length} bytes, "
        f"signing-key=0x{signing_key_id:08X}, "
        f"encryption-key=0x{aes_key_id:08X}, CRC=0x{data_crc:08X}, "
        f"SHA-256={image_sha256.hex().upper()}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bootloader", type=Path)
    parser.add_argument("application", type=Path)
    parser.add_argument("xbin", type=Path)
    parser.add_argument("public_key", type=Path)
    parser.add_argument("--openssl", required=True, type=Path)
    parser.add_argument("--aes-tool", required=True, type=Path)
    parser.add_argument("--encryption-key", required=True, type=Path)
    args = parser.parse_args()

    try:
        boot = args.bootloader.read_bytes()
        app = args.application.read_bytes()
        xbin = args.xbin.read_bytes()
        validate_vector(boot, BOOT_BASE, BOOT_LIMIT, "Bootloader")
        validate_vector(app, APP_BASE, APP_LIMIT, "WeatherClock")
        validate_xbin(xbin, app, args.openssl, args.public_key,
                      args.aes_tool, args.encryption_key)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"Validation failed: {error}", file=sys.stderr)
        return 1

    print("All Formal image checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
