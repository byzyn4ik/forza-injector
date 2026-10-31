"""Branded Azimuth hosts: the core accepts a copy of the Azimuth 1.0.4 release
whose resources only were changed (icon, version details, manifest), also
renamed, and refuses one whose code differs. Nothing is run: the copies are
mapped and relocated (core_on_game.map_image) and the core's test build
checks them as at startup (verify_host).

Needs azimuth-windows-1.0.4.zip in the --game folder (default ~/FH2).
"""
# SPDX-License-Identifier: GPL-3.0-or-later

import ctypes
from ctypes import wintypes
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
from azimuth_identity import code_identity  # noqa: E402

RT_ICON, RT_GROUP_ICON, RT_VERSION, RT_MANIFEST = 3, 14, 16, 24

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_begin = _kernel32.BeginUpdateResourceW
_begin.argtypes, _begin.restype = [wintypes.LPCWSTR, wintypes.BOOL], wintypes.HANDLE
_update = _kernel32.UpdateResourceW
_update.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD, ctypes.c_void_p, wintypes.DWORD]
_update.restype = wintypes.BOOL
_end = _kernel32.EndUpdateResourceW
_end.argtypes, _end.restype = [wintypes.HANDLE, wintypes.BOOL], wintypes.BOOL


def _node(key, value=b"", length=0, text=False, children=()):
    body = struct.pack("<HHH", 0, length, 1 if text else 0) + (key + "\0").encode("utf-16-le")
    body += b"\0" * (-len(body) % 4) + value
    body += b"\0" * (-len(body) % 4)
    for child in children:
        body += child
    return struct.pack("<H", len(body)) + body[2:]


def version_info(name, exe):
    fixed = struct.pack("<13I", 0xFEEF04BD, 0x10000, 0x10000, 0x40, 0x10000, 0x40, 0x3F, 0, 0x40004, 1, 0, 0, 0)
    strings = {"FileDescription": name, "ProductName": name, "OriginalFilename": exe, "FileVersion": "1.0.0.64"}
    entries = [_node(k, (v + "\0").encode("utf-16-le"), len(v) + 1, True) for k, v in strings.items()]
    return _node("VS_VERSION_INFO", fixed, len(fixed), False,
                 [_node("StringFileInfo", text=True, children=[_node("040904b0", text=True, children=entries)]),
                  _node("VarFileInfo", text=True, children=[_node("Translation", bytes([9, 4, 0xB0, 4]), 4)])])


