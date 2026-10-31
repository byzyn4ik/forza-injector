// Saves location (adapters/azimuth_saves.c): Azimuth's saves in Documents\My Games\<game>\Saves\<title>.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Extra Options 0.3.2-openbox's adapters/azimuth_saves.c ([Saves]
// InDocuments, verified in game on Azimuth 1.0.2), for every known title
// instead of Forza Horizon 2's only. See azimuth_saves.h.
//
// Why here and not in a plugin: Azimuth's wWinMain prepares the title's saves
// folder (moves older layouts there, refuses links) and its launcher reads,
// backs up and restores the saves before the game image is mapped, which is
// when the injector's plugins (even ForzaPluginEarlyInitialize) start. DllMain
// of winmm.dll, a static import of azimuth.exe, runs before any of that.
//
// Azimuth 1.0.1 to 1.0.4 keep a title's saves in
//   <exe folder>\data\titles\<title>\saves\<space>\<container>\revisions\<16 hex>\{data\<blob>..., commit}
// A revision is complete once it has its commit (written as "pending" and
// renamed); Azimuth uses the newest one that verifies and drops all but the
// newest two when it saves.
#include "azimuth_saves.h"
#include "azimuth_host.h"
#include "sha256.h"
#include "support.h"

#define ROOT_CAPACITY 2048u
#define COPY_CHUNK (1u << 20)
#define COMMIT_CAPACITY 65536u
#define MAX_BLOBS 128u
#define BLOB_NAME 256u
#define MAX_REVISIONS 4096u
#define KEEP_REVISIONS 2u

// ---------------------------------------------------------------- KERNEL32 (resolved, not imported)

typedef HANDLE(WINAPI *CreateFileWFn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL(WINAPI *CreateDirectoryWFn)(LPCWSTR, LPSECURITY_ATTRIBUTES);
typedef BOOL(WINAPI *PathOnlyFn)(LPCWSTR);
typedef DWORD(WINAPI *GetFileAttributesWFn)(LPCWSTR);
typedef BOOL(WINAPI *GetFileAttributesExWFn)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
typedef HANDLE(WINAPI *FindFirstFileWFn)(LPCWSTR, LPWIN32_FIND_DATAW);
typedef HANDLE(WINAPI *FindFirstFileExWFn)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
typedef BOOL(WINAPI *MoveFileExWFn)(LPCWSTR, LPCWSTR, DWORD);
typedef BOOL(WINAPI *CopyFileWFn)(LPCWSTR, LPCWSTR, BOOL);
typedef BOOL(WINAPI *GetDiskFreeSpaceExWFn)(LPCWSTR, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER);
typedef DWORD(WINAPI *GetFinalPathFn)(HANDLE, LPWSTR, DWORD, DWORD);
typedef BOOL(WINAPI *HandleInfoFn)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
typedef DWORD(WINAPI *GetProfileStringFn)(LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, DWORD, LPCWSTR);
typedef BOOL(WINAPI *WriteProfileStringFn)(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
typedef HINSTANCE(WINAPI *ShellExecuteWFn)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
typedef HRESULT(WINAPI *ParsingNameFn)(PCWSTR, void *, const void *, void **);
typedef LONG(WINAPI *RegOpenKeyExWFn)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LONG(WINAPI *RegQueryValueExWFn)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG(WINAPI *RegCloseKeyFn)(HKEY);
typedef DWORD(WINAPI *ExpandFn)(LPCWSTR, LPWSTR, DWORD);
typedef DWORD(WINAPI *GetEnvironmentFn)(LPCWSTR, LPWSTR, DWORD);
typedef BOOL(WINAPI *SetFileTimeFn)(HANDLE, const FILETIME *, const FILETIME *, const FILETIME *);
typedef BOOL(WINAPI *GetFileTimeFn)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME);

enum {
    API_CREATE_FILE,
    API_CREATE_DIRECTORY,
    API_REMOVE_DIRECTORY,
    API_DELETE_FILE,
    API_MOVE_FILE,
    API_COPY_FILE,
    API_GET_ATTRIBUTES,
    API_GET_ATTRIBUTES_EX,
    API_FIND_FIRST,
    API_FIND_FIRST_EX,
    API_FINAL_PATH,
    API_SET_INFO,
    API_GET_INFO,
    API_DISK_FREE,
    API_GET_PROFILE,
    API_WRITE_PROFILE,
    API_SHELL_EXECUTE,
    API_PARSING_NAME,
    API_COUNT
};

static const char *const api_names[API_COUNT] = {
    "CreateFileW",          "CreateDirectoryW",          "RemoveDirectoryW",
    "DeleteFileW",          "MoveFileExW",               "CopyFileW",
    "GetFileAttributesW",   "GetFileAttributesExW",      "FindFirstFileW",
    "FindFirstFileExW",     "GetFinalPathNameByHandleW", "SetFileInformationByHandle",
    "GetFileInformationByHandleEx", "GetDiskFreeSpaceExW", "GetPrivateProfileStringW",
    "WritePrivateProfileStringW", "ShellExecuteW", "SHCreateItemFromParsingName"};

static void *originals[API_COUNT];
static struct {
    RegOpenKeyExWFn open_key;
    RegQueryValueExWFn query_value;
    RegCloseKeyFn close_key;
    ExpandFn expand;
    GetEnvironmentFn environment;
    SetFileTimeFn set_file_time;
    GetFileTimeFn get_file_time;
} k32;

#define ORIGINAL(api, type) ((type)originals[api])
#define CREATE_DIRECTORY(path) ORIGINAL(API_CREATE_DIRECTORY, CreateDirectoryWFn)(path, NULL)
#define REMOVE_DIRECTORY(path) ORIGINAL(API_REMOVE_DIRECTORY, PathOnlyFn)(path)
#define FILE_ATTRIBUTES_EX(path, data) \
    ORIGINAL(API_GET_ATTRIBUTES_EX, GetFileAttributesExWFn)(path, GetFileExInfoStandard, data)

static FARPROC kernel32_function(const char *name) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    return kernel32 ? GetProcAddress(kernel32, name) : NULL;
}

static FARPROC shell32_function(const char *name) {
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");  // only when Azimuth (or anyone) loaded it
    return shell32 ? GetProcAddress(shell32, name) : NULL;
}

// 1 when every KERNEL32 function this file needs is there (the SHELL32 ones are optional).
static int resolve_functions(void) {
    for (unsigned i = 0; i < API_COUNT; ++i) {
        FARPROC function = i >= API_SHELL_EXECUTE ? shell32_function(api_names[i]) : kernel32_function(api_names[i]);
        originals[i] = (void *)(uintptr_t)function;
        if (!function && i < API_SHELL_EXECUTE) return 0;
    }
    k32.open_key = (RegOpenKeyExWFn)(uintptr_t)kernel32_function("RegOpenKeyExW");
    k32.query_value = (RegQueryValueExWFn)(uintptr_t)kernel32_function("RegQueryValueExW");
    k32.close_key = (RegCloseKeyFn)(uintptr_t)kernel32_function("RegCloseKey");
    k32.expand = (ExpandFn)(uintptr_t)kernel32_function("ExpandEnvironmentStringsW");
    k32.environment = (GetEnvironmentFn)(uintptr_t)kernel32_function("GetEnvironmentVariableW");
    k32.set_file_time = (SetFileTimeFn)(uintptr_t)kernel32_function("SetFileTime");
    k32.get_file_time = (GetFileTimeFn)(uintptr_t)kernel32_function("GetFileTime");
    return k32.expand && k32.environment && k32.set_file_time && k32.get_file_time;
}

// ---------------------------------------------------------------- the titles

typedef struct {
    const wchar_t *title;  // Xbox title id: data\titles\<title>\saves
    const wchar_t *name;   // Documents\My Games\<name>\Saves\<title> (profiles/*.json "name")
    wchar_t game[ROOT_CAPACITY], docs[ROOT_CAPACITY];
    size_t game_length, docs_length;
    int present, redirected;
} TitleRoot;

// Keep in step with profiles/*.json (tests/saves_tests.py checks it). Other
// titles keep their saves where Azimuth puts them.
static TitleRoot titles[] = {
    {L"1A96545B", L"Forza Horizon 2", {0}, {0}, 0, 0, 0, 0},
    {L"5A9771A4", L"Forza Horizon 2 Presents Fast & Furious", {0}, {0}, 0, 0, 0, 0},
};

static volatile LONG active;
static volatile LONG first_use[API_COUNT];
static volatile LONG first_redirect, first_reverse;
static wchar_t documents_base[ROOT_CAPACITY];
static wchar_t launcher_game[16];  // data\azimuth.ini [launcher] game: the title Azimuth starts

// ---------------------------------------------------------------- text and paths

static size_t wide_length(const wchar_t *text) {
    size_t length = 0;
    while (text && text[length]) ++length;
    return length;
}

static wchar_t fold(wchar_t c) {
    return c >= L'a' && c <= L'z' ? (wchar_t)(c - 32) : c == L'/' ? L'\\' : c;
}

// text[0..length) starts with root (case-insensitive, / = \) at a component boundary.
static int root_prefix(const wchar_t *text, size_t length, const wchar_t *root, size_t root_length) {
    if (length < root_length || !root_length) return 0;
    for (size_t i = 0; i < root_length; ++i)
        if (fold(text[i]) != fold(root[i])) return 0;
    return length == root_length || text[root_length] == L'\\' || text[root_length] == L'/';
}

static int contains_folded(const wchar_t *text, const wchar_t *needle) {
    size_t needle_length = wide_length(needle);
    for (; *text; ++text) {
        size_t i = 0;
        while (i < needle_length && text[i] && fold(text[i]) == fold(needle[i])) ++i;
        if (i == needle_length) return 1;
    }
    return 0;
}

