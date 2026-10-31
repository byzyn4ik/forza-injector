// FH2 Extra Options plugins: guarded reads of game memory and chained hooks.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Header only, C and C++, no C runtime calls (Custom Radio links none):
//   - fh2_safe_read / fh2_bytes_equal: read or compare memory without
//     faulting (the game image, another module's code);
//   - fh2_is_code: committed, executable, not a guard page;
//   - fh2_call_target: what a "call rel32" (E8) site calls now;
//   - fh2_allocate_near: a page of free memory within 2 GB of an address;
//   - fh2_chain_call: takes over a "call rel32" site without assuming what it
//     calls. The current target (the game's function, Extra Options' thunk or
//     another plugin's jump) is handed back to the caller, which calls it from
//     its handler; the site is pointed at a jump placed within 2 GB of it
//     ("jmp [rip+0]; dq handler") by swapping only its 4-byte, 4-aligned
//     displacement (compare-exchange), so plugins that chain the same site
//     stack in any load order (Azimuth Graphics Options, Exit Game and
//     Performance chain the XAML resource call at game+0x16ebb37 with it;
//     Keyboard & Mouse chains the same call through a near jump of its own,
//     written by its own patch code with the other threads suspended);
//   - fh2_chain_slot: the same for an 8-byte function pointer (vtable slot).
#ifndef FH2_GUARD_H
#define FH2_GUARD_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#define FH2_REACH ((uintptr_t)0x7ff00000u)  // a little under 2 GB

typedef enum {
    FH2_CHAINED,
    FH2_NOT_CALL,          // the site is not E8 rel32
    FH2_TARGET_NOT_CODE,   // what it calls now is not executable code
    FH2_UNALIGNED,         // the displacement (slot) is not 4- (8-) byte aligned
    FH2_NO_NEAR_MEMORY,    // no free page within 2 GB of the site
    FH2_JUMP_FAILED,       // the jump could not be made executable
    FH2_WRITE_FAILED,      // the site could not be written
    FH2_RACED,             // another module changed the site at the same time
} Fh2ChainResult;

static inline void fh2_copy_bytes(void *output, const void *input, size_t size) {
    unsigned char *to = (unsigned char *)output;
    const unsigned char *from = (const unsigned char *)input;
    while (size--) *to++ = *from++;
}

// Reads `size` bytes at `address` without faulting; 0 if any is unreadable.
static inline int fh2_safe_read(const void *address, void *output, size_t size) {
    SIZE_T done = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), address, output, size, &done) && done == size;
}

// The `size` (at most 64) bytes at `address` are readable and `expected`.
static inline int fh2_bytes_equal(const void *address, const void *expected, size_t size) {
    unsigned char actual[64];
    const unsigned char *want = (const unsigned char *)expected;
    if (size > sizeof(actual) || !fh2_safe_read(address, actual, size)) return 0;
    for (size_t i = 0; i < size; ++i)
        if (actual[i] != want[i]) return 0;
    return 1;
}

// Committed, executable memory that is not a guard page.
static inline int fh2_is_code(const void *address) {
    MEMORY_BASIC_INFORMATION region;
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return address && VirtualQuery(address, &region, sizeof(region)) == sizeof(region) &&
           region.State == MEM_COMMIT && (region.Protect & executable) && !(region.Protect & PAGE_GUARD);
}

// What the "call rel32" at `site` calls; NULL when it is not one.
static inline void *fh2_call_target(const unsigned char *site) {
    unsigned char code[5];
    int32_t displacement;
    if (!fh2_safe_read(site, code, sizeof(code)) || code[0] != 0xe8) return NULL;
    fh2_copy_bytes(&displacement, code + 1, 4);
    return (void *)(site + 5 + displacement);
}

static inline int fh2_reachable(const void *from, const void *to) {
    intptr_t distance = (intptr_t)to - (intptr_t)from;
    return distance > -(intptr_t)FH2_REACH && distance < (intptr_t)FH2_REACH;
}

