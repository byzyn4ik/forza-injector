// FH2 Extra Options plugins: the game's text language and the plugin's INI.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Header only, C and C++, no C runtime calls.
#ifndef FH2_GAME_H
#define FH2_GAME_H
#include "fh2_guard.h"

// The game's text table manager (forza_durango_release_final.exe, image size
// 0x3a18000); its language code is a std::string at +0x30. The same read
// Extra Options and its Weather Control plugin use.
#define FH2_TEXT_TABLES 0x3662990u

// The language code the game's text uses ("EN", "PL", "CHT", ...: 2 or 3
// letters, upper case) into code[4]; its length, or 0 (code empty) when it
// cannot be read.
static inline size_t fh2_game_language(const unsigned char *game, char code[4]) {
    void *manager = NULL;
    unsigned char text[32];
    size_t length, capacity;
    code[0] = 0;
    if (!game || !fh2_safe_read(game + FH2_TEXT_TABLES, &manager, sizeof(manager)) || !manager ||
        !fh2_safe_read((const unsigned char *)manager + 0x30, text, sizeof(text)))
        return 0;
    fh2_copy_bytes(&length, text + 16, sizeof(length));
    fh2_copy_bytes(&capacity, text + 24, sizeof(capacity));
    // An inline (short) std::string of 2 or 3 letters.
    if (capacity != 15 || length < 2 || length > 3 || text[length] != 0) return 0;
    for (size_t i = 0; i < length; ++i) {
        char c = (char)text[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c < 'A' || c > 'Z') {
            code[0] = 0;
            return 0;
        }
        code[i] = c;
    }
    code[length] = 0;
    return length;
}

// The plugin's INI: its DLL path with the extension replaced by ".ini"
// (".ini" appended when the file name has none). 0 when it does not fit.
static inline int fh2_plugin_ini_path(const wchar_t *plugin_path, wchar_t *output, size_t capacity) {
    size_t length = 0, end;
    if (!plugin_path || !output || !capacity) return 0;
    while (plugin_path[length]) ++length;
    end = length;
    for (size_t i = length; i > 0; --i) {
        if (plugin_path[i - 1] == L'\\' || plugin_path[i - 1] == L'/') break;
        if (plugin_path[i - 1] == L'.') {
            end = i - 1;
            break;
        }
    }
    if (end + 5 > capacity) return 0;
    fh2_copy_bytes(output, plugin_path, end * sizeof(wchar_t));
    fh2_copy_bytes(output + end, L".ini", 5 * sizeof(wchar_t));
    return 1;
}

#endif