static int starts_folded(const wchar_t *text, const wchar_t *prefix) {
    for (; *prefix; ++text, ++prefix)
        if (fold(*text) != fold(*prefix)) return 0;
    return 1;
}

static int equal_folded(const wchar_t *a, const wchar_t *b) {
    for (; *a && *b; ++a, ++b)
        if (fold(*a) != fold(*b)) return 0;
    return *a == *b;
}

// path = text; its length, 0 when it does not fit.
static size_t path_set(wchar_t *path, const wchar_t *text) {
    size_t length = wide_length(text);
    if (!length || length >= ROOT_CAPACITY) return 0;
    memory_copy(path, text, (length + 1) * sizeof(wchar_t));
    return length;
}

// path[0..length) + "\" + name; the new length, 0 when length is 0 or it does not fit.
static size_t path_add(wchar_t *path, size_t length, const wchar_t *name) {
    size_t extra = wide_length(name);
    if (!length || length + 1 + extra + 1 > ROOT_CAPACITY) return 0;
    if (path[length - 1] != L'\\') path[length++] = L'\\';
    memory_copy(path + length, name, (extra + 1) * sizeof(wchar_t));
    return length + extra;
}

// path[0..length) + text (no separator).
static size_t path_append(wchar_t *path, size_t length, const wchar_t *text) {
    size_t extra = wide_length(text);
    if (!length || length + extra + 1 > ROOT_CAPACITY) return 0;
    memory_copy(path + length, text, (extra + 1) * sizeof(wchar_t));
    return length + extra;
}

static wchar_t *heap_text(size_t characters) {
    return (wchar_t *)HeapAlloc(GetProcessHeap(), 0, characters * sizeof(wchar_t));
}

static void heap_release(void *memory) {
    if (memory) {
        DWORD error = GetLastError();
        HeapFree(GetProcessHeap(), 0, memory);
        SetLastError(error);
    }
}

// ---------------------------------------------------------------- log
// Kept in memory from DllMain and written to forza-injector.log at once (the
// launcher may run without a game), then again after the core restarts the
// log when the game is mapped.

static SRWLOCK log_lock = SRWLOCK_INIT;
static wchar_t log_path[ROOT_CAPACITY];
static char pending[16384];
static size_t pending_length;
static int pending_dropped, log_live, log_restarted;

