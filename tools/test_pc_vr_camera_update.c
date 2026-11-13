// Asset-free PC VR camera-update fixture (runtime/hle/pc_vr_camera_update.c): the eight pinned camera-update entries
// are overridden only with PC VR, head look and the game version's sites; each override hands x0 to head look and runs
// the original update with its own Cpu result.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_game_sites_182.h"
#include "../runtime/hle/pc_vr_camera_update.c"
#include "../runtime/pc_vr_main.h"

static unsigned checks, override_calls, original_calls, lookup_calls;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "camera update check failed line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

static bool mock_pc_vr;
static bool mock_head_look;
static unsigned head_look_observations;
static uint64_t observed_controller;
static unsigned original_calls_at_observation;
static int missing_type = -1;
static uint32_t module_count = 1;
static GameLibModule game_module = {.name = "main", .base = MAIN_BASE, .size = 0x1d37000,
    .build_id = "de0b6ac4efc1dda7f80f6d7598ec7a7d00000000000000000000000000000000"};
static GameLibTables game_tables = {.modules = &game_module, .nmodules = &module_count};
static Module loaded_main = {.name = "main", .base = MAIN_BASE, .size = 0x1d37000};
static uint64_t override_addresses[CAMERA_TYPES];
static HostFn override_functions[CAMERA_TYPES];

bool pc_vr_enabled(void) { return mock_pc_vr; }
bool pc_vr_main_compatible(void) { return pc_vr_main_layout(gamelib_tables(), loader_module("main")); }
bool pc_vr_head_look_enabled(void) { return mock_head_look; }
void pc_vr_head_look_observe_controller(uint64_t controller) {
    observed_controller = controller;
    original_calls_at_observation = original_calls;
    ++head_look_observations;
}
const GameLibTables *gamelib_tables(void) { return &game_tables; }
Module *loader_module(const char *name) { return !strcmp(name, "main") ? &loaded_main : NULL; }
static void original_update(Cpu *cpu) {
    ++original_calls;
    cpu->x[0] += 17;
    cpu->x[5] ^= UINT64_C(0x87654321);
    cpu->sp -= 32;
}
GuestFn dispatch_lookup(uint64_t address) {
    ++lookup_calls;
    for (unsigned camera_type = 0; camera_type < CAMERA_TYPES; ++camera_type)
        if (address == camera_update_entry(camera_type))
            return (int)camera_type == missing_type ? NULL : original_update;
    return NULL;
}
void hle_override(uint64_t address, HostFn function, const char *name) {
    CHECK(override_calls < CAMERA_TYPES);
    CHECK(!strcmp(name, "PcVrCameraUpdate"));
    override_addresses[override_calls] = address;
    override_functions[override_calls++] = function;
}

// The override observes the controller before the original update and leaves the original's Cpu result untouched.
static void call_and_check(unsigned camera_type, uint64_t controller, uint64_t return_address) {
    Cpu actual = {0}, expected = {0};
    actual.x[0] = expected.x[0] = controller;
    actual.x[5] = expected.x[5] = 9;
    actual.x[30] = expected.x[30] = return_address;
    actual.sp = expected.sp = 0x1000;
    original_update(&expected);
    const unsigned before_original = original_calls, before_observations = head_look_observations;
    override_functions[camera_type](&actual);
    CHECK(original_calls == before_original + 1);
    CHECK(head_look_observations == before_observations + 1);
    CHECK(observed_controller == controller && original_calls_at_observation == before_original);
    CHECK(!memcmp(&actual, &expected, sizeof actual));
}

int main(void) {
    // Head look off, PC VR off, no PC VR sites or main laid out otherwise: no override.
    mock_pc_vr = true;
    mock_head_look = false;
    pc_vr_camera_update_init();
    CHECK(!camera_updates_registered && override_calls == 0 && lookup_calls == 0);
    mock_pc_vr = false;
    mock_head_look = true;
    pc_vr_camera_update_init();
    CHECK(!camera_updates_registered && override_calls == 0 && lookup_calls == 0);
    mock_pc_vr = true;
    const uint64_t builder = g_site.pcvr_matrix_builder;
    g_site.pcvr_matrix_builder = 0;  // (a game version without PC VR's sites)
    pc_vr_camera_update_init();
    CHECK(!camera_updates_registered && override_calls == 0);
    g_site.pcvr_matrix_builder = builder;
    loaded_main.size--;
    pc_vr_camera_update_init();
    CHECK(!camera_updates_registered && override_calls == 0);
    loaded_main.size++;
    missing_type = 4;
    pc_vr_camera_update_init();
    CHECK(!camera_updates_registered && override_calls == 0); // No partial override on lookup failure.
    missing_type = -1;

    pc_vr_camera_update_init();
    CHECK(camera_updates_registered && override_calls == CAMERA_TYPES);
    CHECK(lookup_calls >= CAMERA_TYPES);
    for (unsigned camera_type = 0; camera_type < CAMERA_TYPES; ++camera_type) {
        CHECK(override_addresses[camera_type] == camera_update_entry(camera_type));
        CHECK(override_functions[camera_type] == camera_update_wrappers[camera_type]);
        CHECK(original_updates[camera_type] == original_update);
    }
    // Every type observes, including an unmapped controller pointer, which head look checks itself.
    for (unsigned camera_type = 0; camera_type < CAMERA_TYPES; ++camera_type) {
        call_and_check(camera_type, 1, MAIN_BASE + 0x1234);
        call_and_check(camera_type, GUEST_LO + 0x100 + 8 * camera_type, MAIN_BASE + 0xadbe4c);
    }

    // A reboot runs init again; the process keeps its overrides and registers them once.
    const unsigned lookups_before_reboot = lookup_calls;
    pc_vr_camera_update_init();
    CHECK(camera_updates_registered && override_calls == CAMERA_TYPES && lookup_calls == lookups_before_reboot);
    call_and_check(3, MAIN_BASE + 0x5678, MAIN_BASE + 0x5678);
    printf("pc vr camera update: %u checks passed\n", checks);
    return 0;
}
