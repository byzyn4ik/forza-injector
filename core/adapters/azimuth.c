#include "azimuth_proxy.h"
#include "azimuth_host.h"
#include "azimuth_saves.h"
#include "logging.h"
#include "runtime.h"
#include "sha256.h"
#include "support.h"


/* hash: SHA-256 of the release's azimuth.exe (file_size bytes). code: its code
   identity (tools/azimuth_identity.py, host_code_identity below), the same for
   a copy whose resources were changed (icon, version details, manifest) and
   which may be renamed; all zero: only the release file is accepted. */
typedef struct {
    AzimuthBuild build;
    unsigned file_size, map_game, map_call, thunk, startup_return;
    unsigned char hash[32];
    const char *success;
    unsigned char code[32];
} HostProfile;
static const HostProfile host_profiles[] = {
    {AZIMUTH_101, 9692672u, 0x400e0u, 0xa027bu, 0x91170u, 0x9e950u,
     {0x6e,0x9a,0x22,0x90,0x97,0x15,0x00,0xa1,0x33,0x69,0x99,0x99,0xb3,0x02,0x4e,0xec,
      0x7b,0xe9,0x3c,0xf5,0xd4,0x11,0x6d,0x18,0xd5,0x4b,0x83,0x72,0xd1,0x11,0x72,0xf7},
     "AZIMUTH: 1.0.1 initialized for the mapped game; native game loader and TLS retained.\r\n",
     {0}},
    {AZIMUTH_102, 9715712u, 0x3f2e0u, 0x9f4ebu, 0x903e0u, 0x9dbc0u,
     {0x42,0x52,0x79,0x64,0xf4,0x8c,0x90,0xa5,0x37,0xf2,0x41,0x42,0x00,0xb8,0x59,0x8a,
      0xb9,0xa6,0xf9,0x29,0x19,0xbf,0xbe,0x04,0x6d,0xbe,0xcc,0xcb,0x36,0xe7,0xc5,0x97},
     "AZIMUTH: 1.0.2 initialized for the mapped game; native game loader and TLS retained.\r\n",
     {0x24,0xe9,0x8a,0x0e,0x7d,0x57,0x8d,0x2a,0xb3,0x60,0xdc,0xdf,0xa2,0x5f,0xab,0x6e,
      0xc2,0x4c,0xda,0x8c,0x10,0x98,0x0a,0x9a,0xd3,0xfc,0xf6,0xa8,0x97,0x6a,0x92,0xec}},
    {AZIMUTH_103, 9887232u, 0x3fd50u, 0xa697bu, 0xa0454u, 0xa5150u,
     {0xeb,0xfd,0x5d,0x43,0xa1,0x65,0xdc,0xec,0x5d,0x40,0x07,0xb9,0xd4,0x1e,0xd5,0xb2,0xed,0x12,0x2f,0xcd,0x02,0xec,0xfe,0xf7,0xe4,0xed,0x35,0xb4,0x08,0x26,0x82,0x1f},
     "AZIMUTH: 1.0.3 initialized for the mapped game; native game loader and TLS retained.\r\n",
     {0}},
    {AZIMUTH_104, 8613376u, 0x3f2c0u, 0xa684bu, 0xa1384u, 0xa4e09u,
     {0x56,0xd2,0xc9,0xcb,0x6c,0xcf,0x3a,0xe1,0x83,0x6d,0xee,0x96,0x76,0xb6,0x42,0x85,
      0xf3,0xa2,0x47,0xef,0xa2,0xd1,0x2c,0xf0,0x2c,0x6c,0x74,0x26,0x96,0xb3,0xb0,0x92},
     "AZIMUTH: 1.0.4 initialized for the mapped game; native game loader and TLS retained.\r\n",
     {0x15,0x63,0x06,0xd9,0x1d,0x38,0xbd,0xe1,0xa1,0x3c,0x48,0xf8,0x0f,0xe8,0x4a,0x80,
      0x0a,0xfe,0x5d,0xdb,0x4d,0x0a,0xe2,0x13,0x83,0x0e,0x91,0x73,0xdb,0x14,0xe2,0x7e}},
};
static const HostProfile *host_profile;
#define MAP_GAME (host_profile->map_game)
#define MAP_GAME_CALL (host_profile->map_call)
#define MAP_GAME_THUNK (host_profile->thunk)

