#!/usr/bin/env python3
"""验证断点续传的分块、首个差异定位和擦除回退规则。"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


DATA_OFFSET = 4096
BLOCK_SIZE = 4096
ERASE_ALIGNMENT = 32 * 1024
MAX_DATA_LENGTH = 0x70000
MAX_PACKET_PAYLOAD = 8192 + 8


def block_crc32(data: bytes) -> list[int]:
    """按协议规定生成CRC列表，最后一块只计算真实数据。"""
    return [
        zlib.crc32(data[offset : offset + BLOCK_SIZE]) & 0xFFFFFFFF
        for offset in range(0, len(data), BLOCK_SIZE)
    ]


def find_resume_offset(stored: bytes, expected: bytes) -> int:
    """模拟设备顺序扫描，返回槽内绝对续传偏移。"""
    expected_crc = block_crc32(expected)
    for index, crc in enumerate(expected_crc):
        begin = index * BLOCK_SIZE
        end = min(begin + BLOCK_SIZE, len(expected))
        if zlib.crc32(stored[begin:end]) & 0xFFFFFFFF != crc:
            return DATA_OFFSET + begin
    return DATA_OFFSET + len(expected)


def rewrite_offset(resume_offset: int) -> int:
    """模拟上位机为使用块擦除而回退到前一个32 KiB边界。"""
    erase_start = resume_offset & ~(ERASE_ALIGNMENT - 1)
    return max(erase_start, DATA_OFFSET)


def test_synthetic_interrupts() -> None:
    length = 76 * BLOCK_SIZE + 123
    expected = bytes(((index * 17 + index // 251) & 0xFF) for index in range(length))

    assert find_resume_offset(expected, expected) == DATA_OFFSET + length
    assert find_resume_offset(bytes([0xFF]) * length, expected) == DATA_OFFSET

    for percent in (10, 50, 95):
        cut = length * percent // 100
        stored = bytearray([0xFF]) * length
        stored[:cut] = expected[:cut]
        found = find_resume_offset(stored, expected)
        wanted = DATA_OFFSET + (cut // BLOCK_SIZE) * BLOCK_SIZE
        assert found == wanted, (percent, found, wanted)

        rewrite = rewrite_offset(found)
        assert DATA_OFFSET <= rewrite <= found
        assert rewrite == DATA_OFFSET or rewrite % ERASE_ALIGNMENT == 0


def test_real_xbin(path: Path) -> None:
    xbin = path.read_bytes()
    if len(xbin) < DATA_OFFSET:
        raise ValueError("XBIN shorter than the fixed data offset")

    data_offset = struct.unpack_from("<I", xbin, 36)[0]
    data_length = struct.unpack_from("<I", xbin, 44)[0]
    if data_offset != DATA_OFFSET or not 0 < data_length <= MAX_DATA_LENGTH:
        raise ValueError("XBIN data layout does not match the resume protocol")
    if data_offset + data_length != len(xbin):
        raise ValueError("XBIN length does not match its Header")

    data = xbin[data_offset:]
    crc_list = block_crc32(data)
    request_size = 7 + len(crc_list) * 4
    if request_size > MAX_PACKET_PAYLOAD:
        raise ValueError("resume CRC request exceeds Bootloader MTU")
    if find_resume_offset(data, data) != len(xbin):
        raise AssertionError("complete XBIN was not recognized as fully reusable")

    print(
        f"Resume layout: data={data_length} bytes, blocks={len(crc_list)}, "
        f"request={request_size} bytes"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("xbin", type=Path)
    args = parser.parse_args()

    test_synthetic_interrupts()
    test_real_xbin(args.xbin)
    print("Resumable-upgrade algorithm tests passed.")


if __name__ == "__main__":
    main()
