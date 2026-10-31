#ifndef FORZA_LOGGING_H
#define FORZA_LOGGING_H
#include "host.h"
void log_set_directory(const wchar_t *directory);
void log_reset(void);
void log_append(const char *message);
void log_append_value(const char *label, int value);
#endif