// One read-write page of free memory within reach of `around`, searched
// outwards one allocation granule at a time; NULL when there is none.
static inline unsigned char *fh2_allocate_near(const void *around) {
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    uintptr_t granularity = system.dwAllocationGranularity;
    uintptr_t center = (uintptr_t)around & ~(granularity - 1);
    uintptr_t low = center > FH2_REACH ? center - FH2_REACH : granularity;
    uintptr_t high = center + FH2_REACH;
    if ((uintptr_t)system.lpMaximumApplicationAddress < high) high = (uintptr_t)system.lpMaximumApplicationAddress;
    for (uintptr_t step = granularity; step < FH2_REACH; step += granularity) {
        for (int up = 0; up < 2; ++up) {
            uintptr_t at = up ? center + step : center - step;
            if (up ? at >= high : (center < step || at < low)) continue;
            MEMORY_BASIC_INFORMATION region;
            if (VirtualQuery((void *)at, &region, sizeof(region)) != sizeof(region) || region.State != MEM_FREE)
                continue;
            void *page = VirtualAlloc((void *)at, system.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (page && fh2_reachable(around, page)) return (unsigned char *)page;
            if (page) VirtualFree(page, 0, MEM_RELEASE);
        }
    }
    return NULL;
}

// `site` points at E8 rel32. On FH2_CHAINED, *previous is what the site
// called; it is set before the swap (the game may run the site on another
// thread at once), so the handler must only call it.
static inline Fh2ChainResult fh2_chain_call(unsigned char *site, void *handler, void **previous) {
    void *target = fh2_call_target(site);
    if (!target) return FH2_NOT_CALL;
    if (!fh2_is_code(target)) return FH2_TARGET_NOT_CODE;
    if (((uintptr_t)(site + 1) & 3) != 0) return FH2_UNALIGNED;
    unsigned char *jump = fh2_allocate_near(site);
    if (!jump) return FH2_NO_NEAR_MEMORY;
    unsigned char code[14] = {0xff, 0x25, 0, 0, 0, 0};  // jmp qword [rip+0]; dq handler
    uintptr_t address = (uintptr_t)handler;
    fh2_copy_bytes(code + 6, &address, 8);
    fh2_copy_bytes(jump, code, sizeof(code));
    DWORD protection, ignored;
    if (!VirtualProtect(jump, sizeof(code), PAGE_EXECUTE_READ, &protection)) {
        VirtualFree(jump, 0, MEM_RELEASE);
        return FH2_JUMP_FAILED;
    }
    FlushInstructionCache(GetCurrentProcess(), jump, sizeof(code));
    *previous = target;
    LONG before = (LONG)(int32_t)((unsigned char *)target - (site + 5));
    LONG after = (LONG)(int32_t)(jump - (site + 5));
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &protection)) {
        VirtualFree(jump, 0, MEM_RELEASE);
        return FH2_WRITE_FAILED;
    }
    LONG seen = InterlockedCompareExchange((volatile LONG *)(site + 1), after, before);
    VirtualProtect(site + 1, 4, protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    if (seen != before) {
        VirtualFree(jump, 0, MEM_RELEASE);
        return FH2_RACED;
    }
    return FH2_CHAINED;
}

// Swaps the 8-byte function pointer at `slot` from `expected` to `handler`.
static inline Fh2ChainResult fh2_chain_slot(void **slot, void *expected, void *handler) {
    DWORD protection, ignored;
    if (((uintptr_t)slot & 7) != 0) return FH2_UNALIGNED;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection)) return FH2_WRITE_FAILED;
    void *seen = InterlockedCompareExchangePointer(slot, handler, expected);
    VirtualProtect(slot, sizeof(void *), protection, &ignored);
    return seen == expected ? FH2_CHAINED : FH2_RACED;
}

#endif
