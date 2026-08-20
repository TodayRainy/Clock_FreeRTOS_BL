#!/usr/bin/env python3
"""Create the COS manifest for one XBIN Formal package."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

from validate_images import decrypt_xbin_payload

FORMAL_MAGIC = 0x4D524F46
FORMAL_REVISION = 1
HEADER_SIZE = 320
HEADER_AREA_SIZE = 4096


def read_c_string(data: bytes) -> str:
    """Decode one NUL-terminated ASCII field from the Formal Header."""
    return data.split(b"\0", 1)[0].decode("ascii")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate an OTA manifest from an XBIN Formal package"
    )
    parser.add_argument("xbin", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--url", required=True)
    parser.add_argument("--product", default="clock-f407")
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--aes-tool", required=True, type=Path)
    parser.add_argument("--encryption-key", required=True, type=Path)
    args = parser.parse_args()

    package = args.xbin.read_bytes()
    if len(package) < HEADER_AREA_SIZE:
        parser.error("XBIN is shorter than its 4096-byte header area")
    header = package[:HEADER_SIZE]
    (magic, revision, header_size, data_type, data_offset, data_address,
     data_length, data_crc32, security_version,
     release_sequence) = struct.unpack_from("<IIIIIIIIII", header, 0)
    signing_key_id, encryption_key_id = struct.unpack_from("<II", header, 52)
    image_sha256 = header[64:96]
    version = read_c_string(header[124:188])
    header_crc32 = struct.unpack_from("<I", header, 316)[0]

    try:
        payload = decrypt_xbin_payload(
            package, args.aes_tool, args.encryption_key
        )
    except ValueError as error:
        parser.error(str(error))

    if (magic != FORMAL_MAGIC or revision != FORMAL_REVISION or
            header_size != HEADER_SIZE):
        parser.error("not an XBIN Formal revision 1 image")
    if (data_type != 1 or data_offset != HEADER_AREA_SIZE or
            data_address != 0x08010000 or data_length != len(payload)):
        parser.error("XBIN length or address does not match its Formal Header")
    if zlib.crc32(header[:316]) & 0xFFFFFFFF != header_crc32:
        parser.error("XBIN Header CRC32 is invalid")
    if zlib.crc32(payload) & 0xFFFFFFFF != data_crc32:
        parser.error("XBIN payload CRC32 is invalid")
    if hashlib.sha256(payload).digest() != image_sha256:
        parser.error("XBIN payload SHA-256 is invalid")
    if not args.url.startswith("https://"):
        parser.error("OTA URL must use HTTPS")

    manifest = {
        "manifest_version": 2,
        "product": args.product,
        "channel": args.channel,
        "firmware_version": version,
        "release_sequence": release_sequence,
        "security_version": security_version,
        "xbin_format": "formal",
        "format_revision": revision,
        "xbin_size": len(package),
        "xbin_sha256": hashlib.sha256(package).hexdigest().upper(),
        "payload_size": data_length,
        "payload_crc32": f"{data_crc32:08X}",
        "payload_sha256": image_sha256.hex().upper(),
        "signing_key_id": f"{signing_key_id:08X}",
        "encryption": "AES-256-GCM",
        "encryption_key_id": f"{encryption_key_id:08X}",
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
