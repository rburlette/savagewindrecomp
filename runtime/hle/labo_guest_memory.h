// Small Windows-only diagnostic copy guard. Never touches reserved/unmapped guest pages.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

// Optional per-translation-unit attribution; ordinary callers compile it out.
enum { LABO_GUEST_QUERY, LABO_GUEST_READ, LABO_GUEST_WRITE,
       LABO_GUEST_WORKING_SET, LABO_GUEST_TIMING_OPERATIONS };
#ifndef LABO_GUEST_TIMING_BEGIN
#define LABO_GUEST_TIMING_BEGIN() UINT64_C(0)
#define LABO_GUEST_TIMING_END(operation, start, ok) ((void)(start))
#define LABO_GUEST_TIMING_REJECT() ((void)0)
#endif

#ifdef _WIN32
// The platform layer bumps this before and after any change that can take access away from guest pages
// (platform/windows.c: plat_protect, plat_reset_guest, the debug watchpoint). Otherwise guest pages are only ever
// committed while the game runs, so a page found accessible stays so until the next bump.
uint64_t plat_guest_access_generation(void);

// Pages found accessible in a generation, per thread and translation unit (direct-mapped by page number). A hit
// copies with memcpy: the OS queries and Read/WriteProcessMemory cost microseconds per call, thousands of times
// per frame from the PC VR hooks.
enum { LABO_GUEST_CACHE_PAGES = 256 };
typedef struct { uint64_t page, generation; bool writable; } LaboGuestCachedPage;
static _Thread_local LaboGuestCachedPage labo_guest_cache[LABO_GUEST_CACHE_PAGES];

static uint64_t labo_guest_page_bytes(void) {
    static uint64_t bytes;
    if (!bytes) {
        SYSTEM_INFO system_info;
        GetSystemInfo(&system_info);
        bytes = system_info.dwPageSize ? system_info.dwPageSize : 4096;
    }
    return bytes;
}

static void labo_guest_remember(uint64_t first, uint64_t pages, uint64_t page_bytes, uint64_t generation, bool write) {
    for (uint64_t i = 0; i < pages; ++i) {
        const uint64_t page = first / page_bytes + i;
        LaboGuestCachedPage *cached = &labo_guest_cache[page % LABO_GUEST_CACHE_PAGES];
        if (cached->page == page && cached->generation == generation && cached->writable)
            continue;  // keep a writable entry
        *cached = (LaboGuestCachedPage){.page = page, .generation = generation, .writable = write};
    }
}

static bool labo_guest_permission(DWORD protection, bool write) {
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD access = protection & 0xff;  // (the access kind, without the modifier bits)
    const bool writable = access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
                          access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
    return writable || (!write && (access == PAGE_READONLY || access == PAGE_EXECUTE_READ));
}

static bool labo_guest_region(uint64_t at, bool write, uint64_t *end) {
    MEMORY_BASIC_INFORMATION mi;
    const uint64_t timing_start = LABO_GUEST_TIMING_BEGIN();
    const SIZE_T queried = VirtualQuery((void *)(uintptr_t)at, &mi, sizeof mi);
    LABO_GUEST_TIMING_END(LABO_GUEST_QUERY, timing_start, queried == sizeof mi);
    if (queried != sizeof mi || mi.State != MEM_COMMIT ||
        !labo_guest_permission(mi.Protect, write)) return false;
    const uint64_t base = (uint64_t)(uintptr_t)mi.BaseAddress;
    const uint64_t size = (uint64_t)mi.RegionSize;
    if (base > at || size <= at - base || size > UINT64_MAX - base) return false;
    *end = base + size;
    return true;
}
#endif

static bool labo_guest_copy(uint64_t address, void *buffer, size_t bytes, bool write) {
    if (!buffer || !bytes || address < GUEST_LO || address >= GUEST_HI || bytes > GUEST_HI - address)
        goto rejected;
#ifdef _WIN32
    const uint64_t page_bytes = labo_guest_page_bytes();
    const uint64_t first = address - address % page_bytes;
    const uint64_t pages = (address + bytes - 1 - first) / page_bytes + 1;
    // Read before validating: a bump during validation leaves the entries already expired.
    const uint64_t generation = plat_guest_access_generation();
    bool cached = pages <= 16;
    for (uint64_t i = 0; cached && i < pages; ++i) {
        const uint64_t page = first / page_bytes + i;
        const LaboGuestCachedPage *entry = &labo_guest_cache[page % LABO_GUEST_CACHE_PAGES];
        cached = entry->page == page && entry->generation == generation && (!write || entry->writable);
    }
    if (cached) {
        if (write) memcpy((void *)(uintptr_t)address, buffer, bytes);
        else memcpy(buffer, (const void *)(uintptr_t)address, bytes);
        return true;
    }
    // VirtualQuery scans the remaining same-protection region, which can cost
    // milliseconds inside the multi-GiB guest heap. Query only the pages of a
    // small copy instead: fresh OS attributes, cached only for this generation.
    if (pages <= 16) {
        PSAPI_WORKING_SET_EX_INFORMATION info[16] = {0};
        for (uint64_t i = 0; i < pages; ++i)
            info[i].VirtualAddress = (void *)(uintptr_t)(first + i * page_bytes);
        const uint64_t timing_start = LABO_GUEST_TIMING_BEGIN();
        const bool queried = QueryWorkingSetEx(GetCurrentProcess(), info,
                                               (DWORD)(pages * sizeof info[0]));
        LABO_GUEST_TIMING_END(LABO_GUEST_WORKING_SET, timing_start, queried);
        for (uint64_t i = 0; i < pages; ++i) {
            if (queried && info[i].VirtualAttributes.Valid) {
                if (!labo_guest_permission((DWORD)info[i].VirtualAttributes.Win32Protection, write))
                    goto rejected;
            } else {
                // Nonresident, guarded and uncommitted pages can all have
                // invalid attributes. Only the old full check may admit them.
                uint64_t end;
                if (!labo_guest_region(first + i * page_bytes, write, &end)) goto rejected;
            }
        }
    } else {
        // Larger callers retain the original region walk without stack growth.
        for (uint64_t at = address; at < address + bytes;)
            if (!labo_guest_region(at, write, &at)) goto rejected;
    }
    SIZE_T copied = 0;
    const uint64_t timing_start = LABO_GUEST_TIMING_BEGIN();
    bool ok = write ? WriteProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)address, buffer, bytes, &copied)
                    : ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)address, buffer, bytes, &copied);
    LABO_GUEST_TIMING_END(write ? LABO_GUEST_WRITE : LABO_GUEST_READ,
                         timing_start, ok && copied == bytes);
    if (ok && copied == bytes && pages <= 16) labo_guest_remember(first, pages, page_bytes, generation, write);
    return ok && copied == bytes;
#else
    (void)write;
#endif
rejected:
    LABO_GUEST_TIMING_REJECT();
    return false;
}