typedef void *(*MapGame)(void *, const void *, const void *, const void *);
typedef int(WINAPI *MessageBoxFunction)(HWND, LPCWSTR, LPCWSTR, UINT);
static HINSTANCE mod_instance;
static unsigned char *host_image;
static volatile LONG initialized, handed_off;
static wchar_t mod_directory[PATH_CAPACITY], game_path[PATH_CAPACITY];
static wchar_t filename[PATH_CAPACITY];
static int notifications;
static unsigned char original_call[5];
static unsigned char installed_call[5], original_thunk[12], installed_thunk[12];
static unsigned char original_hashes[2][32], installed_hashes[2][32];

static void notify_refusal(void) {
    ++notifications;
#ifndef FH2_TWEAKS_TEST
    HMODULE user32 = LoadLibraryExW(L"user32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    MessageBoxFunction show =
        user32 ? (MessageBoxFunction)(uintptr_t)GetProcAddress(user32, "MessageBoxW") : NULL;
    if (show) {
        wchar_t message[1024];
        int length =
            MultiByteToWideChar(CP_UTF8, 0, status_get(), -1, message, ARRAY_COUNT(message));
        if (length) {
            show(NULL, message, L"forza_injector", MB_OK | MB_ICONWARNING);
        }
    }
    if (user32) {
        FreeLibrary(user32);
    }
#endif
}

static int refuse(const char *reason) {
    status_set(reason);
    log_set_directory(mod_directory);
    log_append(reason);
    log_append("\r\n");
    notify_refusal();
    return 0;
}

static void hash_bytes(const void *bytes, unsigned size, unsigned char *digest) {
    Sha256Context hash;
    sha256_initialize(&hash);
    sha256_update(&hash, bytes, size);
    sha256_finalize(&hash, digest);
}

static uint32_t read_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void put_u32(unsigned char *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
}

/* The code identity of a PE32+ file (tools/azimuth_identity.py): SHA-256 over
   every section but .rsrc - Name, VirtualSize, VirtualAddress (0 after .rsrc,
   which a larger .rsrc moves), Characteristics, SizeOfRawData, raw data - then
   AddressOfEntryPoint and ImageBase. Changing only resources leaves it as is. */
static int host_code_identity(const unsigned char *data, size_t size, unsigned char *digest) {
    if (!data || size < 0x40 || data[0] != 'M' || data[1] != 'Z') return 0;
    size_t pe = read_u32(data + 0x3c);
    if (pe > 4096 || pe + 24 + 112 > size || !memory_equal(data + pe, "PE\0\0", 4)) return 0;
    unsigned count = data[pe + 6] | (unsigned)data[pe + 7] << 8;
    size_t optional = pe + 24, optional_size = data[pe + 20] | (unsigned)data[pe + 21] << 8;
    if (optional_size < 112 || (data[optional] | (unsigned)data[optional + 1] << 8) != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return 0;
    size_t table = optional + optional_size;
    if (table + 40u * count > size) return 0;
    Sha256Context context;
    sha256_initialize(&context);
    int after_resources = 0;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char *header = data + table + 40u * i;
        if (memory_equal(header, ".rsrc\0\0\0", 8)) {
            after_resources = 1;
            continue;
        }
        uint32_t raw_size = read_u32(header + 16), raw = read_u32(header + 20);
        if (raw > size || raw_size > size - raw) return 0;
        unsigned char fields[24];
        memory_copy(fields, header, 8);
        put_u32(fields + 8, read_u32(header + 8));
        put_u32(fields + 12, after_resources ? 0 : read_u32(header + 12));
        put_u32(fields + 16, read_u32(header + 36));
        put_u32(fields + 20, raw_size);
        sha256_update(&context, fields, sizeof(fields));
        sha256_update(&context, data + raw, raw_size);
    }
    unsigned char tail[12];
    memory_copy(tail, data + optional + 16, 4);     /* AddressOfEntryPoint */
    memory_copy(tail + 4, data + optional + 24, 8); /* ImageBase */
    sha256_update(&context, tail, sizeof(tail));
    sha256_finalize(&context, digest);
    return 1;
}

static int has_code_identity(const HostProfile *profile) {
    for (unsigned i = 0; i < sizeof(profile->code); ++i) if (profile->code[i]) return 1;
    return 0;
}

#define HOST_FILE_CAPACITY (256u * 1024u * 1024u)

/* The release file (its size and SHA-256), or a copy with only its resources
   changed (the profile's code identity). Read once at startup. */
static int verify_host_file(const wchar_t *path, const HostProfile *profile) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size;
    unsigned char *data = NULL;
    DWORD count;
    uint64_t total = 0;
    int valid = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= HOST_FILE_CAPACITY;
    if (valid) valid = (data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart)) != NULL;
    while (valid && total < (uint64_t)size.QuadPart) {
        uint64_t left = (uint64_t)size.QuadPart - total;
        valid = ReadFile(file, data + total, (DWORD)(left > 0x100000u ? 0x100000u : left), &count, NULL) && count;
        if (valid) total += count;
    }
    CloseHandle(file);
    int accepted = 0;
    if (valid && total == (uint64_t)size.QuadPart) {
        unsigned char digest[32];
        if (total == profile->file_size) {
            hash_bytes(data, (unsigned)total, digest);
            accepted = memory_equal(digest, profile->hash, 32);
        }
        if (!accepted && has_code_identity(profile) && host_code_identity(data, (size_t)total, digest))
            accepted = memory_equal(digest, profile->code, 32);
    }
    if (data) HeapFree(GetProcessHeap(), 0, data);
    return accepted;
}

