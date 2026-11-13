// Asset-free checks of the real guarded copy helper, including fresh page
// permissions and the existing VirtualQuery fallback and RPM/WPM copy.
// --benchmark additionally compares 36-byte copies in large committed regions;
// --benchmark-dense first touches each page (up to 1 GiB) to populate its PTEs.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>

#define GUEST_LO UINT64_C(0x7000000000)
#define GUEST_HI UINT64_C(0x7320000000)
typedef struct { unsigned working_set, resident, fallback; } ProbeCounts;
static ProbeCounts *active_counts;
static bool fail_working_set;
static BOOL WINAPI fixture_working_set(HANDLE process, PVOID info, DWORD bytes) {
    if (active_counts) ++active_counts->working_set;
    const BOOL ok = !fail_working_set && QueryWorkingSetEx(process, info, bytes);
    if (ok && active_counts) {
        const PSAPI_WORKING_SET_EX_INFORMATION *pages = info;
        for (DWORD i = 0; i < bytes / sizeof *pages; ++i)
            active_counts->resident += !!pages[i].VirtualAttributes.Valid;
    }
    return ok;
}
static SIZE_T WINAPI fixture_virtual_query(LPCVOID address,
                                           PMEMORY_BASIC_INFORMATION out, SIZE_T bytes) {
    if (active_counts) ++active_counts->fallback;
    return VirtualQuery(address, out, bytes);
}
#undef QueryWorkingSetEx
#define QueryWorkingSetEx fixture_working_set
#define VirtualQuery fixture_virtual_query
#include "../runtime/hle/labo_guest_memory.h"
#undef VirtualQuery
#undef QueryWorkingSetEx
#if PSAPI_VERSION > 1
#define QueryWorkingSetEx K32QueryWorkingSetEx
#endif

// The platform's access generation: a new value per call keeps the cache cold (the checks below count every OS
// query); cache_hits() holds it steady as the platform does between protection changes.
static bool stable_generation;
static uint64_t access_generation = 1;
uint64_t plat_guest_access_generation(void) { return stable_generation ? access_generation : ++access_generation; }

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "guest-memory check failed line %d: %s (error=%lu)\n", \
            __LINE__, #x, GetLastError()); exit(1); \
} } while (0)

static size_t page_bytes;

static bool probe_copy(uint64_t address, void *buffer, size_t bytes,
                       bool write, ProbeCounts *counts) {
    active_counts = counts;
    const bool ok = labo_guest_copy(address, buffer, bytes, write);
    active_counts = NULL;
    return ok;
}

// Retained only as the measured pre-change baseline; correctness exercises
// labo_guest_copy above rather than a duplicate of its implementation.
static bool baseline_copy(uint64_t address, void *buffer, size_t bytes, bool write) {
    if (!buffer || !bytes || address < GUEST_LO || address >= GUEST_HI ||
        bytes > GUEST_HI - address) return false;
    for (uint64_t at = address; at < address + bytes;) {
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery((void *)(uintptr_t)at, &mi, sizeof mi) != sizeof mi ||
            mi.State != MEM_COMMIT || !labo_guest_permission(mi.Protect, write)) return false;
        const uint64_t base = (uint64_t)(uintptr_t)mi.BaseAddress;
        const uint64_t size = (uint64_t)mi.RegionSize;
        if (base > at || size <= at - base || size > UINT64_MAX - base) return false;
        at = base + size;
    }
    SIZE_T copied = 0;
    const bool ok = write ?
        WriteProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)address, buffer, bytes, &copied) :
        ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)address, buffer, bytes, &copied);
    return ok && copied == bytes;
}

