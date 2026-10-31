"""Patch a branded Azimuth 1.0.4 exe (Forza Builder --exe-mode patch), in place:
  1. game starts directly (as with --skip-launcher); --show-launcher opens Azimuth's launcher;
  2. game window title = the game's name (stored in a new RCDATA resource AZ_TITLE);
  3. version info LegalCopyright = "(c) <AppxManifest PublisherDisplayName>"; Azimuth's copyright kept in Comments.
Usage: py -I tools/patch_azimuth.py <exe> <game folder with Mount\\AppxManifest.xml> [--check]
Every original byte is checked first; the memory layout (SizeOfImage, section addresses) and
.text outside the patched bytes must stay identical, else the file is left unchanged.
"""
import sys, struct, os, shutil, ctypes, re, html
from ctypes import wintypes
import pefile
sys.stdout.reconfigure(encoding='utf-8')

exe, game = sys.argv[1], sys.argv[2]
check_only = '--check' in sys.argv
manifest = open(os.path.join(game, 'Mount', 'AppxManifest.xml'), encoding='utf-8-sig').read()
title = re.search(r'<DisplayName>([^<]+)</DisplayName>', manifest).group(1).strip()
title = html.unescape(title)
publisher = html.unescape(re.search(r'<PublisherDisplayName>([^<]+)</PublisherDisplayName>', manifest).group(1).strip())
AZ_COPYRIGHT = 'Copyright © 2026 Almost There Games'

def u16(s): return s.encode('utf-16le')
CODE = [  # (rva, original, patched)
    (0xae94c, bytes.fromhex('32db'), bytes.fromhex('b301')),   # result starts true: skip launcher
    (0xae97c, bytes.fromhex('757e'), bytes.fromhex('747e')),   # once false, stop comparing
    (0xae9f8, bytes.fromhex('32db'), bytes.fromhex('b301')),   # argument differs: stays true
    (0xaea06, bytes.fromhex('b301'), bytes.fromhex('b300')),   # --show-launcher given: false
    (0x6db4b8, u16('--skip-launcher'), u16('--show-launcher')),
]
LEA = 0x70d65  # lea r8,[rip+d] = lpWindowName of the game window's CreateWindowExW
ORIG_TITLE = 0x6b9798  # L"Azimuth"
def lea_to(rva): return bytes.fromhex('4c8d05') + struct.pack('<i', rva - (LEA + 7))

def state(pe):
    m = pe.get_memory_mapped_image(); out = []
    for rva, a, b in CODE + [(LEA, lea_to(ORIG_TITLE), None)]:
        cur = m[rva:rva + len(a)]
        if cur == a: out.append('original')
        elif b is not None and cur == b: out.append('patched')
        elif b is None and cur[:3] == a[:3]: out.append('patched')
        else: out.append('UNKNOWN')
    return out

def layout(pe):
    return (pe.FILE_HEADER.TimeDateStamp, pe.OPTIONAL_HEADER.SizeOfImage,
            [(s.Name, s.VirtualAddress) for s in pe.sections],
            [(s.Name, s.Misc_VirtualSize) for s in pe.sections if not s.Name.startswith(b'.rsrc')])

def text_bytes(pe):
    t = [s for s in pe.sections if s.Name.startswith(b'.text')][0]; return t.get_data()

pe = pefile.PE(exe)
st = state(pe); print('code:', st)
if 'UNKNOWN' in st: sys.exit('unexpected bytes: not Azimuth 1.0.4; nothing changed')
if all(s == 'patched' for s in st): print('already patched'); sys.exit(0)
if any(s == 'patched' for s in st): sys.exit('partly patched; restore the exe first')
print('title:', title, '| copyright: ©', publisher)
if check_only: sys.exit(0)

# ---- version info: rebuild StringFileInfo, keep fixed info and VarFileInfo ----
def pad4(b): return b + b'\0' * ((4 - len(b) % 4) % 4)
def node(key, value=b'', children=b'', wtype=1, vlen=None):
    if vlen is None: vlen = len(value) // 2 if wtype == 1 else len(value)
    head = pad4(struct.pack('<HHH', 0, vlen, wtype) + u16(key) + b'\0\0')
    body = head + (pad4(value) + children if children else value)
    return pad4(struct.pack('<H', len(body)) + body[2:])
def strnode(k, v): return node(k, u16(v) + b'\0\0', wtype=1, vlen=len(v) + 1)

