"""The code identity of an Azimuth executable: what stays the same when only
its resources (icon, version details, manifest) are changed.

SHA-256 over, for every section in header order except .rsrc: its Name (8
bytes), VirtualSize, VirtualAddress (0 for a section after .rsrc: a larger
.rsrc moves it), Characteristics and SizeOfRawData (each u32 LE), then its
raw data; and at the end AddressOfEntryPoint (u32 LE) and ImageBase (u64 LE).
The core (core/adapters/azimuth.c, host_code_identity) computes the same; a
builder that brands azimuth.exe checks it too.

Usage: py tools/azimuth_identity.py <azimuth.exe>...
"""
# SPDX-License-Identifier: GPL-3.0-or-later

import hashlib
from pathlib import Path
import struct
import sys


def code_identity(data):
    """The code identity (32 bytes) of a PE64 file's bytes; ValueError when it is not one."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError("not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    count, optional_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if optional_size < 112 or struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("not a PE32+ file")
    entry = struct.unpack_from("<I", data, optional + 16)[0]
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    table = optional + optional_size
    if table + 40 * count > len(data):
        raise ValueError("truncated section table")
    digest = hashlib.sha256()
    after_resources = False
    for index in range(count):
        header = table + 40 * index
        name = data[header:header + 8]
        virtual_size, address, raw_size, raw = struct.unpack_from("<IIII", data, header + 8)
        characteristics = struct.unpack_from("<I", data, header + 36)[0]
        if name.rstrip(b"\0") == b".rsrc":
            after_resources = True
            continue
        if raw + raw_size > len(data):
            raise ValueError("truncated section")
        digest.update(name + struct.pack("<IIII", virtual_size, 0 if after_resources else address, characteristics, raw_size))
        digest.update(data[raw:raw + raw_size])
    digest.update(struct.pack("<IQ", entry, image_base))
    return digest.digest()


def main():
    for path in sys.argv[1:]:
        print(code_identity(Path(path).read_bytes()).hex(), path)


if __name__ == "__main__":
    main()