static int directory_from_module(HMODULE module, wchar_t *directory) {
    DWORD count = GetModuleFileNameW(module, directory, PATH_CAPACITY);
    if (!count || count >= PATH_CAPACITY) return 0;
    while (count && directory[count - 1] != L'\\' && directory[count - 1] != L'/') --count;
    directory[count] = 0;
    return count != 0;
}

/* The inspected package stores std::wstring members, including the parsed
   XboxLive TitleId (+28) and resolved executable path (+c0). Never infer the
   active title from the launcher's saved selection: --game may override it. */
static int package_text(const void *package, unsigned offset, wchar_t *out, size_t capacity) {
    struct { uintptr_t data[2]; size_t length, capacity; } value;
    const unsigned char *member = (const unsigned char *)package + offset;
    if (!package || !memory_read(member, &value, sizeof(value)) || !capacity ||
        value.length >= capacity || value.length > value.capacity) return 0;
    const void *text = value.capacity > 7 ? (void *)value.data[0] : member;
    if (!memory_read(text, out, (value.length + 1) * sizeof(wchar_t)) || out[value.length]) return 0;
    for (size_t i = 0; i < value.length; ++i) if (!out[i]) return 0;
    return 1;
}

static unsigned package_title(const void *package) {
    wchar_t title[9];
    if (!package_text(package, 0x28, title, ARRAY_COUNT(title)) || lstrlenW(title) != 8) return 0;
    unsigned id = 0;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned ch = title[i];
        if (ch >= '0' && ch <= '9') ch -= '0';
        else if (ch >= 'a' && ch <= 'f') ch = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') ch = ch - 'A' + 10;
        else return 0;
        id = (id << 4) | ch;
    }
    return id;
}

static int resolve_game_path(const void *package) {
    if (!package_text(package, 0xc0, filename, ARRAY_COUNT(filename))) return 0;
    DWORD size = GetFullPathNameW(filename, PATH_CAPACITY, game_path, NULL);
    return size && size < PATH_CAPACITY;
}

static int game_image_is_readable(unsigned char *base, uint32_t size) {
    uintptr_t cursor = (uintptr_t)base, end = cursor + size;
    if (end < cursor) {
        return 0;
    }
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION region;
        if (VirtualQuery((void *)cursor, &region, sizeof(region)) != sizeof(region) ||
            region.State != MEM_COMMIT || region.AllocationBase != base ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            return 0;
        }
        DWORD protection = region.Protect & 0xff;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
            protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) {
            return 0;
        }
        uintptr_t next = (uintptr_t)region.BaseAddress + region.RegionSize;
        if (next <= cursor) {
            return 0;
        }
        cursor = next;
    }
    return 1;
}

