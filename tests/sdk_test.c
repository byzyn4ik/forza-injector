// Offline test of the SDK headers: forza_patch.h (all or nothing, accepted
// variants, already patched sites) and forza_profile.h (pattern scan, game
// identity). Prints ok / FAIL per check; exits 1 on a failure.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <string.h>

#include "forza_patch.h"
#include "forza_profile.h"
#include "forza_saves.h"

static int failures;

static void check(int ok, const char *what) {
    printf("  %s %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok) ++failures;
}

int main(void) {
    const uint32_t size = 0x3000;
    unsigned char *image = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!image) return 2;
    static const unsigned char original_a[] = {0x0f, 0x85, 0x46, 0x01, 0x00, 0x00};
    static const unsigned char patched_a[] = {0xe9, 0x47, 0x01, 0x00, 0x00, 0x90};
    static const unsigned char original_b[] = {0x40, 0x0f, 0xb6, 0xc7};
    static const unsigned char patched_b[] = {0x31, 0xc0, 0xb0, 0x01};
    static const unsigned char *const accepted_a[] = {original_a, patched_a};
    static const unsigned char *const accepted_b[] = {original_b, patched_b};
    const ForzaPatch patches[] = {
        {"a", 0x1100, sizeof(original_a), patched_a, accepted_a, 2},
        {"b", 0x2200, sizeof(original_b), patched_b, accepted_b, 2},
    };
    memcpy(image + 0x1100, original_a, sizeof(original_a));
    memcpy(image + 0x2200, original_b, sizeof(original_b));
    DWORD old;
    VirtualProtect(image, size, PAGE_EXECUTE_READ, &old);  // like the game's code

    uint32_t failed = 99;
    check(forza_patch_apply(image, size, patches, 2, &failed) == FORZA_PATCH_OK && failed == 0, "both sites applied");
    check(!memcmp(image + 0x1100, patched_a, sizeof(patched_a)) && !memcmp(image + 0x2200, patched_b, sizeof(patched_b)),
          "the patched bytes are in place");
    MEMORY_BASIC_INFORMATION info;
    VirtualQuery(image + 0x1100, &info, sizeof(info));
    check(info.Protect == PAGE_EXECUTE_READ, "the page protection is restored");
    check(forza_patch_apply(image, size, patches, 2, NULL) == FORZA_PATCH_OK, "applying again is a no-op");

    // A site with other bytes: nothing at all is written.
    VirtualProtect(image, size, PAGE_READWRITE, &old);
    memcpy(image + 0x1100, original_a, sizeof(original_a));
    image[0x2200] = 0xcc;
    VirtualProtect(image, size, PAGE_EXECUTE_READ, &old);
    check(forza_patch_apply(image, size, patches, 2, &failed) == FORZA_PATCH_UNEXPECTED && failed == 1,
          "an unexpected site refuses the whole set (index 1)");
    check(!memcmp(image + 0x1100, original_a, sizeof(original_a)), "the first site is left as it was");

    const ForzaPatch outside[] = {{"c", size - 2, sizeof(original_b), patched_b, accepted_b, 2}};
    check(forza_patch_apply(image, size, outside, 1, NULL) == FORZA_PATCH_OUTSIDE_IMAGE, "a site past the image is refused");

    // Pattern scan.
    static const unsigned char code[] = {0x90, 0x48, 0x8b, 0x05, 0x11, 0x22, 0x33, 0x44, 0xe8, 0x01};
    check(forza_find_pattern(code, sizeof(code), "48 8b ?? 11 ?? 33") == code + 1, "a pattern with wildcards matches");
    check(forza_find_pattern(code, sizeof(code), "48 8b 06") == NULL, "a pattern that is not there does not");
    check(forza_find_pattern(code, sizeof(code), "4z") == NULL, "a malformed pattern does not match");

    // Game identity of a PE header.
    unsigned char header[0x200] = {'M', 'Z'};
    *(uint32_t *)(header + 0x3c) = 0x80;
    memcpy(header + 0x80, "PE\0\0", 4);
    *(uint32_t *)(header + 0x80 + 8) = 0x55b62fdeu;
    *(uint32_t *)(header + 0x80 + 24 + 56) = 0x3a89000u;
    const ForzaGameInfo *game = forza_game_by_image(header);
    check(game && game->game == FORZA_GAME_FH2FF && !strcmp(game->title_id, "5A9771A4"),
          "Spire's TimeDateStamp and SizeOfImage are Fast & Furious");
    *(uint32_t *)(header + 0x80 + 8) = 0x565501edu;
    check(forza_game_by_image(header) == NULL, "FH2's stamp with Spire's size is no known build");
    *(uint32_t *)(header + 0x80 + 8) = 0x53d8d1f2u;
    *(uint32_t *)(header + 0x80 + 24 + 56) = 0x279d000u;
    check(forza_game_by_image(header) && forza_game_by_image(header)->game == FORZA_GAME_FM5,
          "Forza Motorsport 5 1.0.0.20 is known by its image");
    *(uint32_t *)(header + 0x80 + 8) = 0x58afb1cdu;
    *(uint32_t *)(header + 0x80 + 24 + 56) = 0x1fc8000u;
    check(forza_game_by_image(header) && forza_game_by_image(header)->game == FORZA_GAME_FM6 &&
          !strcmp(forza_game_by_image(header)->package, "Monument"),
          "Forza Motorsport 6 1.0.0.44 is known by its image");
    check(forza_game_by_title("1a96545b") && forza_game_by_title("1a96545b")->game == FORZA_GAME_FH2,
          "a title id matches regardless of case");

    // forza_saves.h without the injector: Azimuth's own folder beside the executable.
    wchar_t saves[1024], expected[1024];
    DWORD count = GetModuleFileNameW(NULL, expected, 1000);
    while (count && expected[count - 1] != L'\\') --count;
    lstrcpyW(expected + count, L"data\\titles\\5A9771A4\\saves");
    check(!forza_injector_module() && !forza_redirected_saves(L"5A9771A4", saves, 1024),
          "forza_saves.h: no injector, no redirect");
    check(forza_title_saves(L"5A9771A4", saves, 1024) == (size_t)lstrlenW(expected) && !lstrcmpW(saves, expected),
          "forza_saves.h: the game-folder saves beside the executable");
    check(!forza_title_saves(L"5A9771A4", saves, 10), "forza_saves.h: a small buffer gives 0");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