orig = {}; lang_cp = None; ver_lang = None; fixed = None; var = None
for fi in pe.FileInfo:
    for x in fi:
        if hasattr(x, 'StringTable'):
            for t in x.StringTable:
                lang_cp = t.LangID.decode() if isinstance(t.LangID, bytes) else t.LangID
                for k, v in t.entries.items(): orig[k.decode()] = v.decode()
        if hasattr(x, 'Var'):
            var = [(v.entry) for v in x.Var]
for e in pe.DIRECTORY_ENTRY_RESOURCE.entries:
    if e.id == 16:
        lang_entry = e.directory.entries[0].directory.entries[0]; ver_lang = lang_entry.id
        d = lang_entry.data.struct
        blob = pe.get_memory_mapped_image()[d.OffsetToData:d.OffsetToData + d.Size]
fixed = blob[40:40 + 52]  # VS_VERSIONINFO header (6) + key "VS_VERSION_INFO\0" (32) + pad (2) = 40
assert struct.unpack_from('<I', fixed)[0] == 0xFEEF04BD
strings = {}
for k, v in orig.items():
    strings[k] = v
    if k == 'InternalName': strings['LegalCopyright'] = '© ' + publisher
strings.setdefault('LegalCopyright', '© ' + publisher)
strings['Comments'] = orig.get('Comments', '') + ' Azimuth: ' + AZ_COPYRIGHT + '.'
table = node(lang_cp, children=b''.join(strnode(k, v) for k, v in strings.items()), vlen=0)
sfi = node('StringFileInfo', children=table, vlen=0)
translation = struct.pack('<HH', int(lang_cp[:4], 16), int(lang_cp[4:], 16))
vfi = node('VarFileInfo', children=node('Translation', translation, wtype=0), vlen=0)
version = node('VS_VERSION_INFO', fixed, sfi + vfi, wtype=0, vlen=52)

# ---- write resources on a copy with UpdateResourceW ----
work = exe + '.az-new'
shutil.copyfile(exe, work)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.BeginUpdateResourceW.restype = wintypes.HANDLE
k32.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
k32.UpdateResourceW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD, ctypes.c_void_p, wintypes.DWORD]
k32.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
title_blob = u16(title) + b'\0\0'
try:
    h = k32.BeginUpdateResourceW(work, False)
    if not h: raise OSError(ctypes.get_last_error())
    for typ, name, lang, data in ((16, 1, ver_lang, version), (10, 'AZ_TITLE', 0, title_blob)):
        buf = ctypes.create_string_buffer(data, len(data))
        nm = ctypes.c_wchar_p(name) if isinstance(name, str) else ctypes.c_void_p(name)
        if not k32.UpdateResourceW(h, ctypes.c_void_p(typ), nm, lang, buf, len(data)): raise OSError(ctypes.get_last_error())
    if not k32.EndUpdateResourceW(h, False): raise OSError(ctypes.get_last_error())

    new = pefile.PE(work)
    if layout(new) != layout(pe): raise RuntimeError('resource update changed the memory layout')
    if text_bytes(new) != text_bytes(pe): raise RuntimeError('resource update changed .text')
    title_rva = None
    for e in new.DIRECTORY_ENTRY_RESOURCE.entries:
        if e.id == 10:
            for n in e.directory.entries:
                if n.name is not None and str(n.name) == 'AZ_TITLE':
                    d = n.directory.entries[0].data.struct; title_rva = d.OffsetToData
    assert title_rva and new.get_memory_mapped_image()[title_rva:title_rva + len(title_blob)] == title_blob
    data = bytearray(new.__data__); off = new.get_offset_from_rva
    for rva, a, b in CODE + [(LEA, lea_to(ORIG_TITLE), lea_to(title_rva))]:
        assert bytes(data[off(rva):off(rva) + len(a)]) == a
        data[off(rva):off(rva) + len(b)] = b
    new.close()
    out = pefile.PE(data=bytes(data))
    out.OPTIONAL_HEADER.CheckSum = out.generate_checksum()
    final = out.write()
    chk = pefile.PE(data=final)
    assert layout(chk) == layout(pe) and all(s == 'patched' for s in state(chk))
    open(work, 'wb').write(final)
    pe.close()
    os.replace(work, exe)
    print('patched:', exe, '| title rva', hex(title_rva))
finally:
    if os.path.exists(work): os.remove(work)
