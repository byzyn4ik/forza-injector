#ifndef FH2_RUNTIME_H
#define FH2_RUNTIME_H
#include "host.h"
int runtime_initialize(unsigned char *game_image, const wchar_t *executable,
                       const wchar_t *directory);
void adapter_process_attach(HINSTANCE instance);
int runtime_initialize_title(unsigned char *game, const wchar_t *executable,
                             const wchar_t *directory, unsigned title);
#endif
