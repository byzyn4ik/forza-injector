// Game-independent host. Native game hooks belong to optional runtime plugins.
#include "runtime.h"
#include "plugins.h"
#include "logging.h"
#include "support.h"
#include "adapters/azimuth_saves.h"
int runtime_initialize_title(unsigned char *game, const wchar_t *executable,
                             const wchar_t *directory, unsigned title) {
    log_set_directory(directory);
    log_reset();
    saves_location_log_ready();
    log_append("forza_injector " MOD_VERSION ": Azimuth game mapped.\r\n");
    if (!injector_plugins_start(game, executable, directory, title)) {
        status_set("REFUSED: plugin runtime initialization failed. See forza-injector.log.");
        return 0;
    }
    status_set("OK: game-independent plugin host initialized.");
    return 1;
}
int runtime_initialize(unsigned char *game, const wchar_t *executable, const wchar_t *directory) {
    return runtime_initialize_title(game, executable, directory, 0);
}
#ifdef FH2_TWEAKS_TEST
__declspec(dllexport) int Fh2TestInitialize(void *base, const wchar_t *path, const wchar_t *directory) {
    return runtime_initialize(base, path, directory);
}
__declspec(dllexport) int ForzaTestInitialize(void *base, const wchar_t *path,
                                          const wchar_t *directory, unsigned title) {
    return runtime_initialize_title(base, path, directory, title);
}
#endif