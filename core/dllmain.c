#include "runtime.h"
#include "support.h"

__declspec(dllexport) const char *Fh2TweaksStatus(void) {
    return status_get();
}

// forza_injector: present only in this build, so plugins tell it from stock
// Extra Options (forza_plugin.h, forza_extra_options_loaded).
__declspec(dllexport) const char *ForzaInjectorStatus(void) {
    return status_get();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        adapter_process_attach(instance);
    }
    return TRUE;
}