static int change_loader(int install) {
    const unsigned rvas[] = {MAP_GAME_THUNK, MAP_GAME_CALL};
    const unsigned sizes[] = {sizeof(original_thunk), sizeof(original_call)};
    const unsigned char *original[] = {original_thunk, original_call};
    const unsigned char *replacement[] = {installed_thunk, installed_call};
    HostCodePatch patches[2];
    HostPatchSet sets[2];
    for (unsigned i = 0; i < 2; ++i) {
        patches[i] = (HostCodePatch){0, sizes[i], install ? replacement[i] : original[i]};
        sets[i] = (HostPatchSet){rvas[i], sizes[i],
            install ? original_hashes[i] : installed_hashes[i],
            install ? installed_hashes[i] : original_hashes[i], &patches[i], 1};
    }
    return azimuth_patch_host_codes(host_image, sets, ARRAY_COUNT(sets)) > 0;
}

static void *on_map_game(void *output, const void *folder, const void *package, const void *options) {
    /* Restore the call and padding before entering native code. The mapper
       retains its own TLS reserve, imports, protection and error handling. */
    int first = !InterlockedExchange(&handed_off, 1);
    int restored = first && change_loader(0);
    void *result = ((MapGame)(host_image + MAP_GAME))(output, folder, package, options);
    DWORD error = GetLastError();
    if (!first) { SetLastError(error); return result; }
    unsigned title = package_title(package);
    if (!restored) {
        refuse("REFUSED: could not restore Azimuth's game-loading call. No game patches applied.");
    } else if (!resolve_game_path(package)) {
        refuse("REFUSED: cannot resolve the loaded executable path. No game patches applied.");
    } else {
        unsigned char *context = NULL, *game = NULL;
        IMAGE_DOS_HEADER dos;
        IMAGE_NT_HEADERS64 nt;
        int mapped = memory_read_pointer(result, (void **)&context) &&
                     memory_read_pointer(context, (void **)&game);
        if (!mapped ||
            !memory_read(game, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew < 64 || dos.e_lfanew > 4096 ||
            !memory_read(game + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            !nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.SizeOfImage > 0x80000000u ||
            !game_image_is_readable(game, nt.OptionalHeader.SizeOfImage)) {
            refuse("REFUSED: Azimuth's mapped game has invalid or unreadable PE headers. No game patches applied.");
        } else if (runtime_initialize_title(game, game_path, mod_directory, title)) {
            log_append(host_profile->success);
        } else {
            notify_refusal();
        }
    }
    SetLastError(error);
    return result;
}

static int arm_loader(unsigned char *host) {
    static const unsigned char loader[] = {0x40,0x53,0x56,0x57,0x41,0x56,0x48,0x81,0xec,0xa8,1,0,0};
    unsigned char actual[sizeof(loader)];
    if (!memory_read(host + MAP_GAME, actual, sizeof(actual)) ||
        !memory_equal(actual, loader, sizeof(loader)))
        return refuse("REFUSED: Azimuth's game loading code has changed. No patches applied.");
    host_image = host;
    handed_off = 0;
    for (unsigned i = 0; i < sizeof(original_thunk); ++i) original_thunk[i] = 0xcc;
    const unsigned char jump[] = {0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
    memory_copy(installed_thunk, jump, sizeof(jump));
    uintptr_t callback = (uintptr_t)on_map_game;
    memory_copy(installed_thunk + 2, &callback, 8);
    original_call[0] = installed_call[0] = 0xe8;
    int32_t native_delta = (int32_t)(MAP_GAME - MAP_GAME_CALL - 5);
    memory_copy(original_call + 1, &native_delta, 4);
    int32_t delta = (int32_t)(MAP_GAME_THUNK - MAP_GAME_CALL - 5);
    memory_copy(installed_call + 1, &delta, 4);
    hash_bytes(original_thunk, sizeof(original_thunk), original_hashes[0]);
    hash_bytes(original_call, sizeof(original_call), original_hashes[1]);
    hash_bytes(installed_thunk, sizeof(installed_thunk), installed_hashes[0]);
    hash_bytes(installed_call, sizeof(installed_call), installed_hashes[1]);
    if (!change_loader(1))
        return refuse("REFUSED: Azimuth's game loading handoff is unavailable or changed. No game patches applied.");
    status_set("Waiting for Azimuth to prepare the loaded game.");
    return 1;
}

static int verify_host(unsigned char *host, const wchar_t *path) {
    AzimuthBuild build = azimuth_host_build(host);
    for (unsigned i = 0; i < ARRAY_COUNT(host_profiles); ++i) {
        const HostProfile *profile = &host_profiles[i];
        if (profile->build == build && verify_host_file(path, profile)) {
            host_profile = profile;
            return 1;
        }
    }
    return refuse("REFUSED: unsupported Azimuth build. forza_injector " MOD_VERSION
                  " supports the inspected Azimuth 1.0.1, 1.0.2, 1.0.3 and 1.0.4 executables only "
                  "(1.0.2 and 1.0.4 also with changed resources: icon, version details, manifest). "
                  "No game or save compatibility patches applied.");
}

/* azimuth.exe, or a renamed copy (e.g. "<Game>.exe" with the game's icon and
   details): any other EXE that loads this winmm.dll is left alone, silently,
   unless its headers are those of an Azimuth build the core supports. */
static int host_process(const wchar_t *name, const unsigned char *host) {
    return !lstrcmpiW(name, L"azimuth.exe") || azimuth_host_build(host) != AZIMUTH_UNKNOWN;
}

void azimuth_proxy_start(void *caller) {
    if (InterlockedCompareExchange(&initialized, 1, 0)) return;
    DWORD length = GetModuleFileNameW(NULL, filename, PATH_CAPACITY);
    if (!length || length >= PATH_CAPACITY) return;
    const wchar_t *name = filename;
    for (const wchar_t *p = filename; *p; ++p) if (*p == L'\\' || *p == L'/') name = p + 1;
    unsigned char *host = (unsigned char *)GetModuleHandleW(NULL);
    if (!host_process(name, host)) return;
    if (!directory_from_module(mod_instance, mod_directory)) {
        refuse("REFUSED: cannot resolve the mod path. No patches applied.");
        return;
    }
    /* File fingerprint once at startup, never on a gameplay or per-file path. */
    if (!verify_host(host, filename)) return;
    if (caller != host + host_profile->startup_return) {
        refuse("REFUSED: unexpected Azimuth initialization order. No patches applied.");
        return;
    }
    arm_loader(host);
}

void adapter_process_attach(HINSTANCE instance) {
    mod_instance = instance;
    DisableThreadLibraryCalls(instance);
    // forza-injector.ini [Saves] Location=Documents: decided, prepared and
    // hooked here, before Azimuth's own code first opens its saves folder.
    saves_location_attach(instance);
}

#ifdef FH2_TWEAKS_TEST
__declspec(dllexport) int Fh2TestArmAzimuth(void *host, const wchar_t *path,
                                         const wchar_t *directory, const wchar_t *game) {
    (void)game;
    lstrcpyW(mod_directory, directory);
    return verify_host(host, path) && arm_loader(host);
}
__declspec(dllexport) int Fh2TestAzimuthNotifications(void) { return notifications; }
__declspec(dllexport) int Fh2TestAzimuthHostProcess(const wchar_t *name, const void *host) {
    return host_process(name, host);
}
__declspec(dllexport) int Fh2TestAzimuthVerifyHost(void *host, const wchar_t *path, const wchar_t *directory) {
    lstrcpyW(mod_directory, directory);
    host_profile = NULL;
    return verify_host(host, path);
}
__declspec(dllexport) int Fh2TestAzimuthCodeIdentity(const unsigned char *data, size_t size, unsigned char *digest) {
    return host_code_identity(data, size, digest);
}
__declspec(dllexport) int Fh2TestAzimuthPackage(const void *package, wchar_t *output) {
    if (!resolve_game_path(package)) return 0;
    lstrcpyW(output, game_path);
    return 1;
}
#endif
