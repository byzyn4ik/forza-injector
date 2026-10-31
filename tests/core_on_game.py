"""Map and relocate a copied PE for offline tests; game code is never executed."""
import ctypes
from pathlib import Path
import struct


def map_image(path):
    data = Path(path).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections, optional = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    size, headers = struct.unpack_from("<I", data, pe + 24 + 56)[0], struct.unpack_from("<I", data, pe + 24 + 60)[0]
    alloc = ctypes.windll.kernel32.VirtualAlloc
    alloc.restype, alloc.argtypes = ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
    base = alloc(None, size, 0x3000, 0x40)  # PAGE_EXECUTE_READWRITE: the core changes protections itself
    ctypes.memmove(base, data, headers)
    for i in range(sections):
        _, vsize, va, raw_size, raw = struct.unpack_from("<8sIIII", data, pe + 24 + optional + 40 * i)
        length = min(raw_size, vsize or raw_size)
        ctypes.memmove(base + va, data[raw:raw + length], length)
    image_base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
    relocations, relocations_size = struct.unpack_from("<II", data, pe + 24 + 112 + 5 * 8)
    delta, at = base - image_base, 0
    while at < relocations_size:
        page, block = struct.unpack("<II", ctypes.string_at(base + relocations + at, 8))
        if not block:
            break
        for i in range((block - 8) // 2):
            entry = struct.unpack("<H", ctypes.string_at(base + relocations + at + 8 + 2 * i, 2))[0]
            if entry >> 12 == 10:  # IMAGE_REL_BASED_DIR64
                site = base + page + (entry & 0xFFF)
                value = struct.unpack("<Q", ctypes.string_at(site, 8))[0]
                ctypes.memmove(site, struct.pack("<Q", (value + delta) & 0xFFFFFFFFFFFFFFFF), 8)
        at += block
    return base

