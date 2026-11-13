// Native Windows regression of the actual read-only collector and atomic publication.
// All guest addresses below are synthetic pages owned by this standalone process. No game files are opened.
#ifndef _WIN32
#error This fixture exercises the Windows committed-memory reader.
#endif
#include <windows.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BOOL fixture_read_process_memory(HANDLE process, LPCVOID address, LPVOID out, SIZE_T bytes, SIZE_T *copied);
#define ReadProcessMemory fixture_read_process_memory
#include "../runtime/hle/framepace.c"

void pc_vr_startup_poll(void) {} // Separately exercised by the PC VR startup fixture.
void labo_trace_vfr(void) {} // This collector fixture does not open the resource trace.
#undef ReadProcessMemory
// The collector reads its roots from the game sites (g_site): 1.8.2's, whose addresses the synthetic pages below use.
#include "test_game_sites_182.h"

volatile int g_movie_playing;
// Unrelated pacing/logging entry points are linked by this translation unit, but must never run in the fixture.
Settings g_settings;
uint64_t g_fs_vfr;
volatile int g_movie_fps;
bool nvn_timeline(void) { abort(); }
uint32_t kernel_current_thread_handle(void) { abort(); }
bool movie_paced(void) { abort(); }
uint64_t kernel_heap_size(void) { abort(); }
void kernel_dump_threads(void) { abort(); }
float winsize_refresh_rate(void) { abort(); }
void gamelib_sync(void) { abort(); }
uint64_t renderer_present_count(void) { abort(); }
int ui_layout_map_mode(uint64_t screen) { (void)screen; abort(); }
static Module fixture_main = {.name = "main", .base = MAIN_BASE, .size = 0x02000000};
static atomic_uint_fast64_t fixture_now = UINT64_C(1000000000);
static int fixture_reboot;
Module *loader_module(const char *name) { return !strcmp(name, "main") ? &fixture_main : NULL; }
uint64_t plat_now_ns(void) { return atomic_load(&fixture_now); }
int rt_reboot_requested(void) { return fixture_reboot; }

static unsigned checks;
#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } \
} while (0)

#define SCREEN_ROOT UINT64_C(0x7001c05d88)
#define WORLD_ROOT UINT64_C(0x7001cf5d00)
#define EVENT_ROOT UINT64_C(0x7001cd8db8)
#define SCREEN_SLOT (HEAP_BASE + 0x10000)
#define SCREEN_OBJECT (HEAP_BASE + 0x20000)
#define SCREEN_ARRAY (HEAP_BASE + 0x30000)
#define WORLD_OBJECT (HEAP_BASE + 0x40000)
#define EVENT_OBJECT (HEAP_BASE + 0x50000)
#define SCREEN_INSTANCES (HEAP_BASE + 0x80000)
#define EVENT_INSTANCE (HEAP_BASE + 0x90000)
#define RESERVED_OBJECT (HEAP_BASE + 0xa0000)

static int mutate_array_after_read;
static BOOL fixture_read_process_memory(HANDLE process, LPCVOID address, LPVOID out, SIZE_T bytes, SIZE_T *copied) {
    BOOL ok = ReadProcessMemory(process, address, out, bytes, copied);
    if (ok && mutate_array_after_read && (uint64_t)(uintptr_t)address == SCREEN_ARRAY) {
        mutate_array_after_read = 0;
        ((uint64_t *)(uintptr_t)SCREEN_ARRAY)[2] = 0;
    }
    return ok;
}

static void put64(uint64_t address, uint64_t value) { memcpy((void *)(uintptr_t)address, &value, sizeof value); }
static void put32(uint64_t address, uint32_t value) { memcpy((void *)(uintptr_t)address, &value, sizeof value); }
static void commit(uint64_t address, SIZE_T size) {
    uint64_t page = address & ~UINT64_C(0xfff);
    CHECK(VirtualAlloc((void *)(uintptr_t)page, (SIZE_T)(address - page) + size,
                       MEM_COMMIT, PAGE_READWRITE) == (void *)(uintptr_t)page);
}

