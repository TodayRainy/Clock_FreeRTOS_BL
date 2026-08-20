#!/usr/bin/env python3
"""Validate the address contract shared by the Bootloader, app and XBIN."""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import struct
import sys
import tempfile
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
ENCRYPTION_HEADER_OFFSET = 256
ENCRYPTION_HEADER_SIZE = 64
ENCRYPTION_MAGIC = 0x33434E45


def encryption_header(xbin: bytes) -> bytes | None:
    """Return and validate the optional XBIN v3 AES-GCM descriptor."""
    if len(xbin) < ENCRYPTION_HEADER_OFFSET + ENCRYPTION_HEADER_SIZE:
        return None
    header = xbin[
        ENCRYPTION_HEADER_OFFSET:ENCRYPTION_HEADER_OFFSET + ENCRYPTION_HEADER_SIZE
    ]
    if struct.unpack_from("<I", header, 0)[0] != ENCRYPTION_MAGIC:
        return None

    magic, version, size, algorithm, key_id = struct.unpack_from("<IIIII", header)
    stored_crc = struct.unpack_from("<I", header, 60)[0]
    if (magic != ENCRYPTION_MAGIC or version != 1 or size != ENCRYPTION_HEADER_SIZE or
            algorithm != 1 or key_id == 0 or header[48:60] != bytes(12) or
            zlib.crc32(header[:60]) & 0xFFFFFFFF != stored_crc):
        raise ValueError("XBIN: invalid AES-GCM encryption descriptor")
    return header


def decrypt_xbin_payload(xbin: bytes, aes_tool: Path | None,
                         encryption_key: Path | None) -> bytes:
    """Authenticate and decrypt v3 payload, or return a plaintext v2 payload."""
    descriptor = encryption_header(xbin)
    data_offset = struct.unpack_from("<I", xbin, 36)[0]
    payload = xbin[data_offset:]
    if descriptor is None:
        return payload
    if aes_tool is None or encryption_key is None:
        raise ValueError("XBIN: encrypted image validation requires AES tool and key")

    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        nonce_file = work / "nonce.bin"
        aad_file = work / "aad.bin"
        input_file = work / "cipher.bin"
        output_file = work / "plain.bin"
        tag_file = work / "tag.bin"
        nonce_file.write_bytes(descriptor[20:32])
        aad_file.write_bytes(xbin[:XBIN_STRUCT_SIZE] + descriptor[:32])
        input_file.write_bytes(payload)
        tag_file.write_bytes(descriptor[32:48])
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


def validate_xbin(xbin: bytes, app: bytes, openssl: Path, public_key: Path,
                  aes_tool: Path | None = None,
                  encryption_key: Path | None = None) -> None:
    """Validate a plaintext v2 or encrypted v3 XBIN and its inner signature."""
    if len(xbin) < XBIN_HEADER_SIZE:
        raise ValueError("XBIN: file is shorter than its 4096-byte header")

    (magic, format_version, header_size, hash_algorithm,
     signature_algorithm, key_id) = struct.unpack_from("<IIIIII", xbin, 0)
    security_version = struct.unpack_from("<I", xbin, 24)[0]
    release_sequence = struct.unpack_from("<I", xbin, 28)[0]
    data_type, data_offset, data_address, data_length, data_crc = struct.unpack_from(
        "<IIIII", xbin, 32
    )
    image_sha256 = xbin[52:84]
    this_address, header_crc = struct.unpack_from("<II", xbin, 248)
    version = xbin[96:160].split(b"\0", 1)[0].decode("ascii")
    signature = xbin[160:224]
    descriptor = encryption_header(xbin)
    plaintext = decrypt_xbin_payload(xbin, aes_tool, encryption_key)

    if magic != XBIN_MAGIC:
        raise ValueError(f"XBIN: bad magic 0x{magic:08X}")
    if (format_version != XBIN_FORMAT_VERSION or header_size != XBIN_STRUCT_SIZE or
            hash_algorithm != 1 or signature_algorithm != 1):
        raise ValueError("XBIN: unsupported signed image format or algorithm")
    if release_sequence == 0:
        raise ValueError("XBIN: release sequence 0 is reserved for legacy images")
    if data_type != 1 or data_offset != XBIN_HEADER_SIZE or data_address != APP_BASE:
        raise ValueError("XBIN: type, data offset or application address violates the contract")
    if this_address != BOOT_LIMIT:
        raise ValueError(f"XBIN: header address is 0x{this_address:08X}, expected 0x{BOOT_LIMIT:08X}")
    if data_length != len(app) or len(xbin) != data_offset + data_length:
        raise ValueError("XBIN: data length does not match the application binary")
    if plaintext != app:
        raise ValueError("XBIN: payload differs from the application binary")
    if zlib.crc32(xbin[:252]) & 0xFFFFFFFF != header_crc:
        raise ValueError("XBIN: header CRC32 mismatch")
    if zlib.crc32(plaintext) & 0xFFFFFFFF != data_crc:
        raise ValueError("XBIN: application CRC32 mismatch")
    if hashlib.sha256(plaintext).digest() != image_sha256:
        raise ValueError("XBIN: application SHA-256 mismatch")

    point = public_point(openssl, public_key)
    if key_id != public_key_id(point):
        raise ValueError("XBIN: key ID does not match the configured public key")
    if not verify_header(openssl, public_key, xbin[:XBIN_STRUCT_SIZE], signature):
        raise ValueError("XBIN: ECDSA-P256 signature mismatch")

    print(
        f"XBIN: container={'v3 AES-256-GCM' if descriptor else 'v2 plaintext'}, "
        f"version={version}, release={release_sequence}, "
        f"security={security_version}, payload={data_length} bytes, "
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
    parser.add_argument("--aes-tool", type=Path)
    parser.add_argument("--encryption-key", type=Path)
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

    print("All image checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
