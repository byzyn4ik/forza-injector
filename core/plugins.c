// Game-independent mod discovery and plugin lifecycle.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugins.h"
#include "forza_runtime.h"
#include "forza_profile.h"
#include "json.h"
#include "logging.h"
#include "support.h"

#define MAX_MODS 128
#define MAX_PLUGINS 512
typedef struct {
    wchar_t name[MAX_PATH];
    int listed, disabled, valid;
    JsonDocument manifest;
    const char *id;
} Mod;
typedef struct {
    FH2PluginContext v1;
    ForzaContext v2;
    ForzaRuntimeContext runtime;
    wchar_t *path, *folder;
    HMODULE module;
    int early_failed;
} Plugin;
static Mod mods[MAX_MODS];
static unsigned mod_count, plugin_count;
static Plugin *plugins[MAX_PLUGINS];
static wchar_t mods_path[PATH_CAPACITY], executable_path[PATH_CAPACITY], settings_path[PATH_CAPACITY];
static char active_title[9];
static unsigned char *game_image;
static uint32_t game_timestamp, game_size;
static const ForzaGameInfo *game_info;
static volatile LONG started;
static HANDLE loader_thread;
static int provider_result;
BOOL forza_file_exports(const wchar_t *path, const char *name);

static BOOL subscribe_unavailable(uint32_t event, ForzaEventFn callback, void *user) {
    (void)event; (void)callback; (void)user;
    // Native events require a game runtime; do not promise unsupported hooks.
    return FALSE;
}
static wchar_t *copy_wide(const wchar_t *source) {
    size_t bytes = ((size_t)lstrlenW(source) + 1) * sizeof(wchar_t);
    wchar_t *value = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (value) memory_copy(value, source, bytes);
    return value;
}
static char *read_text(const wchar_t *path, DWORD maximum, int *present) {
    *present = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) *present = 1;
        return NULL;
    }
    *present = 1;
    LARGE_INTEGER size;
    char *text = NULL;
    if (GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= maximum) {
        text = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)size.QuadPart + 1);
        DWORD count = 0;
        if (text && (!ReadFile(file, text, (DWORD)size.QuadPart, &count, NULL) || count != size.QuadPart ||
                     text_length(text) != count || (count && !MultiByteToWideChar(CP_UTF8,
                     MB_ERR_INVALID_CHARS, text, (int)count, NULL, 0)))) {
            HeapFree(GetProcessHeap(), 0, text); text = NULL;
        }
    }
    CloseHandle(file);
    return text;
}
static int safe_folder(const wchar_t *name) {
    if (!name[0] || name[0] == L'.') return 0;
    for (const wchar_t *p = name; *p; ++p)
        if (*p < 32 || *p == L'/' || *p == L'\\' || *p == L':' || *p == L'*' || *p == L'?') return 0;
    return 1;
}
static int discover_mods(void) {
    wchar_t *path = HeapAlloc(GetProcessHeap(), 0, PATH_CAPACITY * sizeof(wchar_t));
    if (!path || !path_join(path, mods_path, L"*")) return 0;
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(path, &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !safe_folder(entry.cFileName)) continue;
            if (mod_count == MAX_MODS) { FindClose(search); HeapFree(GetProcessHeap(), 0, path); return 0; }
            unsigned at = mod_count++;
            while (at && lstrcmpiW(mods[at - 1].name, entry.cFileName) > 0) {
                memory_copy(&mods[at], &mods[at - 1], sizeof(Mod)); --at;
            }
            lstrcpyW(mods[at].name, entry.cFileName);
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) {
        HeapFree(GetProcessHeap(), 0, path); return 0;
    }
    if (!path_join(path, mods_path, L"loadorder.txt")) { HeapFree(GetProcessHeap(), 0, path); return 0; }
    int present;
    char *order = read_text(path, 65536, &present);
    HeapFree(GetProcessHeap(), 0, path);
    if (present && !order) return 0;
    if (!order) return 1;
    Mod *sorted = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(mods));
    if (!sorted) { HeapFree(GetProcessHeap(), 0, order); return 0; }
    unsigned count = 0;
    char *cursor = order;
    if (text_length(cursor) >= 3 && memory_equal(cursor, "\xef\xbb\xbf", 3)) cursor += 3;
    while (*cursor) {
        char *start = cursor;
        while (*cursor && *cursor != '\r' && *cursor != '\n') ++cursor;
        if (*cursor) *cursor++ = 0;
        while (*start == ' ' || *start == '\t') ++start;
        size_t size = text_length(start);
        while (size && (start[size - 1] == ' ' || start[size - 1] == '\t')) start[--size] = 0;
        int disabled = *start == '#';
        if (disabled) { ++start; while (*start == ' ' || *start == '\t') ++start; }
        if (!*start) continue;
        wchar_t name[MAX_PATH];
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, start, -1, name, MAX_PATH);
        if (!length || !safe_folder(name)) {
            if (disabled) continue; // ordinary explanatory comment
            HeapFree(GetProcessHeap(), 0, sorted); HeapFree(GetProcessHeap(), 0, order); return 0;
        }
        for (unsigned i = 0; i < mod_count; ++i) if (!lstrcmpiW(mods[i].name, name)) {
            if (mods[i].listed) {
                HeapFree(GetProcessHeap(), 0, sorted); HeapFree(GetProcessHeap(), 0, order); return 0;
            }
            mods[i].listed = 1; mods[i].disabled = disabled;
            memory_copy(&sorted[count++], &mods[i], sizeof(Mod)); break;
        }
    }
    for (unsigned i = 0; i < mod_count; ++i)
        if (!mods[i].listed) memory_copy(&sorted[count++], &mods[i], sizeof(Mod));
    memory_copy(mods, sorted, sizeof(mods));
    HeapFree(GetProcessHeap(), 0, sorted); HeapFree(GetProcessHeap(), 0, order);
    return 1;
}
static int mod_allowed(Mod *mod, const wchar_t *folder, wchar_t *path) {
    if (mod->disabled || !path_join(path, folder, L"mod.json")) return 0;
    int present;
    char *text = read_text(path, 1024 * 1024, &present);
    if (!present) return mod->valid = 1; // legacy folders need no manifest
    if (!text || !json_parse(&mod->manifest, text, text_length(text))) return 0;
    JsonValue *root = mod->manifest.root;
    if (!root || root->kind != JSON_OBJECT) return 0;
    JsonValue *enabled = json_get(root, "enabled");
    if (enabled && (enabled->kind != JSON_BOOL || !(enabled->length == 4 && memory_equal(enabled->text, "true", 4)))) return 0;
    unsigned format;
    if (!json_uint(json_get(root, "format_version"), &format) || format != 1) return 0;
    JsonValue *id = json_get(root, "id");
    if (!id || id->kind != JSON_STRING || !id->length) return 0;
    mod->id = id->text;
    for (Mod *previous = mods; previous < mod; ++previous)
        if (previous->valid && previous->id && text_equal_ignore_case(previous->id, mod->id)) return 0;
    JsonValue *games = json_get(root, "games");
    if (games) {
        if (games->kind != JSON_ARRAY) return 0;
        int allowed = !games->child;
        for (JsonValue *item = games->child; item; item = item->next) {
            if (item->kind != JSON_STRING) return 0;
            if ((game_info && text_equal_ignore_case(item->text, game_info->id)) ||
                (active_title[0] && text_equal_ignore_case(item->text, active_title))) allowed = 1;
        }
        if (!allowed) return 0;
    }
    JsonValue *dependencies = json_get(root, "dependencies");
    if (dependencies) {
        if (dependencies->kind != JSON_ARRAY) return 0;
        for (JsonValue *item = dependencies->child; item; item = item->next) {
            if (item->kind != JSON_STRING) return 0;
            int found = 0;
            for (Mod *previous = mods; previous < mod; ++previous)
                if (previous->valid && previous->id && text_equal_ignore_case(previous->id, item->text)) found = 1;
            if (!found) return 0;
        }
    }
    return mod->valid = 1;
}
static void fill_context(Plugin *plugin) {
    plugin->v1 = (FH2PluginContext){sizeof(FH2PluginContext), FH2_PLUGIN_API_VERSION,
        FH2_PLUGIN_AZIMUTH, game_size, game_image, NULL, plugin->folder, plugin->path, "", log_append};
    unsigned char *host = (unsigned char *)GetModuleHandleW(NULL);
    uint32_t host_timestamp = 0, host_size = 0;
    forza_image_identity(host, &host_timestamp, &host_size);
    plugin->v2 = (ForzaContext){sizeof(ForzaContext), FORZA_PLUGIN_API_VERSION, &plugin->v1,
        game_info ? game_info->game : FORZA_GAME_UNKNOWN, game_info ? game_info->id : "", active_title,
        game_timestamp, host, host_timestamp, host_size, MOD_VERSION, subscribe_unavailable};
    plugin->runtime = (ForzaRuntimeContext){sizeof(ForzaRuntimeContext), FORZA_RUNTIME_API_VERSION,
        &plugin->v2, executable_path, settings_path};
}
static int discover_plugins(void) {
    wchar_t *folder = HeapAlloc(GetProcessHeap(), 0, PATH_CAPACITY * sizeof(wchar_t));
    wchar_t *path = HeapAlloc(GetProcessHeap(), 0, PATH_CAPACITY * sizeof(wchar_t));
    wchar_t (*names)[MAX_PATH] = HeapAlloc(GetProcessHeap(), 0, MAX_PLUGINS * sizeof(*names));
    int ok = folder && path && names;
    for (unsigned m = 0; ok && m < mod_count; ++m) {
        if (!path_join(folder, mods_path, mods[m].name) || !mod_allowed(&mods[m], folder, path)) continue;
        if (!path_join(path, folder, L"plugins")) { ok = 0; break; }
        lstrcpyW(folder, path);
        if (!path_join(path, folder, L"*.dll")) { ok = 0; break; }
        WIN32_FIND_DATAW entry;
        HANDLE search = FindFirstFileW(path, &entry);
        if (search == INVALID_HANDLE_VALUE) continue;
        unsigned count = 0;
        do {
            if (entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (count == MAX_PLUGINS || count + plugin_count == MAX_PLUGINS) { ok = 0; break; }
            unsigned at = count++;
            while (at && lstrcmpiW(names[at - 1], entry.cFileName) > 0) {
                lstrcpyW(names[at], names[at - 1]); --at;
            }
            lstrcpyW(names[at], entry.cFileName);
        } while (FindNextFileW(search, &entry));
        FindClose(search);
        for (unsigned i = 0; ok && i < count; ++i) {
            if (!path_join(path, folder, names[i])) { ok = 0; break; }
            Plugin *plugin = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Plugin));
            if (!plugin) { ok = 0; break; }
            plugin->path = copy_wide(path); plugin->folder = copy_wide(folder);
            if (!plugin->path || !plugin->folder) { ok = 0; break; }
            fill_context(plugin); plugins[plugin_count++] = plugin;
        }
    }
    if (folder) HeapFree(GetProcessHeap(), 0, folder);
    if (path) HeapFree(GetProcessHeap(), 0, path);
    if (names) HeapFree(GetProcessHeap(), 0, names);
    return ok;
}
static int load_plugin(Plugin *plugin) {
    if (!plugin->module) {
        plugin->module = LoadLibraryExW(plugin->path, NULL,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        plugin->v1.plugin_module = plugin->module;
        if (!plugin->module) log_append_value("PLUGINS: DLL load failed; Windows error=", (int)GetLastError());
    }
    return plugin->module != NULL;
}
static DWORD WINAPI initialize_plugins(void *parameter) {
    (void)parameter;
    for (unsigned i = 0; i < plugin_count; ++i) {
        Plugin *plugin = plugins[i];
        if (plugin->early_failed || !load_plugin(plugin)) continue;
        ForzaPluginInitializeFn init = (ForzaPluginInitializeFn)(uintptr_t)GetProcAddress(plugin->module, "ForzaPluginInitialize");
        FH2PluginInitializeFn old = (FH2PluginInitializeFn)(uintptr_t)GetProcAddress(plugin->module, "FH2PluginInitialize");
        int ok = init ? init(&plugin->v2) : old ? old(&plugin->v1) : 0;
        log_append(ok ? "PLUGINS: initialization complete.\r\n" : "PLUGINS: initialization refused or no initializer.\r\n");
    }
    return 0;
}
int injector_plugins_start(unsigned char *image, const wchar_t *executable,
                           const wchar_t *directory, unsigned title) {
    if (!image || !executable || !directory || lstrlenW(executable) >= PATH_CAPACITY ||
        lstrlenW(directory) >= PATH_CAPACITY || InterlockedExchange(&started, 1)) return 0;
    if (!forza_image_identity(image, &game_timestamp, &game_size)) return 0;
    game_image = image;
    lstrcpyW(executable_path, executable); lstrcpyW(settings_path, directory);
    if (title) {
        static const char hex[] = "0123456789ABCDEF";
        for (unsigned i = 0; i < 8; ++i) active_title[i] = hex[(title >> ((7 - i) * 4)) & 15];
        game_info = forza_game_by_title(active_title);
    } else {
        game_info = forza_game_by_image(image);
        if (game_info) memory_copy(active_title, game_info->title_id, 9);
    }
    if (!path_join(mods_path, directory, L"mods") || !discover_mods() || !discover_plugins()) {
        log_append("PLUGINS: invalid mod discovery/load order; nothing initialized.\r\n"); return 0;
    }
    for (unsigned i = 0; i < plugin_count; ++i) {
        Plugin *plugin = plugins[i];
        if (!forza_file_exports(plugin->path, "ForzaGameRuntimeInitialize")) continue;
        if (!load_plugin(plugin)) return 0;
        ForzaGameRuntimeInitializeFn init = (ForzaGameRuntimeInitializeFn)(uintptr_t)
            GetProcAddress(plugin->module, "ForzaGameRuntimeInitialize");
        if (!init) return 0;
        int result = init(&plugin->runtime);
        if (result) {
            provider_result = result;
            log_append(result > 0 ? "PLUGINS: game runtime claimed the native plugin lifecycle.\r\n"
                                 : "PLUGINS: game runtime failed; fallback suppressed.\r\n");
            return result > 0;
        }
    }
    for (unsigned i = 0; i < plugin_count; ++i) {
        Plugin *plugin = plugins[i];
        if (!forza_file_exports(plugin->path, "ForzaPluginEarlyInitialize")) continue;
        if (!load_plugin(plugin)) { plugin->early_failed = 1; continue; }
        ForzaPluginEarlyInitializeFn early = (ForzaPluginEarlyInitializeFn)(uintptr_t)
            GetProcAddress(plugin->module, "ForzaPluginEarlyInitialize");
        if (!early || !early(&plugin->v2)) { plugin->early_failed = 1; log_append("PLUGINS: early initialization refused.\r\n"); }
    }
    if (!plugin_count) return 1;
    loader_thread = CreateThread(NULL, 0, initialize_plugins, NULL, 0, NULL);
    return loader_thread != NULL;
}
#ifdef FH2_TWEAKS_TEST
__declspec(dllexport) int ForzaTestProviderResult(void) { return provider_result; }
__declspec(dllexport) unsigned ForzaTestPluginCount(void) { return plugin_count; }
__declspec(dllexport) int ForzaTestWait(unsigned timeout) {
    return !loader_thread || WaitForSingleObject(loader_thread, timeout) == WAIT_OBJECT_0;
}
__declspec(dllexport) BOOL ForzaTestFileExports(const wchar_t *path, const char *name) {
    return forza_file_exports(path, name);
}
#endif
