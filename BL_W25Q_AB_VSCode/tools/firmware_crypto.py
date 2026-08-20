#!/usr/bin/env python3
"""Common ECDSA-P256 helpers used by the XBIN Formal toolchain.

The module deliberately delegates private-key operations to OpenSSL.  Python is
only responsible for the fixed XBIN representation: a 64-byte ``r || s``
signature and the 64-byte ``x || y`` public point expected by micro-ecc.
"""

from __future__ import annotations

import argparse
import hashlib
import secrets
import subprocess
import tempfile
from pathlib import Path


HEADER_SIZE = 320
ENCRYPTION_TAG_OFFSET = 108
ENCRYPTION_TAG_SIZE = 16
SIGNATURE_OFFSET = 188
SIGNATURE_SIZE = 64
HEADER_CRC_OFFSET = 316
SIGNATURE_DOMAIN = b"ClockFW-XBIN-Formal-r1"
P256_OID_DER = bytes.fromhex("06082A8648CE3D030107")
ENCRYPTION_KEY_SIZE = 32


def run_openssl(openssl: Path, *arguments: str, capture: bool = False) -> bytes:
    """Run one OpenSSL command and turn a failure into a concise build error."""
    command = [str(openssl), *map(str, arguments)]
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE,
    )
    if result.returncode:
        message = result.stderr.decode(errors="replace").strip()
        raise RuntimeError(f"OpenSSL failed: {' '.join(command)}\n{message}")
    return result.stdout if capture else b""


def _public_der(openssl: Path, key: Path, private: bool) -> bytes:
    arguments = ["pkey", "-in", str(key)]
    if not private:
        arguments.append("-pubin")
    arguments.extend(["-pubout", "-outform", "DER"])
    return run_openssl(openssl, *arguments, capture=True)


def public_point(openssl: Path, key: Path, private: bool = False) -> bytes:
    """Extract the P-256 public point as the 64-byte ``x || y`` value."""
    der = _public_der(openssl, key, private)
    # A prime256v1 SubjectPublicKeyInfo ends in the uncompressed SEC1 point.
    # The explicit curve OID prevents another 256-bit curve such as secp256k1
    # from producing a package which OpenSSL accepts but the MCU cannot verify.
    if P256_OID_DER not in der or len(der) < 65 or der[-65] != 0x04:
        raise ValueError(f"{key}: not an uncompressed P-256 public key")
    point = der[-64:]
    if len(point) != 64:
        raise ValueError(f"{key}: invalid P-256 public point length")
    return point


def public_key_id(point: bytes) -> int:
    """Return the compact key identifier stored in the XBIN header."""
    return int.from_bytes(hashlib.sha256(point).digest()[:4], "little")


def ensure_key_pair(openssl: Path, private_key: Path, public_key: Path,
                    create: bool) -> bytes:
    """Create a P-256 key pair when requested and verify both PEM files match."""
    private_exists = private_key.is_file()
    public_exists = public_key.is_file()

    if create and not private_exists and not public_exists:
        private_key.parent.mkdir(parents=True, exist_ok=True)
        public_key.parent.mkdir(parents=True, exist_ok=True)
        run_openssl(
            openssl, "ecparam", "-name", "prime256v1", "-genkey", "-noout",
            "-out", str(private_key)
        )
        run_openssl(
            openssl, "pkey", "-in", str(private_key), "-pubout",
            "-out", str(public_key)
        )
        private_exists = public_exists = True

    if not private_exists or not public_exists:
        raise FileNotFoundError(
            "firmware signing requires both private and public PEM files"
        )

    private_point = public_point(openssl, private_key, private=True)
    public = public_point(openssl, public_key)
    if private_point != public:
        raise ValueError("firmware private and public keys do not match")
    return public


