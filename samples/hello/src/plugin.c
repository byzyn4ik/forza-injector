// Hello: the smallest plugin that works under stock Extra Options (version 1)
// and under forza_injector (version 2). It only logs what it was given.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "forza_profile.h"

static void log_line(void (*log_utf8)(const char *), const char *a, const char *b) {
    char line[160];
    size_t n = 0;
    for (const char *p = a; *p && n + 1 < sizeof(line); ++p) line[n++] = *p;
    for (const char *p = b; p && *p && n + 1 < sizeof(line); ++p) line[n++] = *p;
    line[n] = 0;
    if (log_utf8) log_utf8(line);
}

// Version 1: Extra Options calls this. The game is whatever image it mapped.
__declspec(dllexport) BOOL WINAPI FH2PluginInitialize(const FH2PluginContext *context) {
    if (!context || context->size < sizeof(FH2PluginContext)) return FALSE;
    const ForzaGameInfo *game = forza_game_by_image(context->game_image);
    log_line(context->log_utf8, "Hello (v1): game ", game ? game->id : "unknown");
    return TRUE;
}

// Version 2: forza_injector calls this instead when it is the loader.
__declspec(dllexport) BOOL WINAPI ForzaPluginInitialize(const ForzaContext *context) {
    if (!context || context->size < sizeof(ForzaContext) || !context->v1) return FALSE;
    log_line(context->v1->log_utf8, "Hello (v2): game ", context->game_id);
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
