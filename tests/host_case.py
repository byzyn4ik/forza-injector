"""Run one isolated host initialization; game code is never executed."""
import ctypes
import hashlib
import json
from pathlib import Path
import struct
import sys

def main():
    dll, directory, title, game = sys.argv[1:5]
    patches = json.loads(Path(sys.argv[5]).read_text(encoding='utf-8'))['patches'] if len(sys.argv) > 5 else []
    if game != '-':
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from core_on_game import map_image
        base = map_image(game)
        size = struct.unpack('<I', ctypes.string_at(base + 0x3c, 4))[0]
        size = struct.unpack('<I', ctypes.string_at(base + size + 24 + 56, 4))[0]
        image = None
    else:
        size = 0x20000
        image = ctypes.create_string_buffer(size)
        image[0:2] = b'MZ'
        struct.pack_into('<I', image, 0x3c, 0x80)
        image[0x80:0x84] = b'PE\0\0'
        struct.pack_into('<I', image, 0x88, 0x12345678)
        struct.pack_into('<I', image, 0x80 + 24 + 56, size)
        base = ctypes.addressof(image)
    before = hashlib.sha256(ctypes.string_at(base, size)).hexdigest()
    core = ctypes.WinDLL(dll)
    # Azimuth's package reader accepts an arbitrary title and executable name.
    package = ctypes.create_string_buffer(0xe0)
    package_title = ctypes.create_unicode_buffer('DEADBEEF')
    package_exe = ctypes.create_unicode_buffer('C:\\test\\another_title.exe')
    for offset, value in ((0x28, package_title), (0xc0, package_exe)):
        length = len(value.value)
        struct.pack_into('<QQQQ', package, offset, ctypes.addressof(value), 0, length, length)
    package_reader = core.Fh2TestAzimuthPackage
    package_reader.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
    output_path = ctypes.create_unicode_buffer(32768)
    admitted = bool(package_reader(ctypes.addressof(package), output_path)) and output_path.value == package_exe.value
    init = core.ForzaTestInitialize
    init.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32]
    init.restype = ctypes.c_int
    ok = init(base, game if game != '-' else 'unrecognized_game.exe', directory, int(title, 16))
    core.ForzaTestWait.argtypes = [ctypes.c_uint32]
    waited = core.ForzaTestWait(10000)
    after = hashlib.sha256(ctypes.string_at(base, size)).hexdigest()
    log = Path(directory, 'forza-injector.log')
    print(json.dumps({'ok': ok, 'waited': waited, 'unchanged': before == after, 'package_agnostic': admitted,
        'provider': core.ForzaTestProviderResult(), 'plugins': core.ForzaTestPluginCount(),
        'log': log.read_text(encoding='utf-8') if log.exists() else '',
        'sites': {p['id']: ctypes.string_at(base + p['rva'], len(p['original']) // 2).hex() for p in patches}}))

if __name__ == '__main__':
    main()
