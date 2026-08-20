#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Package WeatherClock as one signed and encrypted XBIN Formal image."""

import argparse
import hashlib
import secrets
import subprocess
import struct
import sys
import tempfile
import zlib
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "tools"))

from firmware_crypto import (  # noqa: E402
    encryption_aad,
    encryption_key_id,
    ensure_encryption_key,
    ensure_key_pair,
    public_key_id,
    sign_header,
)

FORMAL_MAGIC = 0x4D524F46  # "FORM"
FORMAL_REVISION = 1
FORMAL_HEADER_SIZE = 320
FORMAL_HEADER_AREA_SIZE = 4096
DATA_TYPE_FIRMWARE = 1
HASH_SHA256 = 1
SIGNATURE_ECDSA_P256_SHA256 = 1
ENCRYPTION_AES_256_GCM = 1
ENCRYPTION_NONCE_SIZE = 12
ENCRYPTION_TAG_SIZE = 16
APPLICATION_ADDRESS = 0x08010000


def encrypt_payload(aes_tool: Path, key_file: Path, nonce: bytes,
                    aad: bytes, plaintext: bytes) -> tuple[bytes, bytes]:
    """Encrypt one payload with the same Mbed TLS code used by Bootloader."""
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        nonce_file = work / "nonce.bin"
        aad_file = work / "aad.bin"
        input_file = work / "plain.bin"
        output_file = work / "cipher.bin"
        tag_file = work / "tag.bin"
        nonce_file.write_bytes(nonce)
        aad_file.write_bytes(aad)
        input_file.write_bytes(plaintext)

        result = subprocess.run(
            [str(aes_tool), "encrypt", str(key_file), str(nonce_file),
             str(aad_file), str(input_file), str(output_file), str(tag_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode:
            error = result.stderr.decode(errors="replace").strip()
            raise RuntimeError(f"AES-GCM packaging failed: {error}")

        ciphertext = output_file.read_bytes()
        tag = tag_file.read_bytes()
        if len(ciphertext) != len(plaintext) or len(tag) != ENCRYPTION_TAG_SIZE:
            raise RuntimeError("AES-GCM packaging tool returned invalid output sizes")
        return ciphertext, tag


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build a signed and AES-256-GCM encrypted XBIN Formal image"
    )
    parser.add_argument("input_file", type=Path)
    parser.add_argument("output_file", nargs="?", type=Path)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--public-key", required=True, type=Path)
    parser.add_argument("--openssl", required=True, type=Path)
    parser.add_argument("--encryption-key", required=True, type=Path)
    parser.add_argument("--aes-tool", required=True, type=Path)
    parser.add_argument("--version", default="formal-1.0.1")
    parser.add_argument("--security-version", type=lambda value: int(value, 0),
                        default=2)
    parser.add_argument("--release-sequence", type=lambda value: int(value, 0),
                        default=5)
    args = parser.parse_args()

    if not args.input_file.is_file():
        parser.error(f"input file does not exist: {args.input_file}")
    if not 0 <= args.security_version <= 0xFFFFFFFF:
        parser.error("security version must fit in uint32")
    if not 1 <= args.release_sequence <= 0xFFFFFFFF:
        parser.error("release sequence must be in uint32 range 1..0xFFFFFFFF")

    version_bytes = args.version.encode("ascii")
    if not version_bytes or len(version_bytes) >= 64:
        parser.error("version must contain 1..63 ASCII bytes")

    application = args.input_file.read_bytes()
    application_crc = zlib.crc32(application) & 0xFFFFFFFF
    application_sha256 = hashlib.sha256(application).digest()
    public = ensure_key_pair(
        args.openssl, args.private_key, args.public_key, create=False
    )
    aes_key = ensure_encryption_key(args.encryption_key, create=False)

    header = bytearray(FORMAL_HEADER_SIZE)
    struct.pack_into(
        "<IIIIIIII", header, 0,
        FORMAL_MAGIC,
        FORMAL_REVISION,
        FORMAL_HEADER_SIZE,
        DATA_TYPE_FIRMWARE,
        FORMAL_HEADER_AREA_SIZE,
        APPLICATION_ADDRESS,
        len(application),
        application_crc,
    )
    struct.pack_into("<II", header, 32,
                     args.security_version, args.release_sequence)
    struct.pack_into(
        "<IIIIII", header, 40,
        HASH_SHA256,
        SIGNATURE_ECDSA_P256_SHA256,
        ENCRYPTION_AES_256_GCM,
        public_key_id(public),
        encryption_key_id(aes_key),
        0,
    )
    header[64:96] = application_sha256
    nonce = secrets.token_bytes(ENCRYPTION_NONCE_SIZE)
    header[96:108] = nonce
    header[124:124 + len(version_bytes)] = version_bytes

    ciphertext, tag = encrypt_payload(
        args.aes_tool, args.encryption_key, nonce,
        encryption_aad(bytes(header)), application
    )
    header[108:124] = tag
    header[188:252] = sign_header(args.openssl, args.private_key, bytes(header))
    struct.pack_into("<I", header, 316,
                     zlib.crc32(header[:316]) & 0xFFFFFFFF)

    output_file = args.output_file or args.input_file.with_name(
        args.input_file.stem + "_upgrade.xbin"
    )
    output_file.parent.mkdir(parents=True, exist_ok=True)
    output_file.write_bytes(
        bytes(header).ljust(FORMAL_HEADER_AREA_SIZE, b"\0") + ciphertext
    )
    internal_header_file = output_file.with_name(
        args.input_file.stem + "_formal_header.bin"
    )
    internal_header_file.write_bytes(bytes(header))

    print(f"Generated XBIN Formal revision {FORMAL_REVISION}: {output_file}")
    print(f"Generated internal Formal Header: {internal_header_file}")
    print(f"Version: {args.version}")
    print(f"Signing key ID: 0x{public_key_id(public):08X}")
    print(f"Encryption key ID: 0x{encryption_key_id(aes_key):08X}")
    print(f"Security version: {args.security_version}")
    print(f"Release sequence: {args.release_sequence}")
    print(f"Image SHA-256: {application_sha256.hex().upper()}")
    print(f"AES-GCM nonce: {nonce.hex().upper()}")


if __name__ == "__main__":
    main()
