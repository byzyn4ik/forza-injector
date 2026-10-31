// Fixtures for lifecycle, dual ABI preference and runtime-provider isolation.
#include "forza_runtime.h"
#ifndef PROBE_NAME
#define PROBE_NAME "probe"
#endif
static DWORD early_thread;
#ifdef PROVIDER_RESULT
__declspec(dllexport) int WINAPI ForzaGameRuntimeInitialize(const ForzaRuntimeContext *context) {
    if (!context || context->size < sizeof(*context) || context->api_version != FORZA_RUNTIME_API_VERSION ||
        !context->plugin || !context->game_executable || !context->settings_directory) return -1;
    context->plugin->v1->log_utf8(PROBE_NAME ":provider\r\n");
    return PROVIDER_RESULT;
}
#endif
#ifdef EARLY
__declspec(dllexport) BOOL WINAPI ForzaPluginEarlyInitialize(const ForzaContext *context) {
    early_thread = GetCurrentThreadId();
    context->v1->log_utf8(PROBE_NAME ":early\r\n");
#ifdef EARLY_FAIL
    return FALSE;
#else
    return TRUE;
#endif
}
#endif
#ifndef LEGACY_ONLY
static void WINAPI observe(uint32_t event, const void *frame, void *user) {
    (void)event; (void)frame; (void)user;
}
__declspec(dllexport) BOOL WINAPI ForzaPluginInitialize(const ForzaContext *context) {
    if (!context || context->size < sizeof(*context) || !context->v1) return FALSE;
    context->v1->log_utf8(PROBE_NAME ":v2\r\n");
    context->v1->log_utf8(context->game_id);
    context->v1->log_utf8("|");
    context->v1->log_utf8(context->title_id);
    context->v1->log_utf8("\r\n");
    if (early_thread && early_thread == GetCurrentThreadId()) return FALSE;
    if (early_thread) context->v1->log_utf8(PROBE_NAME ":worker\r\n");
    if (context->subscribe(FORZA_EVENT_VEHICLE, observe, NULL)) return FALSE;
    return TRUE;
}
#endif
__declspec(dllexport) BOOL WINAPI FH2PluginInitialize(const FH2PluginContext *context) {
    if (!context || context->size < sizeof(*context)) return FALSE;
    context->log_utf8(PROBE_NAME ":v1\r\n");
    return TRUE;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