static void correctness(void) {
    const uint64_t address = GUEST_LO + UINT64_C(0x20000000);
    const size_t allocation_bytes = 5 * page_bytes;
    void *allocation = VirtualAlloc((void *)(uintptr_t)address, allocation_bytes,
                                   MEM_RESERVE, PAGE_READWRITE);
    CHECK(allocation == (void *)(uintptr_t)address);
    CHECK(VirtualAlloc(allocation, 4 * page_bytes, MEM_COMMIT, PAGE_READWRITE) == allocation);
    unsigned char out[36] = {0}, input[36];
    memset(input, 0x5a, sizeof input);
    memcpy(allocation, input, sizeof input); // Make the first page resident.
    ProbeCounts counts = {0};
    CHECK(probe_copy(address, out, sizeof out, false, &counts));
    CHECK(!memcmp(out, input, sizeof out));
    CHECK(counts.working_set == 1 && counts.resident == 1 && !counts.fallback);
    CHECK(probe_copy(address, input, sizeof input, true, &counts));
    CHECK(counts.resident == 2 && !counts.fallback);

    // A freshly committed untouched page has no valid working-set attributes.
    counts = (ProbeCounts){0};
    CHECK(probe_copy(address + page_bytes, out, sizeof out, false, &counts));
    CHECK(counts.working_set == 1 && counts.fallback == 1 && !counts.resident);
    unsigned char zero[36] = {0};
    CHECK(!memcmp(out, zero, sizeof out));

    DWORD prior;
    MEMORY_BASIC_INFORMATION mi;
    CHECK(VirtualProtect(allocation, page_bytes, PAGE_READONLY, &prior));
    counts = (ProbeCounts){0};
    CHECK(probe_copy(address, out, sizeof out, false, &counts));
    CHECK(!probe_copy(address, zero, sizeof zero, true, &counts));
    CHECK(!memcmp(allocation, input, sizeof input));
    CHECK(VirtualQuery(allocation, &mi, sizeof mi) == sizeof mi && mi.Protect == PAGE_READONLY);
    CHECK(counts.resident == 2 && !counts.fallback);
    CHECK(VirtualProtect(allocation, page_bytes, PAGE_READWRITE, &prior));

    const uint64_t guarded = address + 2 * page_bytes;
    *(volatile unsigned char *)(uintptr_t)guarded = 0x42;
    CHECK(VirtualProtect((void *)(uintptr_t)guarded, page_bytes,
                         PAGE_READWRITE | PAGE_GUARD, &prior));
    counts = (ProbeCounts){0};
    CHECK(!probe_copy(guarded, out, sizeof out, false, &counts));
    CHECK(!probe_copy(guarded, input, sizeof input, true, &counts));
    CHECK(VirtualQuery((void *)(uintptr_t)guarded, &mi, sizeof mi) == sizeof mi &&
          (mi.Protect & PAGE_GUARD));
    printf("guard rejection: working_set=%u resident=%u fallback=%u guard_preserved=1\n",
           counts.working_set, counts.resident, counts.fallback);

    // Crossing into a rejected page must reject the full operation before any
    // copy. The first bytes are unchanged even for an attempted spanning write.
    const uint64_t crossing = guarded - 4;
    memset((void *)(uintptr_t)crossing, 0x17, 4);
    counts = (ProbeCounts){0};
    CHECK(!probe_copy(crossing, out, sizeof out, false, &counts));
    CHECK(!probe_copy(crossing, input, sizeof input, true, &counts));
    CHECK(*(uint32_t *)(uintptr_t)crossing == UINT32_C(0x17171717));
    CHECK(VirtualQuery((void *)(uintptr_t)guarded, &mi, sizeof mi) == sizeof mi &&
          (mi.Protect & PAGE_GUARD));
    CHECK(counts.working_set == 2 && counts.resident == 2 && counts.fallback == 2);

    CHECK(VirtualProtect((void *)(uintptr_t)guarded, page_bytes, PAGE_READONLY, &prior));
    counts = (ProbeCounts){0};
    CHECK(probe_copy(crossing, out, sizeof out, false, &counts));
    CHECK(!probe_copy(crossing, input, sizeof input, true, &counts));
    CHECK(*(uint32_t *)(uintptr_t)crossing == UINT32_C(0x17171717));
    CHECK(*(unsigned char *)(uintptr_t)guarded == 0x42);
    CHECK(VirtualQuery((void *)(uintptr_t)guarded, &mi, sizeof mi) == sizeof mi &&
          mi.Protect == PAGE_READONLY);

    CHECK(VirtualProtect((void *)(uintptr_t)guarded, page_bytes, PAGE_NOACCESS, &prior));
    counts = (ProbeCounts){0};
    CHECK(!probe_copy(guarded, out, sizeof out, false, &counts));
    CHECK(!probe_copy(guarded, input, sizeof input, true, &counts));
    CHECK(counts.fallback == 2 && !counts.resident);
    CHECK(!probe_copy(crossing, input, sizeof input, true, &counts));
    CHECK(*(uint32_t *)(uintptr_t)crossing == UINT32_C(0x17171717));
    CHECK(VirtualQuery((void *)(uintptr_t)guarded, &mi, sizeof mi) == sizeof mi &&
          mi.Protect == PAGE_NOACCESS);

    const uint64_t reserved = address + 4 * page_bytes;
    counts = (ProbeCounts){0};
    CHECK(!probe_copy(reserved, out, sizeof out, false, &counts));
    CHECK(counts.fallback == 1 && !counts.resident);
    CHECK(VirtualQuery((void *)(uintptr_t)reserved, &mi, sizeof mi) == sizeof mi &&
          mi.State == MEM_RESERVE);

    // An API-level working-set query failure also takes the existing check.
    fail_working_set = true;
    counts = (ProbeCounts){0};
    CHECK(probe_copy(address, out, sizeof out, false, &counts));
    CHECK(counts.fallback == 1 && !counts.resident);
    CHECK(!memcmp(out, input, sizeof out));
    CHECK(!probe_copy(guarded, out, sizeof out, false, &counts));
    fail_working_set = false;
    CHECK(VirtualFree(allocation, 0, MEM_RELEASE));
    printf("guest memory resident-page helper: %u checks passed\n", checks);
}