def resource_section(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count, optional = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    for index in range(count):
        header = pe + 24 + optional + 40 * index
        if data[header:header + 8].rstrip(b"\0") == b".rsrc":
            return struct.unpack_from("<IIII", data, header + 8)  # virtual size, address, raw size, raw offset
    return None


def manifest_of(data):
    """Azimuth's RT_MANIFEST 1 (read from the release file's resource tree)."""
    _, address, _, raw = resource_section(data)
    def entry(offset, wanted):
        named, ids = struct.unpack_from("<HH", data, raw + offset + 12)
        for i in range(named + ids):
            name, target = struct.unpack_from("<II", data, raw + offset + 16 + 8 * i)
            if wanted is None or name == wanted:
                return target & 0x7FFFFFFF
        raise KeyError(wanted)
    leaf = entry(entry(entry(0, RT_MANIFEST), 1), None)
    rva, size = struct.unpack_from("<II", data, raw + leaf)
    return data[raw + rva - address:raw + rva - address + size]


def brand(path, name, icon_size):
    """Gives a copy the game's details: version info, a manifest description and
    an icon of icon_size bytes (Azimuth's seven icons removed)."""
    data = path.read_bytes()
    manifest = manifest_of(data).replace(b"</assemblyIdentity>",
                                         b"</assemblyIdentity><description>" + name.encode() + b"</description>")
    version = version_info(name, path.name)
    icon = b"\x89PNG\r\n\x1a\n" + bytes((i * 7919) & 0xFF for i in range(icon_size - 8))
    group = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHIH", 0, 0, 0, 0, 1, 32, len(icon), 1)
    handle = _begin(str(path), False)
    if not handle:
        raise OSError(ctypes.get_last_error(), "BeginUpdateResource")
    def put(kind, ident, language, blob):
        buffer = ctypes.create_string_buffer(blob, len(blob)) if blob else None
        if not _update(handle, kind, ident, language, buffer, len(blob) if blob else 0):
            raise OSError(ctypes.get_last_error(), f"UpdateResource {kind}/{ident}")
    for ident in range(1, 8):
        put(RT_ICON, ident, 1033, None)
    put(RT_GROUP_ICON, 1, 1033, None)
    put(RT_ICON, 1, 1033, icon)
    put(RT_GROUP_ICON, 1, 1033, group)
    put(RT_VERSION, 1, 1033, version)
    put(RT_MANIFEST, 1, 1033, manifest)
    if not _end(handle, False):
        raise OSError(ctypes.get_last_error(), "EndUpdateResource")


def image_size(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    return struct.unpack_from("<I", data, pe + 24 + 56)[0]


def text_offset(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    header = pe + 24 + optional
    assert data[header:header + 5] == b".text"
    return struct.unpack_from("<I", data, header + 20)[0]


def run_azimuth_identity_tests(output, game_folder, result):
    archive = Path(game_folder) / "azimuth-windows-1.0.4.zip"
    dll = output / "core-test" / "winmm.dll"
    if not archive.is_file() or not dll.is_file():
        result("skip", f"branded Azimuth: needs {archive.name} in {game_folder} and core-test")
        return
    from core_on_game import map_image
    core = ctypes.WinDLL(str(dll))
    verify = core.Fh2TestAzimuthVerifyHost
    verify.argtypes, verify.restype = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p], ctypes.c_int
    process = core.Fh2TestAzimuthHostProcess
    process.argtypes, process.restype = [ctypes.c_wchar_p, ctypes.c_void_p], ctypes.c_int
    identity = core.Fh2TestAzimuthCodeIdentity
    identity.argtypes, identity.restype = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p], ctypes.c_int
    free = ctypes.windll.kernel32.VirtualFree
    free.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]

    def accepted(path, directory):
        base = map_image(path)
        try:
            return verify(base, str(path), str(directory)), process(path.name, base)
        finally:
            free(base, 0, 0x8000)

    runs = output / "test-runs"
    runs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="branded-azimuth-", dir=runs) as temporary:
        folder = Path(temporary)
        release = folder / "release" / "azimuth.exe"
        release.parent.mkdir()
        with zipfile.ZipFile(archive) as zip_file:
            release.write_bytes(zip_file.read("azimuth.exe"))
        original = release.read_bytes()
        expected = code_identity(original)
        digest = ctypes.create_string_buffer(32)
        result("ok" if identity(original, len(original), digest) and digest.raw == expected else "FAIL",
               "branded Azimuth: the core's code identity of the 1.0.4 release is tools/azimuth_identity.py's")
        result("ok" if expected.hex() == "156306d91d38bde1a13c48f80fe84a800afe5ddb4d0ae213830e9173db14e27e" else "FAIL",
               f"branded Azimuth: 1.0.4 code identity {expected.hex()[:16]}... is the host table's")
        ok, named = accepted(release, folder)
        result("ok" if ok and named else "FAIL", "branded Azimuth: the release azimuth.exe is accepted (as before)")

        cases = {}
        # Azimuth's own icons are 24420 bytes: a little less keeps .rsrc at its 7 pages.
        for label, icon in (("larger", 200_000), ("same-size", 23_000), ("smaller", 3_000)):
            copy = folder / label / "azimuth.exe"
            copy.parent.mkdir()
            shutil.copyfile(release, copy)
            brand(copy, "Forza Horizon 2", icon)
            data = copy.read_bytes()
            cases[label] = copy
            size_ok = {"larger": image_size(data) > image_size(original), "same-size": image_size(data) == image_size(original),
                       "smaller": image_size(data) < image_size(original)}[label]
            result("ok" if data != original and size_ok else "FAIL",
                   f"branded Azimuth ({label} .rsrc): SizeOfImage {image_size(original):#x} -> {image_size(data):#x}")
            result("ok" if code_identity(data) == expected else "FAIL",
                   f"branded Azimuth ({label} .rsrc): code identity unchanged")
            ok, named = accepted(copy, folder)
            result("ok" if ok and named else "FAIL", f"branded Azimuth ({label} .rsrc): accepted as azimuth.exe")
            renamed = copy.with_name("Forza Horizon 2.exe")
            shutil.copyfile(copy, renamed)
            ok, named = accepted(renamed, folder)
            result("ok" if ok and named else "FAIL", f"branded Azimuth ({label} .rsrc): accepted renamed, as {renamed.name}")

        changed = folder / "changed-code" / "Forza Horizon 2.exe"
        changed.parent.mkdir()
        data = bytearray(cases["larger"].read_bytes())
        data[text_offset(data) + 0x1000] ^= 0x01
        changed.write_bytes(bytes(data))
        ok, _ = accepted(changed, folder)
        result("ok" if not ok else "FAIL", "branded Azimuth: one changed .text byte is refused")
        log = folder / "forza-injector.log"
        result("ok" if log.is_file() and "unsupported Azimuth build" in log.read_text(encoding="utf-8", errors="replace")
               else "FAIL", "branded Azimuth: the refusal is logged")

        stranger = folder / "other" / "Forza Horizon 2.exe"
        stranger.parent.mkdir()
        shutil.copyfile(sys.executable, stranger)
        base = map_image(stranger)
        try:
            result("ok" if not process(stranger.name, base) and process("azimuth.exe", base) else "FAIL",
                   "branded Azimuth: another EXE loading winmm.dll is left alone unless named azimuth.exe")
        finally:
            free(base, 0, 0x8000)