def write_public_header(output: Path, point: bytes) -> None:
    """Generate the C header compiled into the Bootloader."""
    output.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    for offset in range(0, len(point), 8):
        values = ", ".join(f"0x{value:02X}u" for value in point[offset:offset + 8])
        rows.append(f"    {values},")

    text = f"""/**
 * @file firmware_public_key.h
 * @brief Generated P-256 firmware verification key; do not edit.
 */

#ifndef FIRMWARE_PUBLIC_KEY_H
#define FIRMWARE_PUBLIC_KEY_H

#include <stdint.h>

#define FIRMWARE_PUBLIC_KEY_ID 0x{public_key_id(point):08X}u

static const uint8_t firmware_public_key[64] =
{{
{chr(10).join(rows)}
}};

#endif /* FIRMWARE_PUBLIC_KEY_H */
"""
    output.write_text(text, encoding="utf-8", newline="\n")


def encryption_key_id(key: bytes) -> int:
    """Return the compact identifier for one AES-256 firmware key."""
    if len(key) != ENCRYPTION_KEY_SIZE:
        raise ValueError("firmware encryption key must contain exactly 32 bytes")
    return int.from_bytes(hashlib.sha256(key).digest()[:4], "little")


def ensure_encryption_key(path: Path, create: bool) -> bytes:
    """Create a random AES-256 key when requested, then validate its size."""
    if create and not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(secrets.token_bytes(ENCRYPTION_KEY_SIZE))
    if not path.is_file():
        raise FileNotFoundError("firmware encryption key file does not exist")
    key = path.read_bytes()
    if len(key) != ENCRYPTION_KEY_SIZE:
        raise ValueError("firmware encryption key must contain exactly 32 raw bytes")
    return key


def write_encryption_key_header(output: Path, key: bytes) -> None:
    """Generate the private AES key header compiled only into the Bootloader."""
    rows = []
    for offset in range(0, len(key), 8):
        values = ", ".join(f"0x{value:02X}u" for value in key[offset:offset + 8])
        rows.append(f"    {values},")

    text = f"""/**
 * @file firmware_encryption_key.h
 * @brief Generated AES-256 firmware decryption key; do not edit or commit.
 */

#ifndef FIRMWARE_ENCRYPTION_KEY_H
#define FIRMWARE_ENCRYPTION_KEY_H

#include <stdint.h>

#define FIRMWARE_ENCRYPTION_KEY_ID 0x{encryption_key_id(key):08X}u

static const uint8_t firmware_encryption_key[32] =
{{
{chr(10).join(rows)}
}};

#endif /* FIRMWARE_ENCRYPTION_KEY_H */
"""
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8", newline="\n")


def canonical_header(header: bytes) -> bytes:
    """Build the byte string hashed and signed for an XBIN Formal header."""
    if len(header) != HEADER_SIZE:
        raise ValueError("signature input must be one 320-byte Formal header")
    normalized = bytearray(header)
    normalized[SIGNATURE_OFFSET:SIGNATURE_OFFSET + SIGNATURE_SIZE] = bytes(SIGNATURE_SIZE)
    normalized[HEADER_CRC_OFFSET:HEADER_SIZE] = bytes(4)
    return SIGNATURE_DOMAIN + normalized


def encryption_aad(header: bytes) -> bytes:
    """Return the canonical Formal Header authenticated as AES-GCM AAD.

    GCM is calculated before the Tag and ECDSA signature exist.  Clearing those
    fields and the final CRC makes the order deterministic without excluding
    any ordinary metadata from authenticated encryption.
    """
    if len(header) != HEADER_SIZE:
        raise ValueError("GCM AAD input must be one 320-byte Formal header")
    normalized = bytearray(header)
    normalized[ENCRYPTION_TAG_OFFSET:
               ENCRYPTION_TAG_OFFSET + ENCRYPTION_TAG_SIZE] = bytes(
                   ENCRYPTION_TAG_SIZE
               )
    normalized[SIGNATURE_OFFSET:SIGNATURE_OFFSET + SIGNATURE_SIZE] = bytes(
        SIGNATURE_SIZE
    )
    normalized[HEADER_CRC_OFFSET:HEADER_SIZE] = bytes(4)
    return bytes(normalized)


def _read_der_length(data: bytes, offset: int) -> tuple[int, int]:
    first = data[offset]
    offset += 1
    if first < 0x80:
        return first, offset
    count = first & 0x7F
    if count == 0 or count > 2 or offset + count > len(data):
        raise ValueError("invalid ECDSA DER length")
    return int.from_bytes(data[offset:offset + count], "big"), offset + count


