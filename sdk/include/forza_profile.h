// forza_injector: which game is running, and finding code in it.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Header only, C and C++, no C runtime calls.
//   - forza_game_by_title / forza_game_by_image: the FORZA_GAME_* of a title
//     id or of a mapped game image (TimeDateStamp + SizeOfImage);
//   - forza_find_pattern: the first match of a byte pattern ("48 8b ?? e8")
//     in a range, for plugins that find their sites in more than one build
//     instead of carrying an address table per game.
// Keep kForzaGames in step with profiles/*.json (tools/validate.py checks it).
#ifndef FORZA_PROFILE_H
#define FORZA_PROFILE_H
#include "forza_plugin.h"

typedef struct ForzaGameInfo {
    uint32_t game;          // FORZA_GAME_*
    const char *id;         // OpenBox's games\<id>
    const char *title_id;   // Xbox title id, 8 upper-case hex digits
    const char *package;    // the package's Identity Name
    uint32_t timestamp;     // the supported game EXE's TimeDateStamp (0: no build supported yet)
    uint32_t image_size;    // and SizeOfImage
} ForzaGameInfo;

static const ForzaGameInfo kForzaGames[] = {
    {FORZA_GAME_FH2, "fh2", "1A96545B", "Anthem", 0x565501edu, 0x3a18000u},
    {FORZA_GAME_FH2FF, "fh2ff", "5A9771A4", "Spire", 0x55b62fdeu, 0x3a89000u},
    {FORZA_GAME_FH3, "fh3", "4CE1DFAB", "", 0, 0},
    {FORZA_GAME_FM5, "fm5", "7B35D917", "Forza", 0x53d8d1f2u, 0x279d000u},
    {FORZA_GAME_FM6, "fm6", "64F9397E", "Monument", 0x58afb1cdu, 0x1fc8000u},
};
#define FORZA_GAME_COUNT (sizeof(kForzaGames) / sizeof(kForzaGames[0]))

static inline int forza_hex_equal(const char *a, const char *b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static inline const ForzaGameInfo *forza_game_info(uint32_t game) {
    for (size_t i = 0; i < FORZA_GAME_COUNT; ++i)
        if (kForzaGames[i].game == game) return &kForzaGames[i];
    return NULL;
}

static inline const ForzaGameInfo *forza_game_by_title(const char *title_id) {
    if (!title_id) return NULL;
    for (size_t i = 0; i < FORZA_GAME_COUNT; ++i)
        if (forza_hex_equal(kForzaGames[i].title_id, title_id)) return &kForzaGames[i];
    return NULL;
}

// The PE TimeDateStamp and SizeOfImage of a mapped image; 0 when it has no
// readable PE header.
static inline int forza_image_identity(const void *image, uint32_t *timestamp, uint32_t *image_size) {
    const unsigned char *base = (const unsigned char *)image;
    if (!base || base[0] != 'M' || base[1] != 'Z') return 0;
    uint32_t pe = *(const uint32_t *)(base + 0x3c);
    if (pe > 0x1000 || base[pe] != 'P' || base[pe + 1] != 'E' || base[pe + 2] || base[pe + 3]) return 0;
    *timestamp = *(const uint32_t *)(base + pe + 8);
    *image_size = *(const uint32_t *)(base + pe + 24 + 56);  // OptionalHeader.SizeOfImage
    return 1;
}

// The game whose supported build this image is (exact TimeDateStamp and
// SizeOfImage), else NULL: another build of a known title gets no game-level
// features until it has a profile.
static inline const ForzaGameInfo *forza_game_by_image(const void *image) {
    uint32_t timestamp, image_size;
    if (!forza_image_identity(image, &timestamp, &image_size)) return NULL;
    for (size_t i = 0; i < FORZA_GAME_COUNT; ++i)
        if (kForzaGames[i].timestamp && kForzaGames[i].timestamp == timestamp &&
            kForzaGames[i].image_size == image_size)
            return &kForzaGames[i];
    return NULL;
}

// Parses "48 8b ?? e8" into bytes and a mask (0 = wildcard); the length, or 0
// when the pattern is malformed or longer than capacity.
static inline size_t forza_parse_pattern(const char *text, unsigned char *bytes, unsigned char *mask, size_t capacity) {
    size_t length = 0;
    while (*text) {
        if (*text == ' ') { ++text; continue; }
        if (length == capacity) return 0;
        if (text[0] == '?') {
            bytes[length] = 0;
            mask[length++] = 0;
            text += text[1] == '?' ? 2 : 1;
            continue;
        }
        unsigned value = 0;
        for (int i = 0; i < 2; ++i) {
            char c = text[i];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') value |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= (unsigned)(c - 'A' + 10);
            else return 0;
        }
        bytes[length] = (unsigned char)value;
        mask[length++] = 1;
        text += 2;
    }
    return length;
}

// The first match of `pattern` in [begin, begin + size), else NULL. The range
// must be readable (an image's code section). Patterns up to 64 bytes.
static inline const unsigned char *forza_find_pattern(const void *begin, size_t size, const char *pattern) {
    unsigned char bytes[64], mask[64];
    size_t length = forza_parse_pattern(pattern, bytes, mask, sizeof(bytes));
    const unsigned char *data = (const unsigned char *)begin;
    if (!length || !data || size < length) return NULL;
    for (size_t at = 0; at + length <= size; ++at) {
        size_t i = 0;
        while (i < length && (!mask[i] || data[at + i] == bytes[i])) ++i;
        if (i == length) return data + at;
    }
    return NULL;
}

#endif
