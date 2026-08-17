#! /usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import sys
import time
import zlib

MAGIC_HEADER_MAGIC = 0x4D414749  # "MAGI"
DATA_TYPE_FIRMWARE = 1
BL_VERSION_MAJOR = 1
BL_VERSION_MINOR = 0
BL_VERSION_PATCH = 0
BL_VERSION_EXTRA = "alpha"

def main():
    project_root = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))

    if len(sys.argv) < 2:
        print("Usage: gen_magic_header.py <input_file>")
        sys.exit(1)

    binfile = os.path.realpath(sys.argv[1])
    if not os.path.isfile(binfile):
        print(f"Error: File '{binfile}' does not exist.")
        sys.exit(1)
    
    with open(binfile, "rb") as f:
        bin_data = f.read()
    bin_crc = zlib.crc32(bin_data) & 0xFFFFFFFF

    # 构建magic header
    header = []

    header.append((MAGIC_HEADER_MAGIC).to_bytes(4, byteorder='little'))  # magic
    header.append((0).to_bytes(4, byteorder='little'))  # bitmask
    header.append((0).to_bytes(4 * 6, byteorder='little'))  # reserved1

    header.append((DATA_TYPE_FIRMWARE).to_bytes(4, byteorder='little'))  # data_type
    header.append((4096).to_bytes(4, byteorder='little'))  # data_offset
    header.append((0x08010000).to_bytes(4, byteorder='little'))  # data_address
    header.append((len(bin_data)).to_bytes(4, byteorder='little'))  # data_length
    header.append((bin_crc).to_bytes(4, byteorder='little'))  # data_crc32
    header.append((0).to_bytes(4 * 11, byteorder='little'))  # reserved2

    version_date = time.strftime("%y%m%d", time.localtime())
    version_time = time.strftime("%H%M", time.localtime())
    version_str = f"v{BL_VERSION_MAJOR}.{BL_VERSION_MINOR}.{BL_VERSION_PATCH}-{version_date}-{version_time}-{BL_VERSION_EXTRA}"
    version_bytes = version_str.encode('ascii')
    version_bytes = version_bytes.ljust(128, b'\x00')
    header.append(version_bytes)  # version

    header.append((0).to_bytes(4 * 6, byteorder='little'))  # reserved3
    header.append((0x0800C000).to_bytes(4, byteorder='little'))  # this_address

    this_crc = zlib.crc32(b''.join(header)) & 0xFFFFFFFF
    header.append((this_crc).to_bytes(4, byteorder='little'))  # this_crc32

    magic_header = b''.join(header)
    magic_header = magic_header.ljust(4096, b'\x00')

    # 生成最终文件
    upgrade_filename = os.path.splitext(os.path.basename(binfile))[0] + "_upgrade.xbin"
    output_file = os.path.join(project_root, "generated", upgrade_filename)
    os.makedirs(os.path.dirname(output_file), exist_ok=True)
    with open(output_file, "wb") as f:
        f.write(b''.join([magic_header, bin_data]))

    print(f"Generated file: {output_file}")

if __name__ == "__main__":
    main()