def _read_der_integer(data: bytes, offset: int) -> tuple[bytes, int]:
    if offset >= len(data) or data[offset] != 0x02:
        raise ValueError("invalid ECDSA DER integer")
    length, offset = _read_der_length(data, offset + 1)
    integer = data[offset:offset + length]
    if len(integer) != length or not integer:
        raise ValueError("truncated ECDSA DER integer")
    if integer[0] == 0:
        integer = integer[1:]
    if len(integer) > 32:
        raise ValueError("ECDSA integer is larger than P-256")
    return integer.rjust(32, b"\0"), offset + length


def der_to_raw_signature(signature: bytes) -> bytes:
    """Convert an OpenSSL DER ECDSA signature to fixed ``r || s`` form."""
    if not signature or signature[0] != 0x30:
        raise ValueError("invalid ECDSA DER sequence")
    sequence_length, offset = _read_der_length(signature, 1)
    if offset + sequence_length != len(signature):
        raise ValueError("invalid ECDSA DER sequence length")
    r, offset = _read_der_integer(signature, offset)
    s, offset = _read_der_integer(signature, offset)
    if offset != len(signature):
        raise ValueError("unexpected bytes after ECDSA signature")
    return r + s


def _der_integer(value: bytes) -> bytes:
    value = value.lstrip(b"\0") or b"\0"
    if value[0] & 0x80:
        value = b"\0" + value
    return b"\x02" + bytes([len(value)]) + value


def raw_to_der_signature(signature: bytes) -> bytes:
    """Convert fixed ``r || s`` form back to the DER expected by OpenSSL."""
    if len(signature) != SIGNATURE_SIZE:
        raise ValueError("raw ECDSA-P256 signature must be 64 bytes")
    body = _der_integer(signature[:32]) + _der_integer(signature[32:])
    return b"\x30" + bytes([len(body)]) + body


def sign_header(openssl: Path, private_key: Path, header: bytes) -> bytes:
    """Sign one canonical XBIN header and return a 64-byte signature."""
    with tempfile.TemporaryDirectory() as directory:
        message = Path(directory) / "header.bin"
        signature = Path(directory) / "signature.der"
        message.write_bytes(canonical_header(header))
        run_openssl(
            openssl, "dgst", "-sha256", "-sign", str(private_key),
            "-out", str(signature), str(message)
        )
        return der_to_raw_signature(signature.read_bytes())


def verify_header(openssl: Path, public_key: Path, header: bytes,
                  signature: bytes) -> bool:
    """Verify a raw XBIN signature with OpenSSL."""
    with tempfile.TemporaryDirectory() as directory:
        message = Path(directory) / "header.bin"
        signature_file = Path(directory) / "signature.der"
        message.write_bytes(canonical_header(header))
        signature_file.write_bytes(raw_to_der_signature(signature))
        result = subprocess.run(
            [str(openssl), "dgst", "-sha256", "-verify", str(public_key),
             "-signature", str(signature_file), str(message)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        return result.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare firmware signing keys")
    parser.add_argument("--openssl", required=True, type=Path)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--public-key", required=True, type=Path)
    parser.add_argument("--output-header", required=True, type=Path)
    parser.add_argument("--encryption-key", required=True, type=Path)
    parser.add_argument("--output-encryption-header", required=True, type=Path)
    parser.add_argument("--create", action="store_true",
                        help="create a local DEV key pair when both files are absent")
    parser.add_argument("--create-encryption-key", action="store_true",
                        help="create a random local 32-byte DEV encryption key")
    args = parser.parse_args()

    try:
        point = ensure_key_pair(
            args.openssl, args.private_key, args.public_key, args.create
        )
        write_public_header(args.output_header, point)
        encryption_key = ensure_encryption_key(
            args.encryption_key, args.create_encryption_key
        )
        write_encryption_key_header(
            args.output_encryption_header, encryption_key
        )
        print(f"Firmware verification key ID: 0x{public_key_id(point):08X}")
        print(
            "Firmware encryption key ID: "
            f"0x{encryption_key_id(encryption_key):08X}"
        )
    except (OSError, RuntimeError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
