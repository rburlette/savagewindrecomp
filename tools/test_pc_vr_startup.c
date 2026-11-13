// Focused contract check for the pinned original startup and mode-2 import seam.
// No game files, generated code or headset are loaded.
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_game_sites_182.h"
#include "../runtime/rt.h"
#include "../runtime/gamelib.h"
#include "../runtime/hle/labo_guest_memory.h"

static const uint64_t test_manager_slot = GUEST_LO + 0x3f00000;
static const uint64_t test_manager = GUEST_LO + 0x4000000;
static const uint64_t test_vr = GUEST_LO + 0x4100000;
static bool manager_ready;
static uint8_t vr_active, mode;
static uint8_t vr_request[2];
static bool sensor_active;
static uint64_t now_ns;
static Cpu test_cpu;
static uint64_t calls[16];
static uint64_t call_x0[16], call_x1[16];
static size_t call_count;
static unsigned timestamp_reset_count;
static jmp_buf fatal_jump;
static int fatal_count;

static bool test_guest_copy(uint64_t address, void *buffer, size_t bytes, bool write) {
    if (write || !manager_ready) return false;
    if (address == MAIN_BASE + 0x1c05cc8 && bytes == 8) memcpy(buffer, &test_manager_slot, 8);
    else if (address == test_manager_slot && bytes == 8) memcpy(buffer, &test_manager, 8);
    else if (address == test_manager + 0x380 && bytes == 8) memcpy(buffer, &test_vr, 8);
    else if (address == test_manager + 0x11be && bytes == 1) memcpy(buffer, &mode, 1);
    else if (address == test_vr + 0x10 && bytes == 1) memcpy(buffer, &vr_active, 1);
    else if (address == test_vr + 0x300 && bytes == 2) memcpy(buffer, vr_request, 2);
    else return false;
    return true;
}

// Replace only the guest-memory read in the included implementation. This
// exercises real startup decisions and original guest-call sequence with a
// fixed synthetic state; the production copy guard remains unchanged.
#define labo_guest_copy test_guest_copy
#include "../runtime/pc_vr.c"
#undef labo_guest_copy
#include "test_pc_vr_settings.h"

static uint32_t module_count = 1;
static GameLibModule game_module = {
    .name = "main", .base = MAIN_BASE, .size = TEST_MAIN_SIZE_182, .build_id =
        "de0b6ac4efc1dda7f80f6d7598ec7a7d00000000000000000000000000000000"
};
static GameLibTables game_tables = {
    .abi = GAMELIB_ABI, .modules = &game_module, .nmodules = &module_count
};
static Module loaded_main = {.name = "main", .base = MAIN_BASE, .size = 0x1d37000};

const GameLibTables *gamelib_tables(void) { return &game_tables; }
Module *loader_module(const char *name) { return !strcmp(name, "main") ? &loaded_main : NULL; }
uint64_t loader_find_symbol_in(const char *module, const char *name) {
    assert(!strcmp(module, "sdk"));
    if (!strcmp(name, "_ZN2nn2pl9EndVrModeEv")) return 0x1001;
    if (!strcmp(name, "_ZN2nn2pl11BeginVrModeEv")) return 0x1002;
    if (!strcmp(name, "_ZN2nn2oe15EnableRecordingEv")) return 0x1003;
    if (!strcmp(name, "_ZN2nn3hid32ResetSevenSixAxisSensorTimestampEv")) return 0x1004;
    return 0;
}
Cpu *kernel_current_cpu(void) { return &test_cpu; }
bool labo_motion_active(void) { return sensor_active; }
void pc_vr_motion_contract_timestamp_reset(void) { ++timestamp_reset_count; }
uint64_t plat_now_ns(void) { return now_ns; }
void guest_call(Cpu *c, uint64_t address) {
    assert(call_count < sizeof calls / sizeof calls[0]);
    calls[call_count] = address;
    call_x0[call_count] = c->x[0];
    call_x1[call_count++] = c->x[1];
    c->x[0] = UINT64_MAX; // startup must restore the intercepted guest CPU.
}
void rt_fatal_trace(void) {
    ++fatal_count;
    longjmp(fatal_jump, 1);
}

static void reset_case(void) {
    pc_vr_startup_reset();
    manager_ready = false;
    vr_active = 0;
    memset(vr_request, 0, sizeof vr_request);
    mode = 2;
    sensor_active = false;
    now_ns = 1000000000ull;
    call_count = 0;
    fatal_count = 0;
    timestamp_reset_count = 0;
    memset(&test_cpu, 0, sizeof test_cpu);
    test_cpu.x[0] = 0x5555;
}

