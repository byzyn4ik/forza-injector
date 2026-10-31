// Optional game runtime providers. This interface keeps game hooks out of the injector.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef FORZA_RUNTIME_H
#define FORZA_RUNTIME_H
#include "forza_plugin.h"
#define FORZA_RUNTIME_API_VERSION 1u
typedef struct ForzaRuntimeContext {
    uint32_t size, api_version;
    const ForzaContext *plugin;
    const wchar_t *game_executable;
    const wchar_t *settings_directory;
} ForzaRuntimeContext;
// Called on Azimuth's map thread before game code runs, outside this DLL's DllMain.
// 0: unsupported, no changes. 1: claimed and initialized. -1: claimed but failed;
// the host must not try another provider or its fallback loader after a failure.
// A claiming provider supplies game hooks and the deferred plugin/event lifecycle.
// Contexts, strings and modules remain alive until process exit. No hot unloading.
typedef int(WINAPI *ForzaGameRuntimeInitializeFn)(const ForzaRuntimeContext *context);
#endif