static void write_log_file(const char *text, size_t length, DWORD disposition) {
    if (!log_path[0]) return;
    HANDLE file = CreateFileW(log_path, disposition == CREATE_ALWAYS ? GENERIC_WRITE : FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    if (length) WriteFile(file, text, (DWORD)length, &written, NULL);
    CloseHandle(file);
}

static void saves_log(const char *text) {
    DWORD error = GetLastError();
    size_t length = text_length(text);
    AcquireSRWLockExclusive(&log_lock);
    if (!log_restarted) {
        if (pending_length + length < sizeof(pending)) {
            memory_copy(pending + pending_length, text, length);
            pending_length += length;
        } else {
            pending_dropped = 1;
        }
    }
    if (log_live) write_log_file(text, length, OPEN_ALWAYS);
    ReleaseSRWLockExclusive(&log_lock);
    SetLastError(error);
}

// After DllMain's work: a fresh log holding what was kept.
static void log_start(void) {
    AcquireSRWLockExclusive(&log_lock);
    if (pending_length) write_log_file(pending, pending_length, CREATE_ALWAYS);
    log_live = 1;
    ReleaseSRWLockExclusive(&log_lock);
}

void saves_location_log_ready(void) {
    AcquireSRWLockExclusive(&log_lock);
    if (pending_length && !log_restarted) {
        write_log_file(pending, pending_length, OPEN_ALWAYS);
        if (pending_dropped) {
            static const char dropped[] = "SAVES: some early save-location log lines were dropped.\r\n";
            write_log_file(dropped, sizeof(dropped) - 1, OPEN_ALWAYS);
        }
    }
    log_restarted = 1;
    ReleaseSRWLockExclusive(&log_lock);
}

typedef struct {
    char text[1536];
    size_t length;
} Line;

static void line_text(Line *line, const char *text) {
    while (*text && line->length + 1 < sizeof(line->text)) line->text[line->length++] = *text++;
    line->text[line->length] = 0;
}

static void line_path(Line *line, const wchar_t *path) {
    if (!path) {
        line_text(line, "(null)");
        return;
    }
    int room = (int)(sizeof(line->text) - line->length - 1);
    int written = room > 0 ? WideCharToMultiByte(CP_UTF8, 0, path, (int)wide_length(path), line->text + line->length,
                                                 room, NULL, NULL)
                           : 0;
    if (written > 0) line->length += (size_t)written;
    line->text[line->length] = 0;
}

static void line_number(Line *line, unsigned long long value) {
    char digits[24];
    char *cursor = digits + sizeof(digits) - 1;
    *cursor = 0;
    do {
        *--cursor = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    line_text(line, cursor);
}

static void line_start(Line *line, const char *text) {
    line->length = 0;
    line->text[0] = 0;
    line_text(line, text);
}

static void line_end(Line *line) {
    line_text(line, "\r\n");
    saves_log(line->text);
}

static void log_path_line(const char *text, const wchar_t *path, const char *after) {
    Line line;
    line_start(&line, text);
    line_path(&line, path);
    line_text(&line, after);
    line_end(&line);
}

// ---------------------------------------------------------------- path mapping

static void note_redirect(unsigned api, const wchar_t *from, const wchar_t *to, int reverse) {
    volatile LONG *first = reverse ? &first_reverse : &first_redirect;
    int api_first = api < API_COUNT && !InterlockedExchange(&first_use[api], 1);
    int overall_first = !InterlockedExchange(first, 1);
    if (!api_first && !overall_first) return;
    Line line;
    line_start(&line, overall_first ? (reverse ? "SAVES: first path mapped back for Azimuth ("
                                               : "SAVES: first redirected path (")
                                    : "SAVES: first redirect through ");
    line_text(&line, api < API_COUNT ? api_names[api] : "injector");
    line_text(&line, overall_first ? "): " : ": ");
    line_path(&line, from);
    line_text(&line, " -> ");
    line_path(&line, to);
    line_end(&line);
}

// A heap copy of path rewritten to the Documents folder, or NULL when it is not
// a redirected title's saves folder or below it. Keeps a \\?\ or \??\ prefix.
static wchar_t *map_forward(const wchar_t *path, unsigned api) {
    if (!active || !path || !contains_folded(path, L"saves")) return NULL;  // every root ends in \saves
    DWORD error = GetLastError();
    size_t prefix = 0;
    if (path[0] == L'\\' && (path[1] == L'\\' || path[1] == L'?') && path[2] == L'?' && path[3] == L'\\') {
        prefix = 4;
        if (starts_folded(path + 4, L"UNC\\")) return NULL;
    } else if ((path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/')) {
        return NULL;  // a share or a \\.\ device
    }
    wchar_t *full = NULL;
    const wchar_t *normal = path + prefix;
    if (!prefix) {
        full = heap_text(PATH_CAPACITY);
        DWORD count = full ? GetFullPathNameW(path, PATH_CAPACITY, full, NULL) : 0;
        if (!count || count >= PATH_CAPACITY) {
            heap_release(full);
            SetLastError(error);
            return NULL;
        }
        normal = full;
    }
    size_t normal_length = wide_length(normal);
    wchar_t *mapped = NULL;
    for (unsigned t = 0; t < ARRAY_COUNT(titles) && !mapped; ++t) {
        const TitleRoot *root = &titles[t];
        if (!root->redirected || !root_prefix(normal, normal_length, root->game, root->game_length)) continue;
        const wchar_t *tail = normal + root->game_length;
        size_t tail_length = normal_length - root->game_length;
        int long_form = !prefix && root->docs_length + tail_length >= 248;
        mapped = heap_text(4 + root->docs_length + tail_length + 1);
        if (!mapped) break;
        size_t at = 0;
        if (prefix) {
            for (; at < 4; ++at) mapped[at] = path[at];
        } else if (long_form) {
            mapped[at++] = L'\\', mapped[at++] = L'\\', mapped[at++] = L'?', mapped[at++] = L'\\';
        }
        for (size_t i = 0; i < root->docs_length; ++i) mapped[at++] = root->docs[i];
        for (size_t i = 0; i < tail_length; ++i) mapped[at++] = tail[i] == L'/' ? L'\\' : tail[i];
        mapped[at] = 0;
    }
    heap_release(full);
    if (mapped) note_redirect(api, path, mapped, 0);
    SetLastError(error);
    return mapped;
}

// A Documents path Windows returns, mapped back to the game-folder path. DOS
// (\\?\C:\..., C:\...), volume-relative (\Users\...), volume GUID and \Device\
// forms. The new length, 0 when unchanged or too long.
static size_t map_reverse_one(const TitleRoot *root, const wchar_t *text, size_t length, wchar_t *output,
                              size_t capacity) {
    size_t q = 0, at;
    if (length >= 4 && text[0] == L'\\' && (text[1] == L'\\' || text[1] == L'?') && text[2] == L'?' && text[3] == L'\\')
        q = 4;
    const wchar_t *match, *replacement;
    size_t match_length, replacement_length;
    int same_drive = fold(root->game[0]) == fold(root->docs[0]) && root->game[1] == L':' && root->docs[1] == L':';
    if (length >= q + 2 && text[q + 1] == L':') {
        at = q;
        match = root->docs, match_length = root->docs_length;
        replacement = root->game, replacement_length = root->game_length;
    } else if (same_drive) {
        match = root->docs + 2, match_length = root->docs_length - 2;
        replacement = root->game + 2, replacement_length = root->game_length - 2;
        if (!q && text[0] == L'\\' && starts_folded(text, L"\\Device\\")) {
            at = 8;
            while (at < length && text[at] != L'\\') ++at;
        } else if (!q && text[0] == L'\\') {
            at = 0;
        } else if (q && starts_folded(text + 4, L"Volume{")) {
            at = 4;
            while (at < length && text[at] != L'}') ++at;
            if (at < length) ++at;
        } else {
            return 0;
        }
    } else {
        return 0;
    }
    if (!root_prefix(text + at, length - at, match, match_length)) return 0;
    size_t tail = length - at - match_length, total = at + replacement_length + tail;
    if (total + 1 > capacity) return 0;
    for (size_t i = 0; i < at; ++i) output[i] = text[i];
    for (size_t i = 0; i < replacement_length; ++i) output[at + i] = replacement[i];
    for (size_t i = 0; i < tail; ++i) output[at + replacement_length + i] = text[at + match_length + i];
    output[total] = 0;
    return total;
}

static size_t map_reverse(const wchar_t *text, size_t length, wchar_t *output, size_t capacity) {
    if (!active || !length) return 0;
    for (unsigned t = 0; t < ARRAY_COUNT(titles); ++t) {
        if (!titles[t].redirected) continue;
        size_t mapped = map_reverse_one(&titles[t], text, length, output, capacity);
        if (mapped) return mapped;
    }
    return 0;
}

static size_t longest_game_root(void) {
    size_t longest = 0;
    for (unsigned t = 0; t < ARRAY_COUNT(titles); ++t)
        if (titles[t].game_length > longest) longest = titles[t].game_length;
    return longest;
}

// ---------------------------------------------------------------- hooks

#define PATH_OR(mapped, path) ((mapped) ? (const wchar_t *)(mapped) : (path))

static int make_folders(const wchar_t *path, size_t trusted_length);

static HANDLE WINAPI hook_create_file(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                      DWORD disposition, DWORD flags, HANDLE template_file) {
    wchar_t *mapped = map_forward(path, API_CREATE_FILE);
    HANDLE result = ORIGINAL(API_CREATE_FILE, CreateFileWFn)(PATH_OR(mapped, path), access, share, security,
                                                            disposition, flags, template_file);
    heap_release(mapped);
    return result;
}

// Is mapped exactly a title's Documents folder? Then its parents (My Games\<name>\Saves) are made first.
static void ensure_parents(const wchar_t *mapped) {
    const wchar_t *plain = mapped;
    if (plain[0] == L'\\' && plain[1] == L'\\' && plain[2] == L'?' && plain[3] == L'\\') plain += 4;
    for (unsigned t = 0; t < ARRAY_COUNT(titles); ++t) {
        if (!titles[t].redirected || !equal_folded(plain, titles[t].docs)) continue;
        static wchar_t parent[ROOT_CAPACITY];
        size_t length = path_set(parent, titles[t].docs);
        while (length && parent[length - 1] != L'\\') --length;
        if (length > 3) {
            parent[length - 1] = 0;
            DWORD error = GetLastError();
            make_folders(parent, wide_length(documents_base));
            SetLastError(error);
        }
    }
}

static BOOL WINAPI hook_create_directory(LPCWSTR path, LPSECURITY_ATTRIBUTES security) {
    wchar_t *mapped = map_forward(path, API_CREATE_DIRECTORY);
    if (mapped) ensure_parents(mapped);
    BOOL result = ORIGINAL(API_CREATE_DIRECTORY, CreateDirectoryWFn)(PATH_OR(mapped, path), security);
    heap_release(mapped);
    return result;
}

static BOOL WINAPI hook_remove_directory(LPCWSTR path) {
    wchar_t *mapped = map_forward(path, API_REMOVE_DIRECTORY);
    BOOL result = ORIGINAL(API_REMOVE_DIRECTORY, PathOnlyFn)(PATH_OR(mapped, path));
    heap_release(mapped);
    return result;
}

static BOOL WINAPI hook_delete_file(LPCWSTR path) {
    wchar_t *mapped = map_forward(path, API_DELETE_FILE);
    BOOL result = ORIGINAL(API_DELETE_FILE, PathOnlyFn)(PATH_OR(mapped, path));
    heap_release(mapped);
    return result;
}

static BOOL WINAPI hook_move_file(LPCWSTR from, LPCWSTR to, DWORD flags) {
    wchar_t *mapped_from = map_forward(from, API_MOVE_FILE);
    wchar_t *mapped_to = map_forward(to, API_MOVE_FILE);
    // One side moved (maybe to another volume): let Windows copy across it.
    if (!mapped_from != !mapped_to && to) flags |= MOVEFILE_COPY_ALLOWED;
    BOOL result = ORIGINAL(API_MOVE_FILE, MoveFileExWFn)(PATH_OR(mapped_from, from), PATH_OR(mapped_to, to), flags);
    heap_release(mapped_from);
    heap_release(mapped_to);
    return result;
}

static BOOL WINAPI hook_copy_file(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists) {
    wchar_t *mapped_from = map_forward(from, API_COPY_FILE);
    wchar_t *mapped_to = map_forward(to, API_COPY_FILE);
    BOOL result = ORIGINAL(API_COPY_FILE, CopyFileWFn)(PATH_OR(mapped_from, from), PATH_OR(mapped_to, to),
                                                      fail_if_exists);
    heap_release(mapped_from);
    heap_release(mapped_to);
    return result;
}

static DWORD WINAPI hook_get_attributes(LPCWSTR path) {
    wchar_t *mapped = map_forward(path, API_GET_ATTRIBUTES);
    DWORD result = ORIGINAL(API_GET_ATTRIBUTES, GetFileAttributesWFn)(PATH_OR(mapped, path));
    heap_release(mapped);
    return result;
}

static BOOL WINAPI hook_get_attributes_ex(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID data) {
    wchar_t *mapped = map_forward(path, API_GET_ATTRIBUTES_EX);
    BOOL result = ORIGINAL(API_GET_ATTRIBUTES_EX, GetFileAttributesExWFn)(PATH_OR(mapped, path), level, data);
    heap_release(mapped);
    return result;
}

static HANDLE WINAPI hook_find_first(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    wchar_t *mapped = map_forward(path, API_FIND_FIRST);
    HANDLE result = ORIGINAL(API_FIND_FIRST, FindFirstFileWFn)(PATH_OR(mapped, path), data);
    heap_release(mapped);
    return result;
}

static HANDLE WINAPI hook_find_first_ex(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search,
                                        LPVOID filter, DWORD flags) {
    wchar_t *mapped = map_forward(path, API_FIND_FIRST_EX);
    HANDLE result =
        ORIGINAL(API_FIND_FIRST_EX, FindFirstFileExWFn)(PATH_OR(mapped, path), level, data, search, filter, flags);
    heap_release(mapped);
    return result;
}

static BOOL WINAPI hook_disk_free(LPCWSTR path, PULARGE_INTEGER available, PULARGE_INTEGER total,
                                  PULARGE_INTEGER free_bytes) {
    wchar_t *mapped = map_forward(path, API_DISK_FREE);
    BOOL result = ORIGINAL(API_DISK_FREE, GetDiskFreeSpaceExWFn)(PATH_OR(mapped, path), available, total, free_bytes);
    heap_release(mapped);
    return result;
}

static DWORD WINAPI hook_get_profile(LPCWSTR section, LPCWSTR key, LPCWSTR fallback, LPWSTR output, DWORD size,
                                     LPCWSTR file) {
    wchar_t *mapped = map_forward(file, API_GET_PROFILE);
    DWORD result =
        ORIGINAL(API_GET_PROFILE, GetProfileStringFn)(section, key, fallback, output, size, PATH_OR(mapped, file));
    heap_release(mapped);
    return result;
}

static BOOL WINAPI hook_write_profile(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR file) {
    wchar_t *mapped = map_forward(file, API_WRITE_PROFILE);
    BOOL result = ORIGINAL(API_WRITE_PROFILE, WriteProfileStringFn)(section, key, value, PATH_OR(mapped, file));
    heap_release(mapped);
    return result;
}

// The launcher's "open folder": Explorer opens the folder the saves are in.
static HINSTANCE WINAPI hook_shell_execute(HWND window, LPCWSTR operation, LPCWSTR file, LPCWSTR parameters,
                                           LPCWSTR directory, INT show) {
    wchar_t *mapped_file = map_forward(file, API_SHELL_EXECUTE);
    wchar_t *mapped_directory = map_forward(directory, API_SHELL_EXECUTE);
    HINSTANCE result = ORIGINAL(API_SHELL_EXECUTE, ShellExecuteWFn)(
        window, operation, PATH_OR(mapped_file, file), parameters, PATH_OR(mapped_directory, directory), show);
    heap_release(mapped_file);
    heap_release(mapped_directory);
    return result;
}

static HRESULT WINAPI hook_parsing_name(PCWSTR path, void *context, const void *interface_id, void **item) {
    wchar_t *mapped = map_forward(path, API_PARSING_NAME);
    HRESULT result = ORIGINAL(API_PARSING_NAME, ParsingNameFn)(PATH_OR(mapped, path), context, interface_id, item);
    heap_release(mapped);
    return result;
}

// The final path of a handle opened in the Documents folder reads as the
// game-folder path, so Azimuth's own comparisons still match.
static DWORD WINAPI hook_final_path(HANDLE file, LPWSTR output, DWORD capacity, DWORD flags) {
    GetFinalPathFn original = ORIGINAL(API_FINAL_PATH, GetFinalPathFn);
    if (!active) return original(file, output, capacity, flags);
    wchar_t local[1024];
    wchar_t *buffer = local, *heap = NULL;
    DWORD buffer_capacity = ARRAY_COUNT(local);
    DWORD result = original(file, buffer, buffer_capacity, flags);
    if (result >= buffer_capacity && result < PATH_CAPACITY) {
        heap = heap_text(result + 1);
        if (!heap) return original(file, output, capacity, flags);
        buffer = heap, buffer_capacity = result + 1;
        result = original(file, buffer, buffer_capacity, flags);
    }
    if (!result || result >= buffer_capacity) {
        heap_release(heap);
        return original(file, output, capacity, flags);  // Windows' own answer
    }
    DWORD error = GetLastError();
    size_t mapped_capacity = result + longest_game_root() + 8;
    wchar_t *mapped = heap_text(mapped_capacity);
    size_t mapped_length = mapped ? map_reverse(buffer, result, mapped, mapped_capacity) : 0;
    const wchar_t *answer_text = buffer;
    size_t answer_length = result;
    if (mapped_length) {
        note_redirect(API_FINAL_PATH, buffer, mapped, 1);
        answer_text = mapped, answer_length = mapped_length;
    }
    DWORD answer;
    if (answer_length < capacity && output) {
        memory_copy(output, answer_text, (answer_length + 1) * sizeof(wchar_t));
        answer = (DWORD)answer_length;
    } else {
        answer = (DWORD)answer_length + 1;  // the size needed, as Windows reports it
    }
    heap_release(mapped);
    heap_release(heap);
    SetLastError(error);
    return answer;
}

static BOOL WINAPI hook_get_info(HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID data, DWORD size) {
    BOOL result = ORIGINAL(API_GET_INFO, HandleInfoFn)(file, kind, data, size);
    // FileNameInfo (2) and FileNormalizedNameInfo (24): volume-relative names.
    if (!result || !active || ((unsigned)kind != 2 && (unsigned)kind != 24) || !data ||
        size < offsetof(FILE_NAME_INFO, FileName))
        return result;
    DWORD error = GetLastError();
    FILE_NAME_INFO *info = (FILE_NAME_INFO *)data;
    size_t room = (size - offsetof(FILE_NAME_INFO, FileName)) / sizeof(wchar_t);
    size_t length = info->FileNameLength / sizeof(wchar_t);
    if (length <= room && length < PATH_CAPACITY) {
        size_t capacity = length + longest_game_root() + 8;
        wchar_t *mapped = heap_text(capacity);
        size_t mapped_length = mapped ? map_reverse(info->FileName, length, mapped, capacity) : 0;
        if (mapped_length && mapped_length <= room) {
            note_redirect(API_GET_INFO, NULL, mapped, 1);
            memory_copy(info->FileName, mapped, mapped_length * sizeof(wchar_t));
            if (mapped_length < room) info->FileName[mapped_length] = 0;
            info->FileNameLength = (DWORD)(mapped_length * sizeof(wchar_t));
        }
        heap_release(mapped);
    }
    SetLastError(error);
    return result;
}

// FileRenameInfo (3) and FileRenameInfoEx (22) carry the target path.
static BOOL WINAPI hook_set_info(HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID data, DWORD size) {
    HandleInfoFn original = ORIGINAL(API_SET_INFO, HandleInfoFn);
    const size_t header = offsetof(FILE_RENAME_INFO, FileName);
    if (!active || ((unsigned)kind != 3 && (unsigned)kind != 22) || !data || size < header)
        return original(file, kind, data, size);
    FILE_RENAME_INFO *info = (FILE_RENAME_INFO *)data;
    size_t length = info->FileNameLength / sizeof(wchar_t);
    if (info->RootDirectory || header + info->FileNameLength > size || length >= PATH_CAPACITY)
        return original(file, kind, data, size);
    wchar_t *name = heap_text(length + 1);
    if (!name) return original(file, kind, data, size);
    memory_copy(name, info->FileName, length * sizeof(wchar_t));
    name[length] = 0;
    wchar_t *mapped = map_forward(name, API_SET_INFO);
    heap_release(name);
    if (!mapped) return original(file, kind, data, size);
    size_t mapped_length = wide_length(mapped);
    DWORD new_size = (DWORD)(header + (mapped_length + 1) * sizeof(wchar_t));
    unsigned char *copy = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, new_size);
    if (!copy) {
        heap_release(mapped);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    memory_copy(copy, data, header);
    FILE_RENAME_INFO *renamed = (FILE_RENAME_INFO *)copy;
    renamed->FileNameLength = (DWORD)(mapped_length * sizeof(wchar_t));
    memory_copy(renamed->FileName, mapped, (mapped_length + 1) * sizeof(wchar_t));
    BOOL result = original(file, kind, copy, new_size);
    heap_release(copy);
    heap_release(mapped);
    return result;
}

typedef struct {
    unsigned api;
    const char *module;
    void *hook;
    int required;
} HookEntry;

static const HookEntry hooks[] = {
    {API_CREATE_FILE, "KERNEL32.dll", (void *)(uintptr_t)hook_create_file, 1},
    {API_CREATE_DIRECTORY, "KERNEL32.dll", (void *)(uintptr_t)hook_create_directory, 1},
    {API_REMOVE_DIRECTORY, "KERNEL32.dll", (void *)(uintptr_t)hook_remove_directory, 1},
    {API_DELETE_FILE, "KERNEL32.dll", (void *)(uintptr_t)hook_delete_file, 1},
    {API_MOVE_FILE, "KERNEL32.dll", (void *)(uintptr_t)hook_move_file, 1},
    {API_COPY_FILE, "KERNEL32.dll", (void *)(uintptr_t)hook_copy_file, 0},
    {API_GET_ATTRIBUTES, "KERNEL32.dll", (void *)(uintptr_t)hook_get_attributes, 1},
    {API_GET_ATTRIBUTES_EX, "KERNEL32.dll", (void *)(uintptr_t)hook_get_attributes_ex, 1},
    {API_FIND_FIRST, "KERNEL32.dll", (void *)(uintptr_t)hook_find_first, 1},
    {API_FIND_FIRST_EX, "KERNEL32.dll", (void *)(uintptr_t)hook_find_first_ex, 1},
    {API_FINAL_PATH, "KERNEL32.dll", (void *)(uintptr_t)hook_final_path, 1},
    {API_SET_INFO, "KERNEL32.dll", (void *)(uintptr_t)hook_set_info, 1},
    {API_GET_INFO, "KERNEL32.dll", (void *)(uintptr_t)hook_get_info, 0},
    {API_DISK_FREE, "KERNEL32.dll", (void *)(uintptr_t)hook_disk_free, 0},
    {API_GET_PROFILE, "KERNEL32.dll", (void *)(uintptr_t)hook_get_profile, 0},
    {API_WRITE_PROFILE, "KERNEL32.dll", (void *)(uintptr_t)hook_write_profile, 0},
    {API_SHELL_EXECUTE, "SHELL32.dll", (void *)(uintptr_t)hook_shell_execute, 0},
    {API_PARSING_NAME, "SHELL32.dll", (void *)(uintptr_t)hook_parsing_name, 0},
};

// Patches a module's import slots (Azimuth's), all or none of the required ones.
// A slot must still hold the function its DLL exports (no one else hooked it).
static int patch_imports(unsigned char *image) {
    void **slots[ARRAY_COUNT(hooks)];
    for (unsigned i = 0; i < ARRAY_COUNT(hooks); ++i) slots[i] = NULL;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)image;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(image + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return 0;
    const IMAGE_IMPORT_DESCRIPTOR *descriptor = (const IMAGE_IMPORT_DESCRIPTOR *)(image + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char *module = (const char *)(image + descriptor->Name);
        if (!descriptor->OriginalFirstThunk) continue;
        const IMAGE_THUNK_DATA64 *names = (const IMAGE_THUNK_DATA64 *)(image + descriptor->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *entries = (IMAGE_THUNK_DATA64 *)(image + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++entries) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const char *name = (const char *)((const IMAGE_IMPORT_BY_NAME *)(image + names->u1.AddressOfData))->Name;
            for (unsigned i = 0; i < ARRAY_COUNT(hooks); ++i)
                if (!slots[i] && text_equal_ignore_case(module, hooks[i].module) &&
                    text_equal_ignore_case(name, api_names[hooks[i].api]))
                    slots[i] = (void **)&entries->u1.Function;
        }
    }
    int usable = 1;
    for (unsigned i = 0; i < ARRAY_COUNT(hooks); ++i) {
        Line line;
        if (!slots[i] || !originals[hooks[i].api]) {
            if (!hooks[i].required) continue;  // optional and not imported: nothing to do
            line_start(&line, "SAVES: Azimuth does not import ");
            slots[i] = NULL;
        } else if (*slots[i] != originals[hooks[i].api]) {
            line_start(&line, "SAVES: another module already replaced Azimuth's import ");
            slots[i] = NULL;
        } else {
            continue;
        }
        line_text(&line, hooks[i].module);
        line_text(&line, "!");
        line_text(&line, api_names[hooks[i].api]);
        line_text(&line, hooks[i].required ? " (required); the saves stay in the game folder.\r\n" : "; not hooked.\r\n");
        saves_log(line.text);
        if (hooks[i].required) usable = 0;
    }
    if (!usable) return 0;
    for (unsigned i = 0; i < ARRAY_COUNT(hooks); ++i) {
        DWORD protection, ignored;
        if (!slots[i]) continue;
        if (!VirtualProtect(slots[i], sizeof(void *), PAGE_READWRITE, &protection)) {
            for (unsigned j = 0; j < i; ++j) {  // undo: all or nothing
                if (slots[j] && VirtualProtect(slots[j], sizeof(void *), PAGE_READWRITE, &protection)) {
                    InterlockedExchangePointer(slots[j], originals[hooks[j].api]);
                    VirtualProtect(slots[j], sizeof(void *), protection, &ignored);
                }
            }
            saves_log("SAVES: Azimuth's import table cannot be written; the saves stay in the game folder.\r\n");
            return 0;
        }
        InterlockedExchangePointer(slots[i], hooks[i].hook);
        VirtualProtect(slots[i], sizeof(void *), protection, &ignored);
    }
    unsigned count = 0;
    for (unsigned i = 0; i < ARRAY_COUNT(hooks); ++i) count += slots[i] != NULL;
    Line line;
    line_start(&line, "SAVES: ");
    line_number(&line, count);
    line_text(&line, " of Azimuth's file imports redirected (KERNEL32 file calls; SHELL32 open-folder calls).");
    line_end(&line);
    return 1;
}

// ---------------------------------------------------------------- folders and files

static int plain_folder_data(const WIN32_FIND_DATAW *data) {
    return (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(data->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           !(data->cFileName[0] == L'.' &&
             (!data->cFileName[1] || (data->cFileName[1] == L'.' && !data->cFileName[2])));
}

// -1 missing, 0 a file or a link, 1 a real folder.
static int folder_state(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return -1;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
}

// Creates every missing folder of an absolute X:\ path; a link at or below
// trusted_length characters is refused. 1 = ok.
static int make_folders(const wchar_t *path, size_t trusted_length) {
    static wchar_t work[ROOT_CAPACITY];
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    size_t length = path_set(work, path);
    int ok = length >= 3;
    for (size_t i = 3; ok && i <= length; ++i) {
        if (i < length && work[i] != L'\\') continue;
        wchar_t saved = work[i];
        work[i] = 0;
        if (!CREATE_DIRECTORY(work)) {
            DWORD attributes = GetFileAttributesW(work);
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) ok = 0;
            else if (i >= trusted_length && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) ok = 0;
        }
        work[i] = saved;
    }
    ReleaseSRWLockExclusive(&lock);
    return ok;
}

static unsigned char *io_buffer;  // COPY_CHUNK bytes, for hashing and copying

static int hex_value(unsigned c) {
    return c >= '0' && c <= '9' ? (int)(c - '0') : c >= 'a' && c <= 'f' ? (int)(c - 'a' + 10)
                                               : c >= 'A' && c <= 'F' ? (int)(c - 'A' + 10) : -1;
}

// A revision folder's name: 1 to 16 hex digits.
static int revision_number(const wchar_t *name, unsigned long long *value) {
    size_t digits = 0;
    *value = 0;
    for (; name[digits]; ++digits) {
        int nibble = name[digits] < 128 ? hex_value((unsigned)name[digits]) : -1;
        if (nibble < 0 || digits >= 16) return 0;
        *value = *value << 4 | (unsigned)nibble;
    }
    return digits > 0;
}

// A save space: player-<...> or machine.
static int space_name(const wchar_t *name) {
    static const wchar_t player[] = L"player-";
    for (unsigned i = 0; i < 7; ++i)
        if (fold(name[i]) != fold(player[i])) return equal_folded(name, L"machine");
    return name[7] != 0;
}

typedef struct {
    unsigned char hash[32];
    unsigned long long size;
    wchar_t name[BLOB_NAME];
} Blob;

typedef struct {
    unsigned count;
    size_t text_size;
    char text[COMMIT_CAPACITY];
    Blob blobs[MAX_BLOBS];
} Commit;

static Commit commit_buffer;

static int blob_name(const char *text, size_t length, wchar_t output[BLOB_NAME]) {
    if (!length || length >= BLOB_NAME || (length == 1 && text[0] == '.') ||
        (length == 2 && text[0] == '.' && text[1] == '.'))
        return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c <= ' ' || c >= 127 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            return 0;
        output[i] = (wchar_t)c;
    }
    output[length] = 0;
    return 1;
}

// A v2 commit exactly as Azimuth writes it (forza-plugins common/include/fh2_saves.h fh2_parse_commit).
static int parse_commit(Commit *commit) {
    static const char header[] = "Azimuth complete save revision v2";
    const char *text = commit->text;
    size_t size = commit->text_size, at = 0;
    commit->count = 0;
    for (unsigned i = 0; i < sizeof(header) - 1; ++i)
        if (at >= size || text[at++] != header[i]) return 0;
    unsigned expected = 0;
    int have_count = 0;
    while (at < size) {
        if (text[at] == '\r') ++at;
        if (at >= size || text[at] != '\n') return 0;
        ++at;
        if (at >= size) break;
        size_t end = at;
        while (end < size && text[end] != '\n' && text[end] != '\r') ++end;
        const char *line = text + at;
        size_t length = end - at;
        if (!have_count) {
            if (length < 7 || line[0] != 'b' || line[1] != 'l' || line[2] != 'o' || line[3] != 'b' || line[4] != 's' ||
                line[5] != ' ')
                return 0;
            for (size_t i = 6; i < length; ++i) {
                if (line[i] < '0' || line[i] > '9' || expected > MAX_BLOBS) return 0;
                expected = expected * 10 + (unsigned)(line[i] - '0');
            }
            if (!expected || expected > MAX_BLOBS) return 0;
            have_count = 1;
        } else {
            if (commit->count >= expected) return 0;
            Blob *blob = &commit->blobs[commit->count];
            if (length < 64 + 4 || line[64] != ' ') return 0;
            for (unsigned i = 0; i < 32; ++i) {
                int high = hex_value((unsigned char)line[2 * i]), low = hex_value((unsigned char)line[2 * i + 1]);
                if (high < 0 || low < 0) return 0;
                blob->hash[i] = (unsigned char)(high << 4 | low);
            }
            size_t i = 65;
            unsigned long long value = 0;
            if (line[i] < '0' || line[i] > '9') return 0;
            for (; i < length && line[i] >= '0' && line[i] <= '9'; ++i) {
                if (value > (1ull << 40)) return 0;
                value = value * 10 + (unsigned)(line[i] - '0');
            }
            if (i >= length || line[i] != ' ') return 0;
            ++i;
            if (!blob_name(line + i, length - i, blob->name)) return 0;
            for (unsigned j = 0; j < commit->count; ++j)
                if (equal_folded(commit->blobs[j].name, blob->name)) return 0;
            blob->size = value;
            ++commit->count;
        }
        at = end;
    }
    return have_count && commit->count == expected;
}

// Reads and parses <revision>\commit. 1 = a complete revision's commit.
static int read_commit(const wchar_t *revision, Commit *commit, FILETIME *written) {
    static wchar_t path[ROOT_CAPACITY];
    size_t length = path_add(path, path_set(path, revision), L"commit");
    if (!length) return 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size;
    DWORD read = 0;
    int ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (LONGLONG)COMMIT_CAPACITY &&
             ReadFile(file, commit->text, (DWORD)size.QuadPart, &read, NULL) && read == (DWORD)size.QuadPart;
    CloseHandle(file);
    if (!ok) return 0;
    commit->text_size = read;
    if (written) {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (FILE_ATTRIBUTES_EX(path, &data)) *written = data.ftLastWriteTime;
        else written->dwLowDateTime = written->dwHighDateTime = 0;
    }
    return parse_commit(commit);
}

// SHA-256 and size of a file. 1 = read.
static int hash_file(const wchar_t *path, unsigned char digest[32], unsigned long long *size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    Sha256Context hash;
    sha256_initialize(&hash);
    *size = 0;
    int ok = 1;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, io_buffer, COPY_CHUNK, &read, NULL)) {
            ok = 0;
            break;
        }
        if (!read) break;
        sha256_update(&hash, io_buffer, read);
        *size += read;
    }
    CloseHandle(file);
    sha256_finalize(&hash, digest);
    return ok;
}

