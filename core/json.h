#ifndef FORZA_JSON_H
#define FORZA_JSON_H
#include "host.h"

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonKind;
typedef struct JsonValue {
    JsonKind kind;
    size_t length;
    char *text, *name;
    struct JsonValue *child, *next;
} JsonValue;
typedef struct {
    char *source;
    void *blocks;
    JsonValue *root;
    const char *error;
    size_t error_offset;
    unsigned nodes;
} JsonDocument;

int json_parse(JsonDocument *document, char *owned_source, size_t size);
void json_free(JsonDocument *document);
JsonValue *json_get(const JsonValue *object, const char *name);
int json_is(const JsonValue *value, const char *text);
int json_uint(const JsonValue *value, unsigned *number);
#endif
