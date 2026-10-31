// forza_injector: byte patches of the game image, all or nothing.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Header only, C and C++, no C runtime calls. The same rules as Extra
// Options' patch engine (core/patch_engine.c), for plugins that carry their
// own patches (the Xtra Options DLLs):
//   - every site must hold one of its accepted byte strings (the original,
//     or a known patched variant), else nothing at all is written;
//   - all sites are written together, the instruction cache flushed and the
//     pages' protection restored;
//   - a site that already holds the bytes to write is left as it is.
// Apply from ForzaPluginEarlyInitialize: the game's code has not run yet
// there, so no thread can be inside a site while it changes.
#ifndef FORZA_PATCH_H
#define FORZA_PATCH_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

typedef struct ForzaPatch {
    const char *name;                // for the log ("intro_resources_skip")
    uint32_t rva, size;              // site in the game image, and its length in bytes
    const unsigned char *patched;    // what to write (size bytes)
    const unsigned char *const *accepted;  // byte strings the site may hold before (each size bytes;
    uint32_t accepted_count;               // include the original and the patched bytes)
} ForzaPatch;

typedef enum {
    FORZA_PATCH_OK = 0,
    FORZA_PATCH_BAD_ARGUMENT,
    FORZA_PATCH_OUTSIDE_IMAGE,   // a site does not fit the image size
    FORZA_PATCH_UNEXPECTED,      // a site holds bytes that are not accepted: nothing written
    FORZA_PATCH_PROTECT_FAILED,  // a page could not be made writable: nothing written
} ForzaPatchResult;

static inline int forza_patch_equal(const unsigned char *a, const unsigned char *b, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i)
        if (a[i] != b[i]) return 0;
    return 1;
}

// Checks every site; on success writes them all. *failed (optional) gets the
// index of the first site that failed.
static inline ForzaPatchResult forza_patch_apply(unsigned char *image, uint32_t image_size, const ForzaPatch *patches,
                                                 uint32_t count, uint32_t *failed) {
    if (failed) *failed = 0;
    if (!image || !patches) return FORZA_PATCH_BAD_ARGUMENT;
    for (uint32_t i = 0; i < count; ++i) {
        const ForzaPatch *patch = &patches[i];
        if (failed) *failed = i;
        if (!patch->size || !patch->patched || !patch->accepted || patch->rva > image_size ||
            patch->size > image_size - patch->rva)
            return patch->size ? FORZA_PATCH_OUTSIDE_IMAGE : FORZA_PATCH_BAD_ARGUMENT;
        int ok = 0;
        for (uint32_t a = 0; a < patch->accepted_count && !ok; ++a)
            ok = forza_patch_equal(image + patch->rva, patch->accepted[a], patch->size);
        if (!ok) return FORZA_PATCH_UNEXPECTED;
    }
    // Every page must be writable before the first byte changes: each is
    // made writable and put back at once, so a failure leaves no trace.
    for (uint32_t i = 0; i < count; ++i) {
        DWORD previous, ignored;
        if (failed) *failed = i;
        if (!VirtualProtect(image + patches[i].rva, patches[i].size, PAGE_EXECUTE_READWRITE, &previous))
            return FORZA_PATCH_PROTECT_FAILED;
        VirtualProtect(image + patches[i].rva, patches[i].size, previous, &ignored);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const ForzaPatch *patch = &patches[i];
        if (forza_patch_equal(image + patch->rva, patch->patched, patch->size)) continue;
        DWORD previous, ignored;
        VirtualProtect(image + patch->rva, patch->size, PAGE_EXECUTE_READWRITE, &previous);
        for (uint32_t b = 0; b < patch->size; ++b) image[patch->rva + b] = patch->patched[b];
        VirtualProtect(image + patch->rva, patch->size, previous, &ignored);
    }
    FlushInstructionCache(GetCurrentProcess(), image, image_size);
    if (failed) *failed = 0;
    return FORZA_PATCH_OK;
}

static inline const char *forza_patch_result_text(ForzaPatchResult result) {
    switch (result) {
    case FORZA_PATCH_OK: return "applied";
    case FORZA_PATCH_BAD_ARGUMENT: return "bad patch definition";
    case FORZA_PATCH_OUTSIDE_IMAGE: return "a site is outside the game image";
    case FORZA_PATCH_UNEXPECTED: return "a site holds unexpected bytes; nothing written";
    case FORZA_PATCH_PROTECT_FAILED: return "a page could not be made writable; nothing written";
    }
    return "?";
}

#endif