// Pages found accessible are copied with memcpy and no OS query until the generation changes; a write needs its
// own validation; a protection change with the platform's bump is seen at once; slot collisions only cost a query.
static void cache_hits(void) {
    const uint64_t address = GUEST_LO + UINT64_C(0x30000000);
    const size_t span = (LABO_GUEST_CACHE_PAGES + 1) * page_bytes;
    void *allocation = VirtualAlloc((void *)(uintptr_t)address, span, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(allocation == (void *)(uintptr_t)address);
    memset(allocation, 0x33, 64);
    stable_generation = true;
    unsigned char out[16] = {0}, input[16];
    memset(input, 0x6c, sizeof input);
    ProbeCounts counts = {0};
    CHECK(probe_copy(address, out, sizeof out, false, &counts) && counts.working_set == 1);
    CHECK(probe_copy(address + 8, out, sizeof out, false, &counts) && counts.working_set == 1); // hit
    CHECK(out[0] == 0x33);
    CHECK(probe_copy(address, input, sizeof input, true, &counts) && counts.working_set == 2); // write: validated
    CHECK(probe_copy(address + 16, input, sizeof input, true, &counts) && counts.working_set == 2); // hit
    CHECK(probe_copy(address + 16, out, sizeof out, false, &counts) && counts.working_set == 2 && out[0] == 0x6c);
    // A page whose slot another page took is validated again, then hits.
    const uint64_t alias = address + LABO_GUEST_CACHE_PAGES * page_bytes;
    CHECK(probe_copy(alias, out, sizeof out, false, &counts) && counts.working_set == 3);
    CHECK(probe_copy(address, out, sizeof out, false, &counts) && counts.working_set == 4);
    CHECK(probe_copy(address, out, sizeof out, false, &counts) && counts.working_set == 4);
    // Access taken away with the platform's bump: rejected, never copied from the stale entry.
    DWORD prior;
    CHECK(VirtualProtect(allocation, page_bytes, PAGE_NOACCESS, &prior));
    ++access_generation;
    CHECK(!probe_copy(address, out, sizeof out, false, &counts));
    CHECK(!probe_copy(address, input, sizeof input, true, &counts));
    CHECK(VirtualProtect(allocation, page_bytes, PAGE_READONLY, &prior));
    ++access_generation;
    CHECK(probe_copy(address, out, sizeof out, false, &counts) && out[0] == 0x6c);
    CHECK(!probe_copy(address, input, sizeof input, true, &counts)); // read-only entry never admits a write
    stable_generation = false;
    CHECK(VirtualFree(allocation, 0, MEM_RELEASE));
    printf("guest memory page cache: hits skip queries, writes and protection changes revalidate\n");
}

static uint64_t now_ns(void) {
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (uint64_t)((unsigned __int128)counter.QuadPart * UINT64_C(1000000000) /
                      (uint64_t)frequency.QuadPart);
}

static void benchmark(bool dense) {
    const size_t sizes[] = {0x10000, 0x1000000, 0x10000000, 0x40000000};
    const char *names[] = {"VirtualQuery", "ReadProcessMemory", "WriteProcessMemory",
                           "QueryWorkingSetEx", "baseline_read", "resident_read"};
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; ++s) {
        const uint64_t address = GUEST_LO + UINT64_C(0x40000000);
        void *allocation = VirtualAlloc((void *)(uintptr_t)address, sizes[s],
                                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!allocation) {
            printf("benchmark skipped bytes=%zu allocation_error=%lu\n", sizes[s], GetLastError());
            continue;
        }
        if (dense)
            for (size_t offset = 0; offset < sizes[s]; offset += page_bytes)
                *(volatile unsigned char *)((unsigned char *)allocation + offset) = 0;
        const size_t offsets[] = {0, sizes[s] / 2, sizes[s] - page_bytes};
        for (unsigned p = 0; p < 3; ++p) {
            const uint64_t at = address + offsets[p];
            *(volatile unsigned char *)(uintptr_t)at = 0;
            MEMORY_BASIC_INFORMATION mi;
            CHECK(VirtualQuery((void *)(uintptr_t)at, &mi, sizeof mi) == sizeof mi);
            printf("benchmark dense=%u bytes=%zu offset=%zu forward_region_bytes=%zu\n",
                   dense, sizes[s], offsets[p], (size_t)mi.RegionSize);
            for (unsigned kind = 0; kind < sizeof names / sizeof names[0]; ++kind) {
                unsigned char buffer[36] = {0};
                unsigned successful = 0;
                const unsigned iterations = 256;
                const uint64_t start = now_ns();
                for (unsigned i = 0; i < iterations; ++i) {
                    SIZE_T copied = 0;
                    PSAPI_WORKING_SET_EX_INFORMATION info = {.VirtualAddress = (void *)(uintptr_t)at};
                    ProbeCounts counts = {0};
                    switch (kind) {
                    case 0: successful += VirtualQuery((void *)(uintptr_t)at, &mi, sizeof mi) == sizeof mi; break;
                    case 1: successful += ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)at,
                                                            buffer, sizeof buffer, &copied) && copied == sizeof buffer; break;
                    case 2: successful += WriteProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)at,
                                                             buffer, sizeof buffer, &copied) && copied == sizeof buffer; break;
                    case 3: successful += QueryWorkingSetEx(GetCurrentProcess(), &info, sizeof info) &&
                                          info.VirtualAttributes.Valid; break;
                    case 4: successful += baseline_copy(at, buffer, sizeof buffer, false); break;
                    case 5: successful += probe_copy(at, buffer, sizeof buffer, false, &counts); break;
                    }
                }
                const uint64_t end = now_ns();
                CHECK(successful == iterations);
                printf("  %s calls=%u mean_ns=%.1f\n", names[kind], iterations,
                       (double)(end - start) / iterations);
            }
        }
        CHECK(VirtualFree(allocation, 0, MEM_RELEASE));
    }
}

int main(int argc, char **argv) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    page_bytes = info.dwPageSize;
    correctness();
    cache_hits();
    if (argc == 2 && !strcmp(argv[1], "--benchmark")) benchmark(false);
    else if (argc == 2 && !strcmp(argv[1], "--benchmark-dense")) benchmark(true);
    else if (argc != 1) return 2;
    return 0;
}
