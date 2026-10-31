#ifndef FORZA_HOST_PLUGINS_H
#define FORZA_HOST_PLUGINS_H
#include "host.h"
int injector_plugins_start(unsigned char *image, const wchar_t *executable,
                           const wchar_t *directory, unsigned title);
#endif