// Every blob the commit lists is in <revision>\data with its size and SHA-256:
// 1 yes, 0 a blob is missing or differs, -1 a blob exists but cannot be read.
static int verify_revision(const wchar_t *revision, const Commit *commit) {
    static wchar_t path[ROOT_CAPACITY];
    size_t data = path_add(path, path_set(path, revision), L"data");
    if (!data) return -1;
    for (unsigned i = 0; i < commit->count; ++i) {
        unsigned char digest[32];
        unsigned long long size;
        path[data] = 0;
        if (!path_add(path, data, commit->blobs[i].name)) return -1;
        if (!hash_file(path, digest, &size)) return GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES ? 0 : -1;
        if (size != commit->blobs[i].size || !memory_equal(digest, commit->blobs[i].hash, 32)) return 0;
    }
    return 1;
}

// The revision folders below <...>\revisions, newest first.
typedef struct {
    unsigned long long number;
    wchar_t name[20];
} Revision;

static Revision revision_list[MAX_REVISIONS];

// count of revisions (newest first) or -1 when the folder cannot be listed.
static int list_revisions(const wchar_t *revisions) {
    static wchar_t pattern[ROOT_CAPACITY];
    WIN32_FIND_DATAW data;
    if (!path_add(pattern, path_set(pattern, revisions), L"*")) return -1;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0 : -1;
    }
    int count = 0;
    do {
        unsigned long long number;
        if (!plain_folder_data(&data) || !revision_number(data.cFileName, &number)) continue;
        if (count == (int)MAX_REVISIONS) break;
        int at = count++;
        while (at && revision_list[at - 1].number < number) {
            revision_list[at] = revision_list[at - 1];
            --at;
        }
        revision_list[at].number = number;
        memory_copy(revision_list[at].name, data.cFileName, (wide_length(data.cFileName) + 1) * sizeof(wchar_t));
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return count;
}