static void fixture_good(void) {
    put64(SCREEN_ROOT, SCREEN_SLOT);
    put64(SCREEN_SLOT, SCREEN_OBJECT);
    put32(SCREEN_OBJECT + 0x28, 64);
    put64(SCREEN_OBJECT + 0x30, SCREEN_ARRAY);
    memset((void *)(uintptr_t)SCREEN_ARRAY, 0, 256 * sizeof(uint64_t));
    put64(SCREEN_ARRAY + 2 * 8, SCREEN_INSTANCES);
    put64(SCREEN_ARRAY + 3 * 8, SCREEN_INSTANCES + 16);
    put32(PAUSE_STATE, 0);
    put64(WORLD_ROOT, WORLD_OBJECT);
    put32(WORLD_OBJECT + 0x5c0, 1);
    put64(EVENT_ROOT, EVENT_OBJECT);
    put64(EVENT_OBJECT + 0x1d2b8, 0);
    g_movie_playing = 0;
    fixture_reboot = 0;
    fixture_main.size = 0x02000000;
    atomic_store(&fixture_now, UINT64_C(1000000000));
    mutate_array_after_read = 0;
    automation_state_reset();
}

static void unknown(void) {
    AutomationState out, zero;
    memset(&out, 0xa5, sizeof out);
    memset(&zero, 0, sizeof zero);
    CHECK(!automation_state_read(&out));
    CHECK(!memcmp(&out, &zero, sizeof out));
}

static AutomationState sample(void) {
    AutomationState out;
    automation_sample();
    CHECK(automation_state_read(&out));
    return out;
}

static void invalid_sample(void) { automation_sample(); unknown(); }

static void test_collector(void) {
    AutomationState s;
    fixture_good();
    unknown();
    s = sample();
    CHECK(s.sampled_ns == UINT64_C(1000000000));
    CHECK(s.screen_count == 64 && s.screen_mask == ((UINT64_C(1) << 2) | (UINT64_C(1) << 3)));
    CHECK(s.pause_state == 0 && s.flags == AUTOMATION_STATE_WORLD_LOADED);

    // At the title, screen 0x38 can be a Load list. Pause context must not be invented by the snapshot.
    fixture_good();
    memset((void *)(uintptr_t)SCREEN_ARRAY, 0, 256 * sizeof(uint64_t));
    put64(SCREEN_ARRAY + 8, SCREEN_INSTANCES);
    put64(SCREEN_ARRAY + 0x38 * 8, SCREEN_INSTANCES + 16);
    put32(WORLD_OBJECT + 0x5c0, 0);
    s = sample();
    CHECK(s.flags == AUTOMATION_STATE_TITLE && s.pause_state == 0);
    CHECK(s.screen_mask == ((UINT64_C(1) << 1) | (UINT64_C(1) << 0x38)));

    fixture_good();
    put32(PAUSE_STATE, 2);
    put64(SCREEN_ARRAY + 0x2f * 8, SCREEN_INSTANCES + 32);
    put64(SCREEN_ARRAY + 0x38 * 8, SCREEN_INSTANCES + 48);
    s = sample();
    CHECK(s.pause_state == 2 && s.flags == (AUTOMATION_STATE_PAUSED | AUTOMATION_STATE_WORLD_LOADED));
    CHECK((s.screen_mask & ((UINT64_C(1) << 0x2f) | (UINT64_C(1) << 0x38))) ==
          ((UINT64_C(1) << 0x2f) | (UINT64_C(1) << 0x38)));

    fixture_good();
    put64(EVENT_OBJECT + 0x1d2b8, EVENT_INSTANCE);
    g_movie_playing = 1;
    s = sample();
    CHECK(s.flags == (AUTOMATION_STATE_WORLD_LOADED | AUTOMATION_STATE_EVENT | AUTOMATION_STATE_MOVIE));
    g_movie_playing = -1;
    invalid_sample();
    fixture_good();
    put32(SCREEN_OBJECT + 0x28, 71);
    put64(SCREEN_ARRAY + 70 * 8, SCREEN_INSTANCES + 64);
    s = sample();
    CHECK(s.screen_count == 71 && (s.flags & AUTOMATION_STATE_HIGH_SCREENS));
    CHECK(s.screen_mask == ((UINT64_C(1) << 2) | (UINT64_C(1) << 3)));
    fixture_good();
    put64(WORLD_ROOT, 0);
    put64(EVENT_ROOT, 0);
    s = sample();
    CHECK(!s.flags);  // absent world/event managers are not dereferenced

    const uint64_t bad_slots[] = {0, 1, SCREEN_SLOT + 1, GUEST_LO - 8, GUEST_HI, UINT64_MAX};
    for (unsigned i = 0; i < sizeof bad_slots / sizeof *bad_slots; i++) {
        fixture_good(); put64(SCREEN_ROOT, bad_slots[i]); invalid_sample();
    }
    fixture_good(); put64(SCREEN_SLOT, SCREEN_OBJECT + 1); invalid_sample();
    fixture_good(); put64(SCREEN_SLOT, GUEST_HI - 8); invalid_sample();
    fixture_good(); put32(SCREEN_OBJECT + 0x28, 0); invalid_sample();
    fixture_good(); put32(SCREEN_OBJECT + 0x28, 257); invalid_sample();
    fixture_good(); put64(SCREEN_OBJECT + 0x30, 0); invalid_sample();
    fixture_good(); put64(SCREEN_OBJECT + 0x30, GUEST_HI - 8); invalid_sample();
    fixture_good(); put64(SCREEN_ARRAY + 2 * 8, UINT64_MAX); invalid_sample();
    fixture_good(); put64(WORLD_ROOT, WORLD_OBJECT + 1); invalid_sample();
    fixture_good(); put64(EVENT_ROOT, GUEST_HI - 8); invalid_sample();
    fixture_good(); put64(EVENT_OBJECT + 0x1d2b8, UINT64_MAX); invalid_sample();
    fixture_good(); fixture_main.size = 0x1000; invalid_sample();
    fixture_good(); fixture_main.size = UINT64_MAX; invalid_sample();
    fixture_good(); mutate_array_after_read = 1; invalid_sample();
    CHECK(!mutate_array_after_read);  // actual collector saw an entry change between its two reads
}

