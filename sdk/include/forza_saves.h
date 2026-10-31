// forza_injector: where Azimuth keeps a title's saves.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Header only, C and C++, no C runtime calls. With forza-injector.ini
// [Saves] Location=Documents the injector redirects Azimuth's
// <exe folder>\data\titles\<title>\saves to Documents\My Games\<game>\Saves\<title>
// (core/adapters/azimuth_saves.c). A plugin that reads the saves asks here
// instead of building the game-folder path itself: the game-folder copy is
// stale while the redirect is on.
#ifndef FORZA_SAVES_H
#define FORZA_SAVES_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>

// winmm.dll's export: the redirected folder of `title` (8 hex digits), its
// length; 0 when that title's saves are in Azimuth's own folder.
typedef unsigned (*ForzaInjectorSavesFolderFn)(const wchar_t *title, wchar_t *output, unsigned capacity);

// The injector's winmm.dll (the one beside the process executable that exports
// ForzaInjectorStatus), or NULL under stock Extra Options or anything else.
static inline HMODULE forza_injector_module(void) {
    wchar_t path[1024];
    DWORD count = GetModuleFileNameW(NULL, path, 1024 - 16);
    if (!count || count >= 1024 - 16) return NULL;
    while (count && path[count - 1] != L'\\' && path[count - 1] != L'/') --count;
    static const wchar_t name[] = L"winmm.dll";
    for (unsigned i = 0; i < sizeof(name) / sizeof(name[0]); ++i) path[count + i] = name[i];
    HMODULE module = GetModuleHandleW(path);
    return module && GetProcAddress(module, "ForzaInjectorStatus") ? module : NULL;
}

// The folder Azimuth's saves of `title` are redirected to, with its length;
// 0 when they are not redirected (no injector, an older one, Location=Game).
static inline size_t forza_redirected_saves(const wchar_t *title, wchar_t *output, size_t capacity) {
    HMODULE injector = forza_injector_module();
    ForzaInjectorSavesFolderFn folder =
        injector ? (ForzaInjectorSavesFolderFn)(void *)GetProcAddress(injector, "ForzaInjectorSavesFolder") : NULL;
    if (!folder || !output || !capacity || capacity > 0xffffffffu) return 0;
    return folder(title, output, (unsigned)capacity);
}

// Azimuth's saves folder of `title` in use: the redirected one, else
// <folder of the process executable>\data\titles\<title>\saves. Its length, 0 on failure.
static inline size_t forza_title_saves(const wchar_t *title, wchar_t *output, size_t capacity) {
    size_t length = forza_redirected_saves(title, output, capacity);
    if (length) return length;
    DWORD count = capacity <= 0xffffffffu ? GetModuleFileNameW(NULL, output, (DWORD)capacity) : 0;
    if (!count || count >= capacity) return 0;
    while (count && output[count - 1] != L'\\' && output[count - 1] != L'/') --count;
    static const wchar_t data[] = L"data\\titles\\";
    size_t title_length = 0;
    while (title && title[title_length]) ++title_length;
    if (!count || !title_length || count + 12 + title_length + 6 + 1 > capacity) return 0;
    for (unsigned i = 0; i < 12; ++i) output[count++] = data[i];
    for (size_t i = 0; i < title_length; ++i) output[count++] = title[i];
    static const wchar_t saves[] = L"\\saves";
    for (unsigned i = 0; i < 7; ++i) output[count + i] = saves[i];
    return count + 6;
}
#endif