// Copies one blob, hashing it on the way; 1 when size and SHA-256 match the commit's.
static int copy_blob(const wchar_t *from, const wchar_t *to, const Blob *blob) {
    HANDLE input = CreateFileW(from, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (input == INVALID_HANDLE_VALUE) return 0;
    HANDLE output = CreateFileW(to, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) {
        CloseHandle(input);
        return 0;
    }
    Sha256Context hash;
    sha256_initialize(&hash);
    unsigned long long size = 0;
    int ok = 1;
    for (;;) {
        DWORD read = 0, written = 0;
        if (!ReadFile(input, io_buffer, COPY_CHUNK, &read, NULL)) {
            ok = 0;
            break;
        }
        if (!read) break;
        sha256_update(&hash, io_buffer, read);
        size += read;
        if (!WriteFile(output, io_buffer, read, &written, NULL) || written != read) {
            ok = 0;
            break;
        }
    }
    FILETIME created, accessed, modified;
    if (ok && k32.get_file_time(input, &created, &accessed, &modified)) k32.set_file_time(output, &created, &accessed, &modified);
    ok = ok && FlushFileBuffers(output);
    CloseHandle(output);
    CloseHandle(input);
    unsigned char digest[32];
    sha256_finalize(&hash, digest);
    return ok && size == blob->size && memory_equal(digest, blob->hash, 32);
}

static int write_whole(const wchar_t *path, const char *text, size_t size) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    int ok = WriteFile(file, text, (DWORD)size, &written, NULL) && written == size && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

// Removes a staging revision this file made (its blobs, commit, data, folder).
static void remove_staging(const wchar_t *staging, const Commit *commit) {
    static wchar_t path[ROOT_CAPACITY];
    size_t length = path_set(path, staging), data = path_add(path, length, L"data");
    for (unsigned i = 0; data && i < commit->count; ++i) {
        path[data] = 0;
        if (path_add(path, data, commit->blobs[i].name)) ORIGINAL(API_DELETE_FILE, PathOnlyFn)(path);
    }
    if (data) {
        path[data] = 0;
        REMOVE_DIRECTORY(path);
    }
    path[length] = 0;
    if (path_add(path, length, L"commit")) ORIGINAL(API_DELETE_FILE, PathOnlyFn)(path);
    path[length] = 0;
    REMOVE_DIRECTORY(path);
}

// <Saves>\<title><suffix>: a sibling of the title's Documents folder (Azimuth never sees it).
static size_t sibling_path(wchar_t *output, const TitleRoot *root, const wchar_t *suffix) {
    return path_append(output, path_set(output, root->docs), suffix);
}

static void hex_text(wchar_t *output, unsigned long long value) {
    for (int shift = 60, at = 0; shift >= 0; shift -= 4, ++at) output[at] = L"0123456789abcdef"[(value >> shift) & 15];
    output[16] = 0;
}

// Copies the committed revision `from` (verified while copying and read back)
// into `revisions`\<name>: through <Saves>\<title>.injector-staging and one
// rename. 1 = in place.
static int copy_revision(const TitleRoot *root, const wchar_t *from, const wchar_t *revisions, const wchar_t *name,
                         const Commit *commit) {
    static wchar_t staging[ROOT_CAPACITY], source[ROOT_CAPACITY], target[ROOT_CAPACITY];
    wchar_t unique[40];
    size_t length = sibling_path(staging, root, L".injector-staging");
    if (!length || !make_folders(staging, wide_length(documents_base))) return 0;
    hex_text(unique, GetTickCount64() ^ ((unsigned long long)GetCurrentThreadId() << 48));
    length = path_add(staging, length, name);
    length = length ? path_append(staging, length, L"-") : 0;
    length = length ? path_append(staging, length, unique) : 0;
    if (!length || !CREATE_DIRECTORY(staging)) return 0;
    if (!path_add(staging, length, L"data") || !CREATE_DIRECTORY(staging)) {
        staging[length] = 0;
        REMOVE_DIRECTORY(staging);
        return 0;
    }
    staging[length] = 0;
    size_t source_data = path_add(source, path_set(source, from), L"data");
    int ok = source_data != 0;
    staging[length] = 0;
    for (unsigned i = 0; ok && i < commit->count; ++i) {
        source[source_data] = 0;
        size_t blob_target = path_add(target, path_add(target, path_set(target, staging), L"data"), commit->blobs[i].name);
        ok = path_add(source, source_data, commit->blobs[i].name) && blob_target &&
             copy_blob(source, target, &commit->blobs[i]);
    }
    if (ok) ok = path_add(target, path_set(target, staging), L"commit") && write_whole(target, commit->text, commit->text_size);
    if (ok) ok = verify_revision(staging, commit) == 1;  // read back from the disk
    if (ok) ok = make_folders(revisions, wide_length(documents_base));
    if (ok) ok = path_add(target, path_set(target, revisions), name) && MoveFileExW(staging, target, 0);
    if (!ok) remove_staging(staging, commit);
    return ok;
}

static int later(FILETIME a, FILETIME b) {
    return a.dwHighDateTime != b.dwHighDateTime ? a.dwHighDateTime > b.dwHighDateTime : a.dwLowDateTime > b.dwLowDateTime;
}

// The newest revision below `revisions` with a commit that parses: its index in
// revision_list (filled), -1 when none, -2 when the folder cannot be listed.
static int newest_committed(const wchar_t *revisions, FILETIME *written) {
    static wchar_t path[ROOT_CAPACITY];
    int count = list_revisions(revisions);
    if (count < 0) return -2;
    for (int i = 0; i < count; ++i) {
        path[0] = 0;
        if (path_add(path, path_set(path, revisions), revision_list[i].name) &&
            read_commit(path, &commit_buffer, written))
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------- preparing a title's Documents folder

typedef struct {
    unsigned copied, kept, skipped, failed, newer;
} Migration;

// One container of the game folder: copied when Documents has no complete revision of it.
static void migrate_container(const TitleRoot *root, const wchar_t *space, const wchar_t *container, Migration *result) {
    static wchar_t game[ROOT_CAPACITY], docs[ROOT_CAPACITY], revision[ROOT_CAPACITY];
    static Revision game_list[MAX_REVISIONS];
    size_t game_length = path_add(game, path_add(game, path_add(game, path_set(game, root->game), space), container),
                                  L"revisions");
    size_t docs_length = path_add(docs, path_add(docs, path_add(docs, path_set(docs, root->docs), space), container),
                                  L"revisions");
    if (!game_length || !docs_length) {
        ++result->failed;
        return;
    }
    FILETIME docs_time = {0, 0}, game_time = {0, 0};
    int docs_newest = newest_committed(docs, &docs_time);
    if (docs_newest == -2) {
        log_path_line("SAVES: cannot list ", docs, "; nothing copied there.");
        ++result->failed;
        return;
    }
    wchar_t docs_name[20];
    if (docs_newest >= 0) memory_copy(docs_name, revision_list[docs_newest].name, sizeof(docs_name));
    int count = list_revisions(game);
    if (count < 0) {
        log_path_line("SAVES: cannot list ", game, "; nothing copied from there.");
        ++result->failed;
        return;
    }
    for (int i = 0; i < count; ++i) game_list[i] = revision_list[i];
    if (docs_newest >= 0) {
        ++result->kept;
        // Only a note: the game uses Documents' newest complete revision.
        for (int i = 0; i < count; ++i) {
            if (!path_add(revision, path_set(revision, game), game_list[i].name) ||
                !read_commit(revision, &commit_buffer, &game_time))
                continue;
            if (later(game_time, docs_time)) {
                ++result->newer;
                Line line;
                line_start(&line, "SAVES: note: the game folder's ");
                line_path(&line, revision);
                line_text(&line, " was saved after Documents' revision ");
                line_path(&line, docs_name);
                line_text(&line, "; the game uses Documents' (the game folder's is kept as it is).");
                line_end(&line);
            }
            break;
        }
        return;
    }
    unsigned copied = 0;
    for (int i = 0; i < count; ++i) {
        if (!path_add(revision, path_set(revision, game), game_list[i].name)) continue;
        if (!read_commit(revision, &commit_buffer, NULL)) continue;  // being written or damaged: Azimuth skips it too
        int verified = verify_revision(revision, &commit_buffer);
        if (verified < 0) {
            log_path_line("SAVES: ", revision, " cannot be read; nothing copied from it this time.");
            ++result->failed;
            return;
        }
        if (!verified) {
            log_path_line("SAVES: ", revision, " does not match its commit; not copied.");
            ++result->skipped;
            continue;
        }
        if (!copy_revision(root, revision, docs, game_list[i].name, &commit_buffer)) {
            log_path_line("SAVES: copying ", revision, " to Documents FAILED (the original is kept as it is).");
            ++result->failed;
            return;
        }
        ++copied;
        if (copied >= KEEP_REVISIONS) break;  // Azimuth keeps the newest two
    }
    result->copied += copied;
}

// <Saves>\<title>.migrated.txt: the game folders already copied (one per line, UTF-8).
static int already_migrated(const TitleRoot *root) {
    static wchar_t path[ROOT_CAPACITY];
    static char text[16384], wanted[ROOT_CAPACITY * 3];
    if (!sibling_path(path, root, L".migrated.txt")) return 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD read = 0;
    ReadFile(file, text, sizeof(text) - 1, &read, NULL);
    CloseHandle(file);
    text[read] = 0;
    int wanted_length = WideCharToMultiByte(CP_UTF8, 0, root->game, (int)root->game_length, wanted,
                                            (int)sizeof(wanted) - 1, NULL, NULL);
    if (wanted_length <= 0) return 0;
    wanted[wanted_length] = 0;
    for (char *line = text; *line;) {
        char *end = line;
        while (*end && *end != '\r' && *end != '\n') ++end;
        char saved = *end;
        *end = 0;
        int same = 1;
        size_t i = 0;
        for (; line[i] && wanted[i]; ++i) {
            char a = line[i], b = wanted[i];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) {
                same = 0;
                break;
            }
        }
        same = same && !line[i] && !wanted[i];
        *end = saved;
        if (same) return 1;
        line = end;
        while (*line == '\r' || *line == '\n') ++line;
    }
    return 0;
}

static void note_migrated(const TitleRoot *root) {
    static wchar_t path[ROOT_CAPACITY];
    static char line[ROOT_CAPACITY * 3 + 64];
    if (!sibling_path(path, root, L".migrated.txt")) return;
    int length = WideCharToMultiByte(CP_UTF8, 0, root->game, (int)root->game_length, line, (int)sizeof(line) - 3,
                                     NULL, NULL);
    if (length <= 0) return;
    line[length++] = '\r';
    line[length++] = '\n';
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, line, (DWORD)length, &written, NULL);
    CloseHandle(file);
}

// First redirected start of this install: every container of the game folder
// that Documents has no complete revision of is copied (the newest two complete
// ones that verify). 1 = done (or nothing to do).
static int migrate_title(const TitleRoot *root) {
    static wchar_t pattern[ROOT_CAPACITY], space_pattern[ROOT_CAPACITY];
    if (already_migrated(root)) return 1;
    int state = folder_state(root->game);
    if (state < 0) {
        note_migrated(root);
        return 1;
    }
    if (!state) {
        log_path_line("SAVES: the game-folder saves ", root->game, " are a link or a file; nothing copied.");
        return 0;
    }
    Migration result = {0, 0, 0, 0, 0};
    WIN32_FIND_DATAW space, container;
    if (!path_add(pattern, path_set(pattern, root->game), L"*")) return 0;
    HANDLE spaces = FindFirstFileW(pattern, &space);
    if (spaces != INVALID_HANDLE_VALUE) {
        do {
            if (!plain_folder_data(&space) || !space_name(space.cFileName)) continue;
            size_t length = path_add(space_pattern, path_set(space_pattern, root->game), space.cFileName);
            if (!length || !path_add(space_pattern, length, L"*")) {
                ++result.failed;
                continue;
            }
            HANDLE containers = FindFirstFileW(space_pattern, &container);
            if (containers == INVALID_HANDLE_VALUE) continue;
            do {
                if (plain_folder_data(&container)) migrate_container(root, space.cFileName, container.cFileName, &result);
            } while (FindNextFileW(containers, &container));
            FindClose(containers);
        } while (FindNextFileW(spaces, &space));
        FindClose(spaces);
    }
    Line line;
    line_start(&line, "SAVES: first start with the saves in Documents: ");
    line_number(&line, result.copied);
    line_text(&line, " revisions copied from the game folder (verified, SHA-256; the originals are kept), ");
    line_number(&line, result.kept);
    line_text(&line, " saves already in Documents used as they are");
    if (result.skipped) {
        line_text(&line, ", ");
        line_number(&line, result.skipped);
        line_text(&line, " damaged revisions not copied");
    }
    if (result.failed) {
        line_text(&line, ", ");
        line_number(&line, result.failed);
        line_text(&line, " FAILED");
    }
    line_text(&line, ".");
    line_end(&line);
    if (result.failed) return 0;
    note_migrated(root);
    return 1;
}

// Containers in Documents with more revisions than Azimuth keeps (a mirror's
// history): all but the newest two that verify (and anything newer than them)
// move to <Saves>\<title>.history\<space>\<container>\revisions, untouched, so
// Azimuth's pruning does not delete them. Same volume: renames only.
static void archive_container(const TitleRoot *root, const wchar_t *space, const wchar_t *container,
                              unsigned *archived) {
    static wchar_t revisions[ROOT_CAPACITY], history[ROOT_CAPACITY], from[ROOT_CAPACITY], to[ROOT_CAPACITY];
    size_t length = path_add(revisions, path_add(revisions, path_add(revisions, path_set(revisions, root->docs), space),
                                                 container),
                             L"revisions");
    if (!length) return;
    int count = list_revisions(revisions);
    if (count <= (int)KEEP_REVISIONS) return;
    unsigned kept = 0;
    size_t history_length = sibling_path(history, root, L".history");
    history_length = history_length ? path_add(history, history_length, space) : 0;
    history_length = history_length ? path_add(history, history_length, container) : 0;
    history_length = history_length ? path_add(history, history_length, L"revisions") : 0;
    if (!history_length) return;
    for (int i = 0; i < count; ++i) {
        if (!path_add(from, path_set(from, revisions), revision_list[i].name)) return;
        if (kept < KEEP_REVISIONS) {
            if (read_commit(from, &commit_buffer, NULL) && verify_revision(from, &commit_buffer) == 1) ++kept;
            continue;
        }
        if (!make_folders(history, wide_length(documents_base))) return;
        if (!path_add(to, path_set(to, history), revision_list[i].name)) return;
        if (GetFileAttributesW(to) != INVALID_FILE_ATTRIBUTES) {
            log_path_line("SAVES: ", to, " already exists; the revision stays where it is (Azimuth may remove it).");
            continue;
        }
        if (MoveFileExW(from, to, 0)) ++*archived;
        else log_path_line("SAVES: cannot move ", from, " to the history folder; Azimuth may remove it.");
    }
}

static void archive_title(const TitleRoot *root) {
    static wchar_t pattern[ROOT_CAPACITY], space_pattern[ROOT_CAPACITY], history[ROOT_CAPACITY];
    WIN32_FIND_DATAW space, container;
    unsigned archived = 0;
    if (!path_add(pattern, path_set(pattern, root->docs), L"*")) return;
    HANDLE spaces = FindFirstFileW(pattern, &space);
    if (spaces == INVALID_HANDLE_VALUE) return;
    do {
        if (!plain_folder_data(&space) || !space_name(space.cFileName)) continue;
        size_t length = path_add(space_pattern, path_set(space_pattern, root->docs), space.cFileName);
        if (!length || !path_add(space_pattern, length, L"*")) continue;
        HANDLE containers = FindFirstFileW(space_pattern, &container);
        if (containers == INVALID_HANDLE_VALUE) continue;
        do {
            if (plain_folder_data(&container)) archive_container(root, space.cFileName, container.cFileName, &archived);
        } while (FindNextFileW(containers, &container));
        FindClose(containers);
    } while (FindNextFileW(spaces, &space));
    FindClose(spaces);
    if (archived) {
        sibling_path(history, root, L".history");
        Line line;
        line_start(&line, "SAVES: ");
        line_number(&line, archived);
        line_text(&line, " older revisions (beyond the newest two Azimuth keeps) moved, untouched, to ");
        line_path(&line, history);
        line_end(&line);
    }
}

// Documents folder of one title: no links, created, history archived, game-folder saves copied once.
static int prepare_title(TitleRoot *root) {
    size_t trusted = wide_length(documents_base);
    if (!make_folders(root->docs, trusted)) {
        log_path_line("SAVES: cannot create ", root->docs, " (or it is a link: Azimuth refuses links).");
        return 0;
    }
    archive_title(root);
    return migrate_title(root);
}

// ---------------------------------------------------------------- setup

// Documents (User Shell Folders\Personal, wherever it was moved), else %USERPROFILE%\Documents.
static size_t documents_folder(wchar_t *output) {
    static wchar_t raw[ROOT_CAPACITY];
    size_t length = 0;
    HKEY key;
    static const wchar_t *const keys[] = {L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders"};
    for (unsigned i = 0; i < 2 && !length && k32.open_key && k32.query_value && k32.close_key; ++i) {
        if (k32.open_key(HKEY_CURRENT_USER, keys[i], 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) continue;
        DWORD type = 0, size = sizeof(raw) - sizeof(wchar_t);
        if (k32.query_value(key, L"Personal", NULL, &type, (LPBYTE)raw, &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            raw[size / sizeof(wchar_t)] = 0;
            DWORD count = k32.expand(raw, output, ROOT_CAPACITY);
            if (count && count <= ROOT_CAPACITY) length = wide_length(output);
        }
        k32.close_key(key);
    }
    if (!length) {
        DWORD count = k32.environment(L"USERPROFILE", output, ROOT_CAPACITY - 16);
        if (count && count < ROOT_CAPACITY - 16) length = path_add(output, count, L"Documents");
    }
    while (length > 3 && output[length - 1] == L'\\') output[--length] = 0;
    // A full X:\ path only (a share works for Windows, not for Azimuth's checks).
    if (length < 4 || output[1] != L':' || output[2] != L'\\') return 0;
    return length;
}

// [Saves] Location: 1 = Documents, 0 = Game (also absent), -1 = unknown value.
static int location_setting(const wchar_t *ini, wchar_t *value, unsigned capacity) {
    GetPrivateProfileStringW(L"Saves", L"Location", L"", value, capacity, ini);
    if (equal_folded(value, L"Documents")) return 1;
    if (!value[0] || equal_folded(value, L"Game")) return 0;
    return -1;
}

// Roots of every known title: <exe folder>\data\titles\<title>\saves and
// <documents>\My Games\<name>\Saves\<title>. 1 = usable.
static int set_roots(const wchar_t *titles_folder, const wchar_t *my_games) {
    for (unsigned t = 0; t < ARRAY_COUNT(titles); ++t) {
        TitleRoot *root = &titles[t];
        size_t length = path_set(root->game, titles_folder);
        length = length ? path_add(root->game, length, root->title) : 0;
        root->game_length = length ? path_add(root->game, length, L"saves") : 0;
        length = path_set(root->docs, my_games);
        length = length ? path_add(root->docs, length, root->name) : 0;
        length = length ? path_add(root->docs, length, L"Saves") : 0;
        root->docs_length = length ? path_add(root->docs, length, root->title) : 0;
        if (!root->game_length || !root->docs_length ||
            root_prefix(root->docs, root->docs_length, root->game, root->game_length) ||
            root_prefix(root->game, root->game_length, root->docs, root->docs_length))
            return 0;
        root->redirected = 0;
        root->present = 0;
    }
    return 1;
}

// Prepares the titles this install has (data\titles\<title>, or the launcher's
// game); another title's Documents folder is left alone. 1 = all ready.
static int prepare_titles(void) {
    static wchar_t title_folder[ROOT_CAPACITY];
    for (unsigned t = 0; t < ARRAY_COUNT(titles); ++t) {
        TitleRoot *root = &titles[t];
        size_t length = path_set(title_folder, root->game);
        while (length && title_folder[length - 1] != L'\\') --length;
        if (length) title_folder[length - 1] = 0;
        root->present = folder_state(title_folder) == 1 || equal_folded(launcher_game, root->title);
        if (root->present && !prepare_title(root)) return 0;
    }
    return 1;
}

static void log_summary(void) {
    for (unsigned t = 0; t < ARRAY_COUNT(titles); ++t) {
        if (!titles[t].present) continue;
        Line line;
        line_start(&line, "SAVES: ");
        line_path(&line, titles[t].title);
        line_text(&line, ": Azimuth's ");
        line_path(&line, titles[t].game);
        line_text(&line, " -> ");
        line_path(&line, titles[t].docs);
        line_text(&line, " (loaded and saved there; the game-folder saves are not read or written).");
        line_end(&line);
    }
}

void saves_location_attach(HINSTANCE module) {
    static wchar_t directory[ROOT_CAPACITY], ini[ROOT_CAPACITY], exe[ROOT_CAPACITY], my_games[ROOT_CAPACITY];
    wchar_t value[64];
    DWORD count = GetModuleFileNameW(module, directory, ROOT_CAPACITY);
    if (!count || count >= ROOT_CAPACITY) return;
    while (count && directory[count - 1] != L'\\' && directory[count - 1] != L'/') --count;
    if (!count) return;
    directory[count] = 0;
    if (!path_add(ini, path_set(ini, directory), L"forza-injector.ini") ||
        !path_add(log_path, path_set(log_path, directory), L"forza-injector.log"))
        return;
    int setting = location_setting(ini, value, ARRAY_COUNT(value));
    if (setting <= 0) {
        if (setting < 0) {
            log_path_line("SAVES: forza-injector.ini [Saves] Location=", value,
                          " is not Documents or Game; the saves stay in Azimuth's folder.");
            log_start();
        }
        return;
    }
    if (!resolve_functions()) {
        saves_log("SAVES: Windows lacks a file function this needs; the saves stay in Azimuth's folder.\r\n");
        log_start();
        return;
    }
    AzimuthBuild build = azimuth_host_build((const unsigned char *)GetModuleHandleW(NULL));
    if (build != AZIMUTH_101 && build != AZIMUTH_102 && build != AZIMUTH_103 && build != AZIMUTH_104) {
        saves_log("SAVES: [Saves] Location=Documents, but the host is not Azimuth 1.0.1 to 1.0.4 (their save "
                  "layout is the one redirected); the saves stay where they are.\r\n");
        log_start();
        return;
    }
    count = GetModuleFileNameW(NULL, exe, ROOT_CAPACITY);
    if (!count || count >= ROOT_CAPACITY) return;
    while (count && exe[count - 1] != L'\\' && exe[count - 1] != L'/') --count;
    exe[count] = 0;
    static wchar_t azimuth_ini[ROOT_CAPACITY];
    if (path_add(azimuth_ini, path_add(azimuth_ini, path_set(azimuth_ini, exe), L"data"), L"azimuth.ini"))
        GetPrivateProfileStringW(L"launcher", L"game", L"", launcher_game, ARRAY_COUNT(launcher_game), azimuth_ini);
    size_t length = documents_folder(documents_base);
    size_t my_games_length = length ? path_add(my_games, path_set(my_games, documents_base), L"My Games") : 0;
    static wchar_t titles_folder[ROOT_CAPACITY];
    if (!my_games_length || !path_add(titles_folder, path_add(titles_folder, path_set(titles_folder, exe), L"data"),
                                      L"titles") ||
        !set_roots(titles_folder, my_games)) {
        saves_log("SAVES: [Saves] Location=Documents, but the Documents folder is not found or the folders are "
                  "not usable; the saves stay in Azimuth's folder.\r\n");
        log_start();
        return;
    }
    log_path_line("SAVES: [Saves] Location=Documents: Azimuth's saves go to ", my_games, "\\<game>\\Saves\\<title>.");
    io_buffer = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, COPY_CHUNK);
    int ready = io_buffer && prepare_titles();
    heap_release(io_buffer);
    io_buffer = NULL;
    if (!ready) {
        saves_log("SAVES: the Documents folder is not ready; this session the saves stay in Azimuth's folder.\r\n");
        log_start();
        return;
    }
    for (unsigned t = 0; t < ARRAY_COUNT(titles); ++t) titles[t].redirected = 1;
    InterlockedExchange(&active, 1);
    if (!patch_imports((unsigned char *)GetModuleHandleW(NULL))) {
        InterlockedExchange(&active, 0);
        for (unsigned t = 0; t < ARRAY_COUNT(titles); ++t) titles[t].redirected = 0;
        log_start();
        return;
    }
    log_summary();
    log_start();
}

// For plugins (forza-plugins common/include/fh2_saves.h fh2_effective_title_saves):
// the folder Azimuth's saves of `title` (8 hex digits) are in when they are
// redirected: its length, copied to output; 0 when they are in Azimuth's own
// folder (no redirect, another title, or capacity too small).
__declspec(dllexport) unsigned ForzaInjectorSavesFolder(const wchar_t *title, wchar_t *output, unsigned capacity) {
    if (!active || !title || !output) return 0;
    for (unsigned t = 0; t < ARRAY_COUNT(titles); ++t) {
        if (!titles[t].redirected || !equal_folded(title, titles[t].title)) continue;
        if (titles[t].docs_length >= capacity) return 0;
        memory_copy(output, titles[t].docs, (titles[t].docs_length + 1) * sizeof(wchar_t));
        return (unsigned)titles[t].docs_length;
    }
    return 0;
}

#ifdef FH2_TWEAKS_TEST
// Offline tests (tests/saves_tests.py): roots below a temporary folder, the log,
// optionally the preparation; the hooks then call KERNEL32 directly.
// titles_folder stands for <exe folder>\data\titles, my_games for Documents\My Games.
__declspec(dllexport) int ForzaTestSavesConfigure(const wchar_t *titles_folder, const wchar_t *my_games,
                                                  const wchar_t *log_file, int prepare) {
    InterlockedExchange(&active, 0);
    if (!resolve_functions()) return 0;
    for (unsigned i = 0; i < API_COUNT; ++i) first_use[i] = 0;
    first_redirect = first_reverse = 0;
    pending_length = 0;
    log_restarted = 1;  // straight to the file
    log_live = 1;
    log_path[0] = 0;
    if (log_file) path_set(log_path, log_file);
    launcher_game[0] = 0;
    if (!titles_folder || !my_games || !set_roots(titles_folder, my_games)) return 0;
    size_t length = path_set(documents_base, my_games);
    while (length && documents_base[length - 1] != L'\\') --length;
    if (length) documents_base[length - 1] = 0;
    if (prepare) {
        io_buffer = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, COPY_CHUNK);
        int ready = io_buffer && prepare_titles();
        heap_release(io_buffer);
        io_buffer = NULL;
        if (!ready) return 0;
    }
    for (unsigned t = 0; t < ARRAY_COUNT(titles); ++t) titles[t].redirected = 1;
    InterlockedExchange(&active, 1);
    return 1;
}

__declspec(dllexport) void ForzaTestSavesOff(void) {
    InterlockedExchange(&active, 0);
}

__declspec(dllexport) void *ForzaTestSavesHook(const char *name) {
    for (unsigned i = 0; i < ARRAY_COUNT(hooks); ++i)
        if (text_equal_ignore_case(name, api_names[hooks[i].api])) return hooks[i].hook;
    return NULL;
}

__declspec(dllexport) int ForzaTestSavesMap(const wchar_t *path, wchar_t *output, unsigned capacity, int reverse) {
    if (reverse) return (int)map_reverse(path, wide_length(path), output, capacity);
    wchar_t *mapped = map_forward(path, API_COUNT);
    if (!mapped) return 0;
    size_t length = wide_length(mapped);
    int fits = length < capacity;
    if (fits) memory_copy(output, mapped, (length + 1) * sizeof(wchar_t));
    heap_release(mapped);
    return fits ? (int)length : 0;
}

// [Saves] Location of an ini: 1 Documents, 0 Game or absent, -1 unknown.
__declspec(dllexport) int ForzaTestSavesSetting(const wchar_t *ini) {
    wchar_t value[64];
    return location_setting(ini, value, ARRAY_COUNT(value));
}

__declspec(dllexport) int ForzaTestDocumentsFolder(wchar_t *output, unsigned capacity) {
    static wchar_t found[ROOT_CAPACITY];
    if (!resolve_functions()) return 0;
    size_t length = documents_folder(found);
    if (!length || length >= capacity) return 0;
    memory_copy(output, found, (length + 1) * sizeof(wchar_t));
    return (int)length;
}

// Patches a loaded module's imports (a test DLL standing for azimuth.exe).
__declspec(dllexport) int ForzaTestSavesPatch(void *image) {
    return patch_imports((unsigned char *)image);
}

// "<title>\t<name>\n" for every known title (checked against profiles/*.json).
__declspec(dllexport) int ForzaTestSavesTitles(wchar_t *output, unsigned capacity) {
    size_t at = 0;
    for (unsigned t = 0; t < ARRAY_COUNT(titles); ++t) {
        size_t a = wide_length(titles[t].title), b = wide_length(titles[t].name);
        if (at + a + b + 3 > capacity) return 0;
        memory_copy(output + at, titles[t].title, a * sizeof(wchar_t));
        at += a;
        output[at++] = L'\t';
        memory_copy(output + at, titles[t].name, b * sizeof(wchar_t));
        at += b;
        output[at++] = L'\n';
    }
    output[at] = 0;
    return (int)at;
}
#endif
