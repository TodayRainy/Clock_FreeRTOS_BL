#!/usr/bin/env python3
"""Create the small COS manifest consumed by WeatherClock OTA."""

import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

MAGIC = 0x4D414749
FORMAT_V2 = 2
HEADER_SIZE = 256
HEADER_AREA_SIZE = 4096


def read_c_string(data: bytes) -> str:
    return data.split(b"\0", 1)[0].decode("ascii")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate an OTA manifest from a signed XBIN v2 image"
    )
    parser.add_argument("xbin", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--url", required=True)
    parser.add_argument("--product", default="clock-f407")
    parser.add_argument("--channel", default="stable")
    args = parser.parse_args()

    package = args.xbin.read_bytes()
    if len(package) < HEADER_AREA_SIZE:
        parser.error("XBIN is shorter than its 4096-byte header area")
    header = package[:HEADER_SIZE]
    magic, image_format = struct.unpack_from("<II", header, 0)
    security_version = struct.unpack_from("<I", header, 24)[0]
    release_sequence = struct.unpack_from("<I", header, 28)[0]
    data_offset, data_address, data_length, data_crc32 = struct.unpack_from(
        "<IIII", header, 36
    )
    image_sha256 = header[52:84]
    version = read_c_string(header[96:160])
    header_crc32 = struct.unpack_from("<I", header, 252)[0]
    payload = package[data_offset:]

    if magic != MAGIC or image_format != FORMAT_V2:
        parser.error("not a signed XBIN v2 image")
    if (
        data_offset != HEADER_AREA_SIZE
        or data_address != 0x08010000
        or data_length != len(payload)
    ):
        parser.error("XBIN length does not match its header")
    if zlib.crc32(header[:252]) & 0xFFFFFFFF != header_crc32:
        parser.error("XBIN header CRC32 is invalid")
    if zlib.crc32(payload) & 0xFFFFFFFF != data_crc32:
        parser.error("XBIN payload CRC32 is invalid")
    if hashlib.sha256(payload).digest() != image_sha256:
        parser.error("XBIN payload SHA-256 is invalid")
    if not args.url.startswith("https://"):
        parser.error("OTA URL must use HTTPS")

    manifest = {
        "manifest_version": 1,
        "product": args.product,
        "channel": args.channel,
        "firmware_version": version,
        "release_sequence": release_sequence,
        "security_version": security_version,
        "xbin_format_version": image_format,
        "xbin_size": len(package),
        "xbin_sha256": hashlib.sha256(package).hexdigest().upper(),
        "payload_size": data_length,
        "payload_crc32": f"{data_crc32:08X}",
        "payload_sha256": image_sha256.hex().upper(),
        "download_url": args.url,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )
    print(f"OTA manifest: {args.output}")
    print(f"Version: {version}")
    print(f"Release sequence: {release_sequence}")
    print(f"XBIN SHA-256: {manifest['xbin_sha256']}")


if __name__ == "__main__":
    main()