static void test_permissions(void) {
    MEMORY_BASIC_INFORMATION mi;
    DWORD old;
    fixture_good();
    put64(WORLD_ROOT, RESERVED_OBJECT);
    CHECK(VirtualQuery((void *)(uintptr_t)RESERVED_OBJECT, &mi, sizeof mi) == sizeof mi && mi.State == MEM_RESERVE);
    invalid_sample();
    CHECK(VirtualQuery((void *)(uintptr_t)RESERVED_OBJECT, &mi, sizeof mi) == sizeof mi && mi.State == MEM_RESERVE);

    const DWORD denied[] = {PAGE_NOACCESS, PAGE_READWRITE | PAGE_GUARD};
    for (unsigned i = 0; i < sizeof denied / sizeof *denied; i++) {
        fixture_good();
        CHECK(VirtualProtect((void *)(uintptr_t)SCREEN_ARRAY, 0x1000, denied[i], &old));
        invalid_sample();
        CHECK(VirtualQuery((void *)(uintptr_t)SCREEN_ARRAY, &mi, sizeof mi) == sizeof mi && mi.Protect == denied[i]);
        CHECK(VirtualProtect((void *)(uintptr_t)SCREEN_ARRAY, 0x1000, PAGE_READWRITE, &old));
    }
    fixture_good();
    CHECK(VirtualProtect((void *)(uintptr_t)SCREEN_ARRAY, 0x1000, PAGE_READONLY, &old));
    (void)sample();
    CHECK(VirtualProtect((void *)(uintptr_t)SCREEN_ARRAY, 0x1000, PAGE_READWRITE, &old));

    // A range crossing from committed to reserved memory must fail before copying any guest bytes.
    fixture_good();
    put32(SCREEN_OBJECT + 0x28, 2);
    put64(SCREEN_OBJECT + 0x30, SCREEN_ARRAY + 0xff8);
    invalid_sample();
    CHECK(VirtualQuery((void *)(uintptr_t)(SCREEN_ARRAY + 0x1000), &mi, sizeof mi) == sizeof mi && mi.State == MEM_RESERVE);
}

static void test_freshness_and_reset(void) {
    fixture_good(); (void)sample();
    atomic_store(&fixture_now, UINT64_C(1499999999));
    AutomationState out;
    CHECK(automation_state_read(&out));
    atomic_store(&fixture_now, UINT64_C(1500000000)); unknown();
    atomic_store(&fixture_now, UINT64_C(999999999)); unknown();
    fixture_good(); (void)sample(); automation_state_reset(); unknown();
    fixture_good(); (void)sample(); fixture_reboot = 1; unknown();
    automation_sample(); fixture_reboot = 0; unknown();
    fixture_good(); (void)sample();
    atomic_fetch_add(&automation_pub.sequence, 1); unknown();  // bounded reader fails closed during publication
    atomic_fetch_add(&automation_pub.sequence, 1);
    CHECK(!automation_state_read(NULL));
    // Process-latched opt-in is not changed underneath a running macro by an environment edit.
    CHECK(_putenv_s("SAVAGEWIND_QUICKSAVE", "0") == 0);
    CHECK(automation_enabled());
    CHECK(_putenv_s("SAVAGEWIND_QUICKSAVE", "1") == 0);
}

