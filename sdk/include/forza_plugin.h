// forza_injector plugin API.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Version 1 is the Extra Options plugin API, unchanged (fh2_plugin.h, upstream's
// file with LF line ends): a plugin that exports only FH2PluginInitialize
// and the other FH2Plugin* functions loads under stock Extra Options and under
// forza_injector alike.
//
// Version 2 adds ForzaPluginInitialize. forza_injector calls it in place of
// FH2PluginInitialize when a plugin exports both (Extra Options never calls
// it), so one DLL can export both and work under either loader. The other
// FH2Plugin* exports (vehicle update, database, media directory) are called
// the same way under both.
#ifndef FORZA_PLUGIN_H
#define FORZA_PLUGIN_H
#include "fh2_plugin.h"

#define FORZA_PLUGIN_API_VERSION 2u

// The games forza_injector knows (forza_profile.h has their image
// identities). FORZA_GAME_UNKNOWN: a game without a profile; the injector
// still loads mods, media and plugins there, and host-level plugins work.
enum {
    FORZA_GAME_UNKNOWN = 0,
    FORZA_GAME_FH2 = 1,    // Forza Horizon 2, title 1A96545B ("Anthem")
    FORZA_GAME_FH2FF = 2,  // Forza Horizon 2 Presents Fast & Furious, title 5A9771A4 ("Spire")
    FORZA_GAME_FH3 = 3,    // Forza Horizon 3, title 4CE1DFAB
    FORZA_GAME_FM5 = 4,    // Forza Motorsport 5, title 7B35D917
    FORZA_GAME_FM6 = 5,    // Forza Motorsport 6, title 64F9397E
};

// Events supplied by a claiming game-runtime provider. Several plugins share
// its native sites. A generic host without a game runtime returns FALSE from
// subscribe: it does not install game hooks or promise unsupported events.
enum {
    FORZA_EVENT_FILES_READY = 1,   // the game's file resolver ran for the first time (frame NULL)
    FORZA_EVENT_DATABASE = 2,      // a merged game database is ready (frame: FH2PluginDatabaseFrame)
    FORZA_EVENT_VEHICLE = 3,       // local-car update (frame: FH2PluginVehicleFrame)
};
typedef void(WINAPI *ForzaEventFn)(uint32_t event, const void *frame, void *user);

typedef struct ForzaContext {
    uint32_t size;                     // sizeof(ForzaContext) of the injector; read only fields below it
    uint32_t api_version;              // FORZA_PLUGIN_API_VERSION
    const FH2PluginContext *v1;        // everything version 1 gives (game image, paths, log)
    uint32_t game;                     // FORZA_GAME_*
    const char *game_id;               // "fh2", "fh2ff", ... ("" when unknown); OpenBox's games\<id>
    const char *title_id;              // "1A96545B" (upper case, 8 hex digits)
    uint32_t game_timestamp;           // the mapped game image's PE TimeDateStamp
    void *host_image;                  // azimuth.exe in memory
    uint32_t host_timestamp;           // its TimeDateStamp and SizeOfImage: which Azimuth build
    uint32_t host_image_size;
    const char *injector_version;
    // Calls `callback` for every `event` from now on (no unsubscribe: plugins
    // are never unloaded). FALSE for an event this game has no hook for.
    BOOL (*subscribe)(uint32_t event, ForzaEventFn callback, void *user);
} ForzaContext;

// Optional export: extern "C" __declspec(dllexport) BOOL WINAPI
// ForzaPluginInitialize(const ForzaContext *context);
// Same thread and lifetime rules as FH2PluginInitialize.
typedef BOOL(WINAPI *ForzaPluginInitializeFn)(const ForzaContext *context);

// Optional export: extern "C" __declspec(dllexport) BOOL WINAPI
// ForzaPluginEarlyInitialize(const ForzaContext *context);
// Called once the game image is mapped and before the game's code runs (where
// Extra Options applies its own patches), on Azimuth's thread, in mod load
// order. The injector finds it in the DLL's export table without loading the
// DLL, so only plugins that export it are loaded this early. Byte patches
// (forza_patch.h) belong here. The context is the same one ForzaPluginInitialize
// gets later (ForzaPluginInitialize, or else FH2PluginInitialize, still follows
// on the loader's worker thread). subscribe already works here.
typedef BOOL(WINAPI *ForzaPluginEarlyInitializeFn)(const ForzaContext *context);

// A plugin that duplicates an Extra Options feature (the Xtra Options DLLs)
// does nothing when Extra Options' winmm.dll is the loader: true when the
// process has a module exporting Extra Options' status function.
static inline BOOL forza_extra_options_loaded(void) {
    HMODULE winmm = GetModuleHandleW(L"winmm.dll");
    return winmm && GetProcAddress(winmm, "Fh2TweaksStatus") && !GetProcAddress(winmm, "ForzaInjectorStatus");
}

#endif
