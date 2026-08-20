#! /usr/bin/env python3
# -*- coding: utf-8 -*-

"""Package a WeatherClock application binary as a signed XBIN v2 image."""

import argparse
import hashlib
import os
import struct
import sys
import time
import zlib
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "tools"))

from firmware_crypto import (  # noqa: E402
    ensure_key_pair,
    public_key_id,
    sign_header,
)

MAGIC_HEADER_MAGIC = 0x4D414749  # "MAGI"
MAGIC_HEADER_FORMAT = 2
MAGIC_HEADER_SIZE = 256
MAGIC_HEADER_AREA_SIZE = 4096
DATA_TYPE_FIRMWARE = 1
HASH_SHA256 = 1
SIGNATURE_ECDSA_P256_SHA256 = 1
BL_VERSION_MAJOR = 1
BL_VERSION_MINOR = 4
BL_VERSION_PATCH = 0
BL_VERSION_EXTRA = "ab"

def main():
    parser = argparse.ArgumentParser(description="Build a signed XBIN v2 image")
    parser.add_argument("input_file", type=Path)
    parser.add_argument("output_file", nargs="?", type=Path)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--public-key", required=True, type=Path)
    parser.add_argument("--openssl", required=True, type=Path)
    parser.add_argument("--version")
    parser.add_argument("--security-version", type=lambda value: int(value, 0),
                        default=1)
    parser.add_argument("--release-sequence", type=lambda value: int(value, 0),
                        default=1)
    args = parser.parse_args()

    if not args.input_file.is_file():
        parser.error(f"input file does not exist: {args.input_file}")
    if not 0 <= args.security_version <= 0xFFFFFFFF:
        parser.error("security version must fit in uint32")
    if not 1 <= args.release_sequence <= 0xFFFFFFFF:
        parser.error("release sequence must be in uint32 range 1..0xFFFFFFFF")

    bin_data = args.input_file.read_bytes()
    bin_crc = zlib.crc32(bin_data) & 0xFFFFFFFF
    bin_sha256 = hashlib.sha256(bin_data).digest()
    public_point = ensure_key_pair(
        args.openssl, args.private_key, args.public_key, create=False
    )
    key_id = public_key_id(public_point)

    # Header保持为固定偏移的二进制结构，避免C、Python和C#各自做隐式对齐。
    header = bytearray(MAGIC_HEADER_SIZE)
    struct.pack_into(
        "<IIIIII", header, 0,
        MAGIC_HEADER_MAGIC,
        MAGIC_HEADER_FORMAT,
        MAGIC_HEADER_SIZE,
        HASH_SHA256,
        SIGNATURE_ECDSA_P256_SHA256,
        key_id,
    )
    struct.pack_into("<I", header, 24, args.security_version)
    struct.pack_into("<I", header, 28, args.release_sequence)
    struct.pack_into(
        "<IIIII", header, 32,
        DATA_TYPE_FIRMWARE,
        MAGIC_HEADER_AREA_SIZE,
        0x08010000,
        len(bin_data),
        bin_crc,
    )
    header[52:84] = bin_sha256

    version_date = time.strftime("%y%m%d", time.localtime())
    version_time = time.strftime("%H%M", time.localtime())
    version_str = args.version or (
        f"v{BL_VERSION_MAJOR}.{BL_VERSION_MINOR}.{BL_VERSION_PATCH}-"
        f"{version_date}-{version_time}-{BL_VERSION_EXTRA}"
    )
    version_bytes = version_str.encode('ascii')
    if len(version_bytes) >= 64:
        parser.error("version string must be shorter than 64 ASCII bytes")
    header[96:96 + len(version_bytes)] = version_bytes
    struct.pack_into("<I", header, 248, 0x0800C000)

    # 先对signature和this_crc32为零的规范化Header签名，再让CRC32覆盖签名。
    header[160:224] = sign_header(args.openssl, args.private_key, bytes(header))
    struct.pack_into("<I", header, 252, zlib.crc32(header[:252]) & 0xFFFFFFFF)

    magic_header = bytes(header).ljust(MAGIC_HEADER_AREA_SIZE, b'\x00')

    # 生成最终文件
    output_file = args.output_file
    if output_file is None:
        output_file = args.input_file.with_name(args.input_file.stem + "_upgrade.xbin")
    output_file.parent.mkdir(parents=True, exist_ok=True)
    output_file.write_bytes(magic_header + bin_data)

    print(f"Generated signed XBIN: {output_file}")
    print(f"Version: {version_str}")
    print(f"Key ID: 0x{key_id:08X}")
    print(f"Security version: {args.security_version}")
    print(f"Release sequence: {args.release_sequence}")
    print(f"Image SHA-256: {bin_sha256.hex().upper()}")

if __name__ == "__main__":
    main()