static atomic_bool writer_done;
static const AutomationState pair[2] = {
    {.sampled_ns = UINT64_C(1000000000), .screen_mask = UINT64_C(1) << 2, .screen_count = 64,
     .pause_state = 0, .flags = AUTOMATION_STATE_WORLD_LOADED},
    {.sampled_ns = UINT64_C(1000000001), .screen_mask = UINT64_C(1) << 56, .screen_count = 128,
     .pause_state = 2, .flags = AUTOMATION_STATE_PAUSED | AUTOMATION_STATE_MOVIE}
};
static DWORD WINAPI publish_thread(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 50000; i++) {
        while (atomic_flag_test_and_set(&automation_pub.writer)) SwitchToThread();
        automation_publish(&pair[i & 1], true);
        atomic_flag_clear(&automation_pub.writer);
        if (!(i & 255)) SwitchToThread();
    }
    atomic_store(&writer_done, true);
    return 0;
}
static bool matches(const AutomationState *a, const AutomationState *b) {
    return a->sampled_ns == b->sampled_ns && a->screen_mask == b->screen_mask && a->screen_count == b->screen_count &&
           a->pause_state == b->pause_state && a->flags == b->flags;
}
static void test_concurrency(void) {
    fixture_good();
    atomic_store(&fixture_now, UINT64_C(1000000002));
    atomic_store(&writer_done, false);
    HANDLE thread = CreateThread(NULL, 0, publish_thread, NULL, 0, NULL);
    CHECK(thread != NULL);
    unsigned accepted = 0, mixed = 0, attempts = 0;
    do {
        AutomationState s;
        attempts++;
        if (automation_state_read(&s)) {
            accepted++;
            if (!matches(&s, &pair[0]) && !matches(&s, &pair[1])) mixed++;
        }
    } while (!atomic_load(&writer_done));
    CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CHECK(CloseHandle(thread));
    CHECK(attempts > 0 && accepted > 0 && mixed == 0);
    printf("atomic publication: 50000 writes, %u reads, %u coherent accepted, %u mixed\n", attempts, accepted, mixed);
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "enabled";
    const char *value = !strcmp(mode, "enabled") ? "1" : !strcmp(mode, "off") ? "0" :
                        !strcmp(mode, "two") ? "2" : !strcmp(mode, "leading-zero") ? "01" :
                        !strcmp(mode, "space") ? "1 " : "";
    CHECK(_putenv_s("SAVAGEWIND_QUICKSAVE", value) == 0);
    bool enabled = !strcmp(mode, "enabled");
    CHECK(automation_enabled() == enabled);
    if (!enabled) {
        // No guest reservation at all: sampling a disabled feature must not touch any guest address.
        automation_sample(); unknown();
        CHECK(_putenv_s("SAVAGEWIND_QUICKSAVE", "1") == 0);
        CHECK(!automation_enabled());
        automation_sample(); unknown();
    } else {
        CHECK(VirtualAlloc((void *)(uintptr_t)GUEST_LO, (SIZE_T)(GUEST_HI - GUEST_LO),
                           MEM_RESERVE, PAGE_READWRITE) == (void *)(uintptr_t)GUEST_LO);
        commit(SCREEN_ROOT, 8); commit(WORLD_ROOT, 8); commit(EVENT_ROOT, 8); commit(PAUSE_STATE, 4);
        commit(SCREEN_SLOT, 8); commit(SCREEN_OBJECT, 0x38); commit(SCREEN_ARRAY, 0x1000);
        commit(WORLD_OBJECT, 0x5c4); commit(EVENT_OBJECT, 0x1d2c0);
        commit(SCREEN_INSTANCES, 0x1000); commit(EVENT_INSTANCE, 8);
        test_collector(); test_permissions(); test_freshness_and_reset(); test_concurrency();
        CHECK(VirtualFree((void *)(uintptr_t)GUEST_LO, 0, MEM_RELEASE));
    }
    printf("automation state %s: %u checks passed\n", mode, checks);
    return 0;
}
