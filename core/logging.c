#include "logging.h"
#include "support.h"
static wchar_t log_path[PATH_CAPACITY];
void log_set_directory(const wchar_t *directory) {
    if (!path_join(log_path, directory, L"forza-injector.log")) log_path[0] = 0;
}
void log_reset(void) {
    if (!log_path[0]) return;
    HANDLE file = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}
void log_append(const char *message) {
    if (!log_path[0]) return;
    HANDLE file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD count;
    WriteFile(file, message, (DWORD)text_length(message), &count, NULL);
    CloseHandle(file);
}
void log_append_value(const char *label, int value) {
    char digits[12], number[14];
    unsigned magnitude = value < 0 ? 0u - (unsigned)value : (unsigned)value;
    unsigned count = 0, out = 0;
    do { digits[count++] = (char)('0' + magnitude % 10); magnitude /= 10; } while (magnitude);
    if (value < 0) number[out++] = '-';
    while (count) number[out++] = digits[--count];
    number[out++] = '\r'; number[out++] = '\n'; number[out] = 0;
    log_append(label); log_append(number);
}