int main(void) {
    (void)labo_guest_copy; // declared by the real guard header; mocked in this fixture
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL});
    assert(pc_vr_enabled());

    reset_case();
    pc_vr_startup_tick(); // title exists; platform manager does not yet
    assert(call_count == 0);
    manager_ready = true;
    pc_vr_startup_tick();
    assert(call_count == 3 && calls[0] == MAIN_BASE + 0x114d090 &&
           calls[1] == MAIN_BASE + 0x0d76ce0 && calls[2] == MAIN_BASE + 0x0d76d90);
    assert(call_x0[0] == test_vr && call_x1[0] == 1 &&
           call_x0[1] == 1 && call_x0[2] == 1);
    assert(test_cpu.x[0] == 0x5555);
    pc_vr_startup_tick();
    assert(call_count == 3); // one request, not once per title frame
    vr_active = 1;
    sensor_active = true;
    pc_vr_startup_poll(); // title hook may have ceased after Continue
    assert(call_count == 3 && startup_active_logged);
    vr_active = 0;
    sensor_active = false;
    now_ns += 21000000000ull;
    pc_vr_startup_poll(); // later user shutdown cannot trip the startup deadline

    reset_case();
    manager_ready = true;
    vr_active = 1; // inherited game state still needs sensor lifecycle refresh
    pc_vr_startup_tick();
    assert(call_count == 3);
    now_ns += 21000000000ull;
    if (!setjmp(fatal_jump)) { pc_vr_startup_poll(); assert(0); }
    assert(fatal_count == 1);

    reset_case();
    pc_vr_startup_tick();
    now_ns += 21000000000ull;
    if (!setjmp(fatal_jump)) { pc_vr_startup_poll(); assert(0); }
    assert(fatal_count == 1 && call_count == 0); // readiness timeout

    reset_case();
    manager_ready = true;
    loaded_main.size = 0x1000;
    if (!setjmp(fatal_jump)) { pc_vr_startup_tick(); assert(0); }
    assert(fatal_count == 1 && call_count == 0); // pinned ABI/layout guard
    loaded_main.size = 0x1d37000;

    reset_case();
    manager_ready = true;
    vr_request[1] = 2; // reject an object whose transition bytes are not booleans
    pc_vr_startup_tick();
    assert(call_count == 0);
    vr_request[1] = 0;
    pc_vr_startup_tick();
    assert(call_count == 3);

    reset_case();
    manager_ready = true;
    vr_active = 1;
    test_cpu.x[19] = test_manager;
    test_cpu.x[21] = test_manager + 0x11be;
    assert(pc_vr_labo_mode2_tracking(&test_cpu));
    mode = 3;
    assert(!pc_vr_labo_mode2_tracking(&test_cpu));
    mode = 2;
    test_cpu.x[21]++;
    assert(!pc_vr_labo_mode2_tracking(&test_cpu));
    test_cpu.x[21]--;

    PcVrHostFn end = pc_vr_lifecycle_lookup("_ZN2nn2pl9EndVrModeEv");
    PcVrHostFn recording = pc_vr_lifecycle_lookup("_ZN2nn2oe15EnableRecordingEv");
    assert(end && recording);
    test_cpu.x[30] = MAIN_BASE + 0x114d158;
    end(&test_cpu);
    assert(calls[0] == 0x1002 && test_cpu.x[30] == MAIN_BASE + 0x114d158);
    test_cpu.x[30] = MAIN_BASE + 0x114d15c;
    recording(&test_cpu);
    assert(calls[1] == 0x1004 && timestamp_reset_count == 1);
    vr_active = 0;
    test_cpu.x[30] = MAIN_BASE + 0x114d158;
    end(&test_cpu);
    assert(calls[2] == 0x1001); // original disable still ends VR
    vr_active = 1;
    test_cpu.x[30] = MAIN_BASE + 0x114d158 + 4;
    end(&test_cpu);
    assert(calls[3] == 0x1001); // only the pinned import return site redirects

    puts("PC VR startup and mode-2 lifecycle fixture passed");
    return 0;
}

// hle/labo_guest_memory.h: a new access generation per call keeps its page cache cold here.
uint64_t plat_guest_access_generation(void) { static uint64_t generation; return ++generation; }
