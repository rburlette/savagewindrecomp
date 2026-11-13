// Asset-free projection/native-view observer and guarded experiment checks. Synthetic guest objects
// only; the real game library, runtime and headset are never opened.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sched.h>
#endif
#ifdef _WIN32
#include <windows.h>
static bool fail_next_parameter_write;
static unsigned parameter_write_calls;
static SIZE_T last_parameter_write_size;
static uintptr_t last_parameter_write_address;
static BOOL WINAPI fixture_write_process_memory(HANDLE process, LPVOID address,
                                                LPCVOID in, SIZE_T bytes, SIZE_T *copied) {
    ++parameter_write_calls;
    last_parameter_write_size = bytes;
    last_parameter_write_address = (uintptr_t)address;
    if (fail_next_parameter_write) {
        fail_next_parameter_write = false;
        // Exercise restoration even when an unsuccessful OS write touched a
        // prefix. This is synthetic guest memory owned by this fixture.
        if (bytes >= sizeof(float)) memcpy(address, in, sizeof(float));
        if (copied) *copied = sizeof(float);
        return FALSE;
    }
    return WriteProcessMemory(process, address, in, bytes, copied);
}
#define WriteProcessMemory fixture_write_process_memory
#endif
#include "test_game_sites_182.h"
#include "../runtime/pc_vr_main.h"
#include "../runtime/hle/pc_vr_projection_trace.c"
#ifdef _WIN32
#undef WriteProcessMemory
#endif
#include "test_pc_vr_settings.h"

static unsigned checks, originals, overrides, lookups, pose_reads;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "projection check failed line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static bool mock_pc_vr = true, missing_device, missing_render;
static uint64_t mock_now = UINT64_C(10000000000);
static XrPoseSnapshot mock_pose;
static XrPoseSnapshot mock_head_pose;
static XrPoseReadResult mock_pose_result = XR_POSE_UNAVAILABLE;
static uint64_t mock_publication_step;
static bool mock_generate_projection, mock_bad_logical, mock_bad_device;
static atomic_bool hold_original, original_entered, release_original;
static void fixture_yield(void) {
#ifdef _WIN32
    Sleep(0);
#else
    sched_yield();
#endif
}
static void emulate_projection(Cpu *c);
static uint32_t module_count = 1;
static GameLibModule game_module = {.name = "main", .base = MAIN_BASE, .size = 0x1d37000,
    .build_id = "de0b6ac4efc1dda7f80f6d7598ec7a7d00000000000000000000000000000000"};
static GameLibTables game_tables = {.modules = &game_module, .nmodules = &module_count};
static Module loaded_main = {.name = "main", .base = MAIN_BASE, .size = 0x1d37000};
bool pc_vr_enabled(void) { return mock_pc_vr; }
const GameLibTables *gamelib_tables(void) { return &game_tables; }
Module *loader_module(const char *name) { return !strcmp(name, "main") ? &loaded_main : NULL; }
bool pc_vr_main_compatible(void) { return pc_vr_main_layout(gamelib_tables(), loader_module("main")); }
uint64_t xr_runtime_pose_clock_ns(void) { return mock_now; }
XrPoseReadResult xr_runtime_get_pose(XrPoseSnapshot *out) {
    ++pose_reads;
    if (mock_publication_step && mock_pose_result == XR_POSE_VALID) {
        mock_now += mock_publication_step;
        mock_pose.published_monotonic_ns = mock_now;
        ++mock_pose.frame_id;
    }
    if (mock_pose_result == XR_POSE_VALID) *out = mock_pose;
    return mock_pose_result;
}
void rt_fatal_trace(void) { fprintf(stderr, "unexpected fatal restoration failure\n"); exit(99); }
static void fake_original(Cpu *c) {
    ++originals;
    if (atomic_load(&hold_original)) {
        atomic_store(&original_entered, true);
        while (!atomic_load(&release_original)) fixture_yield();
    }
    if (mock_generate_projection) emulate_projection(c);
    c->x[5] ^= UINT64_C(0xabcdef); c->sp -= 16;
}
GuestFn dispatch_lookup(uint64_t address) {
    ++lookups;
    return address == PROJ_BUILDER || (address == PROJ_DEVICE && !missing_device) ||
        ((address == RENDER_BEGIN || address == RENDER_END) && !missing_render) ? fake_original : NULL;
}
void hle_override(uint64_t address, HostFn fn, const char *name) {
    CHECK(address == PROJ_BUILDER || address == PROJ_DEVICE || address == RENDER_BEGIN || address == RENDER_END);
    CHECK(fn == projection_builder_wrapper || fn == projection_device_wrapper || fn == render_begin_wrapper || fn == render_end_wrapper);
    CHECK(!strcmp(name, "PcVrProjectionObserver") || !strcmp(name, "PcVrProjectionDeviceObserver") ||
        !strcmp(name, "PcVrRenderScopeBegin") || !strcmp(name, "PcVrRenderScopeEnd"));
    ++overrides;
}
// The projection trace armed as the play configuration arms it (pc_vr_projection_trace_init), at this world scale. The
// configuration's parser keeps its own value finite and above 0 (test_play_configuration); the fixture sets any value,
// to see the module refuse the others itself.
static void fixture_init_at(double units_per_meter) {
    process_configuration.play.native_view_units_per_meter = units_per_meter;
    pc_vr_projection_trace_init();
}
static void fixture_init(void) { fixture_init_at(1.0); }

static void frustum(float params[9], float matrix[16], float left, float right, float up, float down) {
    const double n = 0.125, f = 8000, width = right - left, height = up - down;
    const double half_fovy = atan(height * 0.5);
    const float values[9] = {(float)n, (float)f, (float)(2 * half_fovy), (float)sin(half_fovy),
        (float)cos(half_fovy), (float)(height * 0.5), (float)(width / height),
        (float)((right + left) / (2 * width)), (float)((up + down) / (2 * height))};
    memcpy(params, values, sizeof values);
    memset(matrix, 0, 16 * sizeof *matrix);
    matrix[0] = (float)(2 / width); matrix[2] = (float)((right + left) / width);
    matrix[5] = (float)(2 / height); matrix[6] = (float)((up + down) / height);
    matrix[10] = (float)(-(f + n) / (f - n)); matrix[11] = (float)(-2 * f * n / (f - n)); matrix[14] = -1;
}

static void emulate_projection(Cpu *c) {
    const uint64_t object = c->x[0];
    float matrix[16];
    if (c->x[30] == PROJ_BUILD_CALLER && c->x[1] == object + 0xc) {
        const float *p = (const float *)(uintptr_t)(object + 0x98);
        const double n = p[0], f = p[1];
        const float values[16] = {(float)(1 / ((double)p[5] * p[6])), 0, 2 * p[7], 0,
            0, 1 / p[5], 2 * p[8], 0, 0, 0, (float)(-(f + n) / (f - n)),
            (float)(-2 * f * n / (f - n)), 0, 0, -1, 0};
        memcpy(matrix, values, sizeof matrix);
        if (mock_bad_logical) matrix[2] += 0.1f;
        memcpy((void *)(uintptr_t)c->x[1], matrix, sizeof matrix);
    } else if (c->x[30] == PROJ_DEVICE_CALLER && c->x[1] == object + 0x4c) {
        memcpy(matrix, (void *)(uintptr_t)(object + 0xc), sizeof matrix);
        const float *depth = (const float *)(uintptr_t)(object + 0x90);
        matrix[8] *= depth[0]; matrix[9] *= depth[0];
        matrix[10] = depth[0] * (matrix[10] + depth[1] * matrix[14]);
        matrix[11] = matrix[11] * depth[0] + depth[1] * matrix[15];
        if (mock_bad_device) matrix[0] += 0.1f;
        memcpy((void *)(uintptr_t)c->x[1], matrix, sizeof matrix);
    }
}

static void test_math(void) {
    float params[9], matrix[16], bad[16];
    frustum(params, matrix, -0.6f, 0.9f, 0.8f, -0.7f);
    CHECK(projection_matrix_contract(params, matrix));
    frustum(params, matrix, -1.2f, 0.7f, 1.1f, -0.4f);
    CHECK(projection_matrix_contract(params, matrix));
    for (unsigned i = 0; i < 16; ++i) {
        memcpy(bad, matrix, sizeof bad); bad[i] += 0.1f;
        CHECK(!projection_matrix_contract(params, bad));
        bad[i] = NAN; CHECK(!projection_matrix_contract(params, bad));
    }
    for (unsigned i = 0; i < 9; ++i) {
        const float saved = params[i]; params[i] = INFINITY;
        CHECK(!projection_matrix_contract(params, matrix)); params[i] = saved;
    }
    params[0] = -1; CHECK(!projection_matrix_contract(params, matrix));
    frustum(params, matrix, -0.6f, 0.9f, 0.8f, -0.7f);
    params[3] += 0.1f; CHECK(!projection_matrix_contract(params, matrix));
}

static void test_init(void) {
    mock_pc_vr = false; pc_vr_projection_trace_init(); CHECK(!overrides);  // (PC VR off: nothing)
    mock_pc_vr = true;
    // (a game version without PC VR's sites, or a main laid out otherwise: nothing registered)
    const uint64_t builder = g_site.pcvr_matrix_builder;
    g_site.pcvr_matrix_builder = 0; fixture_init(); CHECK(!overrides);
    g_site.pcvr_matrix_builder = builder;
    loaded_main.size = 0x1e98000; fixture_init(); CHECK(!overrides);
    loaded_main.size = TEST_MAIN_SIZE_182;
    // (a world scale the module refuses: nothing looked up, registered or armed)
    fixture_init_at(NAN); CHECK(!overrides && !lookups && !atomic_load(&projection_enabled));
    // (an original projection writer or render scope callback missing: all four looked up, none registered, nothing
    // armed; there is no part of the chain without the rest)
    missing_device = true; fixture_init();
    CHECK(!overrides && lookups == 4 && !atomic_load(&projection_enabled));
    missing_device = false; missing_render = true; fixture_init();
    CHECK(!overrides && lookups == 8 && !atomic_load(&projection_enabled) && !pc_vr_render_trace_enabled());
    CHECK(!atomic_load(&projection_view_experiment) && !atomic_load(&projection_fov_experiment));
    missing_render = false; fixture_init();
    CHECK(overrides == 4 && projection_registered && atomic_load(&projection_enabled) && pc_vr_render_trace_enabled());
    CHECK(atomic_load(&render_collecting) && atomic_load(&projection_view_experiment) &&
          atomic_load(&projection_fov_experiment) && projection_view_scale == 1.0);
    fixture_init(); CHECK(overrides == 4 && lookups == 12); // no recursive re-wrapping
}

#ifdef _WIN32
static uint64_t region, base, layers[2], frames[2];
static void u64(uint64_t at, uint64_t value) { memcpy((void *)(uintptr_t)at, &value, sizeof value); }
static void fixture_objects(void) {
    region = MAIN_BASE + UINT64_C(0x26000000);
    CHECK(VirtualAlloc((void *)(uintptr_t)region, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) == (void *)(uintptr_t)region);
    base = region + 0x100;
    layers[0] = region + 0x1000; layers[1] = region + 0x2000;
    frames[0] = region + 0x3800; frames[1] = region + 0x3700;
    u64(frames[0], region + 0x3900); u64(frames[0] + 8, PROJ_ROOT_RETURN);
    u64(frames[1], frames[0]); u64(frames[1] + 8, PROJ_SIBLING_RETURN);
    u64(base, PROJ_BASE_VTABLE);
    const float pose[9] = {10, 2, 4, 10, 2, 0, 0, 1, 0}, right[3] = {1, 0, 0};
    memcpy((void *)(uintptr_t)(base + 0x38), pose, sizeof pose);
    memcpy((void *)(uintptr_t)(base + 8), right, sizeof right);
    for (unsigned i = 0; i < 2; ++i) {
        const uint64_t layer = layers[i], object = layer + 0x288;
        u64(layer + 0x80, base); u64(layer + 0x220, object); u64(object, PROJ_VTABLE);
        u64(layer + 0x228, PROJ_EYE_VTABLE); u64(layer + 0x210, region + 0x3000);
        u64(layer + 0x208, i ? 0 : layers[1]);
        *(uint16_t *)(uintptr_t)(layer + 0x90) = i ? 0x1000 : 0;
        const float eye[9] = {10 + (i ? 0.04f : -0.04f), 2, 4,
            10 + (i ? 0.04f : -0.04f), 2, 0, 0, 1, 0};
        memcpy((void *)(uintptr_t)(layer + 0x260), eye, sizeof eye);
        float params[9], matrix[16], device[16];
        frustum(params, matrix, -0.6f, 0.9f, 0.8f, -0.7f);
        memcpy(device, matrix, sizeof device);
        device[10] = (matrix[10] + matrix[14]) * 0.5f;
        device[11] = matrix[11] * 0.5f;
        const float depth[2] = {0.5f, 1};
        memcpy((void *)(uintptr_t)(object + 0x98), params, sizeof params);
        memcpy((void *)(uintptr_t)(object + 0xc), matrix, sizeof matrix);
        memcpy((void *)(uintptr_t)(object + 0x4c), device, sizeof device);
        memcpy((void *)(uintptr_t)(object + 0x90), depth, sizeof depth);
    }
}
static void call_writer(uint64_t object, bool device, uint64_t caller, uint64_t output) {
    Cpu actual = {0}, expected;
    actual.x[0] = object; actual.x[1] = output; actual.x[2] = object + 0xc;
    actual.x[5] = 3; actual.x[30] = caller; actual.sp = 0x1000; expected = actual;
    const bool generate = mock_generate_projection;
    mock_generate_projection = false;
    fake_original(&expected);
    mock_generate_projection = generate;
    const unsigned before = originals;
    if (device) projection_device_wrapper(&actual); else projection_builder_wrapper(&actual);
    CHECK(originals == before + 1 && !memcmp(&actual, &expected, sizeof actual));
}
static void test_observation(void) {
    fixture_objects();
    unsigned char before[0x4000]; memcpy(before, (void *)(uintptr_t)region, sizeof before);
    const uint64_t object = layers[0] + 0x288;
    call_writer(object, false, PROJ_BUILD_CALLER, object + 0xc);
    CHECK(projection_writes == 0); // no verified layer note
    for (unsigned i = 0; i < 2; ++i)
        pc_vr_projection_trace_eye(layers[i], base, 5, 77, 3, true, frames[i], &mock_pose);
    CHECK(projection_note_count == 2 && projection_pairs == 1);
    CHECK(projection_notes[0].offset_sign == -1 && projection_notes[1].offset_sign == 1);
    CHECK(projection_notes[0].base_hash == projection_notes[1].base_hash);
    call_writer(object, false, PROJ_BUILD_CALLER, object + 0xc);
    call_writer(object, true, PROJ_DEVICE_CALLER, object + 0x4c);
    CHECK(projection_writes == 1 && projection_device_writes == 1 && !projection_bad_matrix);
    CHECK(!memcmp(before, (void *)(uintptr_t)region, sizeof before)); // absolutely no guest writes
    call_writer(object, false, PROJ_BUILD_CALLER + 4, object + 0xc);
    call_writer(object, false, PROJ_BUILD_CALLER, object + 0x10);
    call_writer(object, true, PROJ_DEVICE_CALLER + 4, object + 0x4c);
    call_writer(object, true, PROJ_DEVICE_CALLER, object + 0x50);
    CHECK(projection_writes == 1 && projection_device_writes == 1);
    u64(layers[0] + 0x220, object + 8);
    call_writer(object, false, PROJ_BUILD_CALLER, object + 0xc);
    CHECK(projection_writes == 1);
    u64(layers[0] + 0x220, object);
    u64(object, PROJ_VTABLE + 8);
    call_writer(object, false, PROJ_BUILD_CALLER, object + 0xc);
    CHECK(projection_writes == 1); u64(object, PROJ_VTABLE);
    mock_now += UINT64_C(250000001);
    call_writer(object, false, PROJ_BUILD_CALLER, object + 0xc);
    CHECK(projection_writes == 1);
    pc_vr_projection_trace_eye(layers[0], base, 6, 78, 3, true, frames[0], &mock_pose);
    *(float *)(uintptr_t)(object + 0xc) += 0.1f;
    call_writer(object, false, PROJ_BUILD_CALLER, object + 0xc);
    CHECK(projection_writes == 2 && projection_bad_matrix == 1);
    *(float *)(uintptr_t)(object + 0x4c + 0x28) += 0.1f;
    call_writer(object, true, PROJ_DEVICE_CALLER, object + 0x4c);
    CHECK(projection_device_writes == 2);
    // No budget: eyes and writers go on being observed.
    const uint64_t notes_before = projection_note_count, writes_before = projection_writes;
    for (unsigned i = 0; i < 250; ++i) {
        pc_vr_projection_trace_eye(layers[i % 2], base, 9, 80, 4, false, frames[i % 2], &mock_pose);
        call_writer(object, false, PROJ_BUILD_CALLER, object + 0xc);
    }
    CHECK(projection_note_count == notes_before + 250 && projection_writes == writes_before + 250);
    pc_vr_projection_trace_reset();
    call_writer(object, false, PROJ_BUILD_CALLER, object + 0xc);
    CHECK(projection_writes == writes_before + 250 && !projection_notes[0].note && !atomic_load(&projection_enabled));
    fixture_init();
    for (unsigned i = 0; i < 250; ++i)
        pc_vr_projection_trace_eye(0, base, 1, 1, 1, true, frames[0], &mock_pose);
    CHECK(projection_rejected == 250 && !projection_note_count && atomic_load(&projection_enabled));
    // No deadline: an eye older than the pose age is not matched to a writer, and the trace goes on.
    fixture_init();
    pc_vr_projection_trace_eye(layers[0], base, 1, 1, 1, true, frames[0], &mock_pose);
    mock_now += UINT64_C(5000000000);
    call_writer(object, false, PROJ_BUILD_CALLER, object + 0xc);
    CHECK(!projection_writes && atomic_load(&projection_enabled));
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

static void native_pose(void) {
    mock_pose = (XrPoseSnapshot){.frame_id = 77, .game_present_id = 35, .generation = 3,
        .published_monotonic_ns = mock_now, .predicted_display_time_ns = INT64_C(9000000000),
        .orientation_xyzw = {0, 0, 0, 1}, .angular_velocity_xyz = {0.1f, 0.2f, 0.3f},
        .angular_velocity_valid = true, .active = true, .focused = true, .orientation_valid = true,
        .orientation_tracked = true, .views_valid = true, .native_views_valid = true,
        .eyes = {{{0, 0, 0, 1}, {-0.032f, 1.5f, 0.3f}, {-0.8f, 0.9f, 0.85f, -0.75f}},
                 {{0, 0, 0, 1}, {0.032f, 1.5f, 0.3f}, {-0.9f, 0.8f, 0.85f, -0.75f}}},
        .calibration = {.epoch = 3, .heading_xyzw = {0, 0, 0, 1}, .origin_xyz = {0, 1.5f, 0.3f}}};
    mock_pose_result = XR_POSE_VALID;
    mock_head_pose = mock_pose;
}

static void observe(unsigned side) {
    pc_vr_projection_trace_eye(layers[side], base, 5, mock_head_pose.frame_id, mock_head_pose.generation,
        true, frames[side], &mock_head_pose);
}

static void restart_traversal(void) {
    fixture_init();
    native_pose();
}

static void *other_thread_sibling(void *unused) {
    (void)unused;
    observe(1);
    return NULL;
}

static void test_traversal(void) {
    fixture_objects();
    unsigned char before[0x4000]; memcpy(before, (void *)(uintptr_t)region, sizeof before);
    restart_traversal();
    const unsigned reads_before = pose_reads;
    CHECK(pc_vr_projection_trace_root(frames[0]));
    CHECK(!pc_vr_projection_trace_root(frames[1]));
    // New publication before the root must not replace the complete sample
    // handed in by head look, including either eye's pose/FOV.
    const XrPoseSnapshot head_sample = mock_head_pose;
    ++mock_pose.frame_id;
    mock_pose.eyes[0].fov_left_right_up_down[0] -= 0.02f;
    observe(0);
    const uint64_t first_serial = projection_traversal.serial;
    const XrPoseSnapshot captured = projection_traversal.native_snapshot;
    CHECK(first_serial && projection_roots == 1);
    CHECK(projection_traversal.open && projection_traversal.native_latched && !projection_traversal.native_pair);
    CHECK(pose_reads == reads_before + 1 && !memcmp(&captured, &head_sample, sizeof captured));
    // A newer publication changes every time/eye field: the sibling must keep
    // the full earlier snapshot, not synthesize a mixture or relatch per eye.
    ++mock_pose.frame_id; ++mock_pose.game_present_id; ++mock_pose.predicted_display_time_ns;
    mock_pose.eyes[0].position_xyz[0] -= 0.01f; mock_pose.eyes[1].position_xyz[0] += 0.01f;
    mock_pose.eyes[0].fov_left_right_up_down[0] -= 0.01f;
    mock_pose.angular_velocity_xyz[0] = 0.7f;
    observe(1);
    CHECK(projection_traversal_pairs == 1);
    CHECK(!projection_traversal.open && projection_traversal.native_pair);
    CHECK(projection_notes[0].traversal_serial == first_serial && projection_notes[1].traversal_serial == first_serial);
    CHECK(pose_reads == reads_before + 2 && !memcmp(&captured, &projection_traversal.native_snapshot, sizeof captured));
    CHECK(!memcmp(before, (void *)(uintptr_t)region, sizeof before));
    observe(1); // One-use consumption, even with the same saved stack chain.
    CHECK(projection_stack_rejected == 1 && !projection_traversal.native_latched && !projection_traversal.native_pair);

    restart_traversal(); observe(0);
    const uint64_t abandoned_serial = projection_traversal.serial;
    ++mock_pose.frame_id; mock_head_pose = mock_pose; observe(0); // Same stack/object addresses, new root.
    CHECK(projection_traversal.serial > abandoned_serial && projection_roots == 2);
    const uint64_t replacement_serial = projection_traversal.serial;
    observe(1);
    CHECK(projection_traversal_pairs == 1 && projection_traversal.native_pair);
    CHECK(projection_notes[1].traversal_serial == replacement_serial);
    CHECK(projection_traversal.native_snapshot.frame_id == mock_pose.frame_id);

    restart_traversal(); observe(1);
    CHECK(projection_stack_rejected == 1 && !projection_roots && !projection_traversal_pairs);
    restart_traversal(); observe(0);
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, other_thread_sibling, NULL));
    CHECK(!pthread_join(thread, NULL));
    CHECK(projection_stack_rejected == 1 && !projection_traversal_pairs && !projection_traversal.open);

    // Exact stack/callsite/forward-link/config/base guards fail closed. Restore
    // the fixture each time so each failure has only one cause.
#define BAD_STACK(at, value) do { \
    const uint64_t saved_value = *(uint64_t *)(uintptr_t)(at); \
    restart_traversal(); observe(0); u64((at), (value)); observe(1); \
    CHECK(!projection_traversal_pairs && projection_stack_rejected == 1 && !projection_traversal.open); \
    u64((at), saved_value); \
} while (0)
    BAD_STACK(frames[1] + 8, PROJ_SIBLING_RETURN + 4);
    BAD_STACK(frames[1], frames[0] + 16);
    BAD_STACK(frames[0] + 8, PROJ_ROOT_RETURN + 4);
    BAD_STACK(layers[0] + 0x208, layers[1] + 8);
    BAD_STACK(layers[0] + 0x210, region + 0x3100);
    BAD_STACK(layers[1] + 0x210, region + 0x3100);
    BAD_STACK(layers[0] + 0x220, layers[0] + 0x290);
    BAD_STACK(layers[1] + 0x80, base + 8);
#undef BAD_STACK
    restart_traversal(); observe(0);
    *(uint16_t *)(uintptr_t)(layers[1] + 0x90) = 0;
    observe(1); CHECK(!projection_traversal_pairs && projection_stack_rejected == 1);
    *(uint16_t *)(uintptr_t)(layers[1] + 0x90) = 0x1000;
    restart_traversal(); observe(0);
    *(float *)(uintptr_t)(base + 0x38) += 1;
    observe(1); CHECK(!projection_traversal_pairs && projection_stack_rejected == 1);
    *(float *)(uintptr_t)(base + 0x38) -= 1;
    const uint64_t invalid_frames[] = {0, GUEST_HI - 8, frames[0] + 8, region + 0x4000};
    for (unsigned i = 0; i < sizeof invalid_frames / sizeof invalid_frames[0]; ++i) {
        restart_traversal();
        CHECK(!pc_vr_projection_trace_root(invalid_frames[i]));
        pc_vr_projection_trace_eye(layers[0], base, 5, 77, 3, true, invalid_frames[i], &mock_pose);
        CHECK(projection_stack_rejected == 1 && !projection_traversal.open);
    }
    const uint64_t invalid_parents[] = {0, frames[0], frames[0] - 16, frames[0] + 8};
    for (unsigned i = 0; i < sizeof invalid_parents / sizeof invalid_parents[0]; ++i) {
        restart_traversal(); u64(frames[0], invalid_parents[i]); observe(0);
        CHECK(!pc_vr_projection_trace_root(frames[0]));
        CHECK(projection_stack_rejected == 1 && !projection_roots);
    }
    u64(frames[0], region + 0x3900);
    restart_traversal(); observe(0);
    pc_vr_projection_trace_eye(0, base, 5, 77, 3, true, frames[0], &mock_pose);
    observe(1); CHECK(!projection_traversal_pairs && projection_stack_rejected == 2);

    // Admission is stricter than orientation-only head look and retains no
    // earlier valid snapshot after a busy/invalid root. No eye-specific retry.
#define BAD_NATIVE(change) do { \
    restart_traversal(); change; mock_head_pose = mock_pose; observe(0); \
    CHECK(projection_roots == 1 && !projection_traversal.native_latched); \
    native_pose(); observe(1); \
    CHECK(projection_traversal_pairs == 1 && !projection_traversal.native_pair); \
} while (0)
    BAD_NATIVE(mock_pose_result = XR_POSE_BUSY);
    BAD_NATIVE(mock_pose_result = XR_POSE_UNAVAILABLE);
    BAD_NATIVE(mock_pose.active = false);
    BAD_NATIVE(mock_pose.orientation_valid = false);
    BAD_NATIVE(mock_pose.orientation_tracked = false);
    BAD_NATIVE(mock_pose.views_valid = false);
    BAD_NATIVE(mock_pose.native_views_valid = false);
    BAD_NATIVE(mock_pose.frame_id = 0);
    BAD_NATIVE(mock_pose.generation = 0);
    BAD_NATIVE(mock_pose.published_monotonic_ns = 0);
    BAD_NATIVE(mock_pose.published_monotonic_ns = mock_now + 1);
    BAD_NATIVE(mock_pose.published_monotonic_ns = mock_now - PROJ_POSE_MAX_AGE_NS - 1);
    BAD_NATIVE(mock_pose.orientation_xyzw[3] = 0);
    BAD_NATIVE(mock_pose.eyes[0].orientation_xyzw[0] = NAN);
    BAD_NATIVE(mock_pose.eyes[1].orientation_xyzw[3] = 0);
    BAD_NATIVE(mock_pose.eyes[0].position_xyz[1] = INFINITY);
    BAD_NATIVE(mock_pose.eyes[1].position_xyz[2] = NAN);
    BAD_NATIVE(mock_pose.eyes[0].fov_left_right_up_down[0] = NAN);
    BAD_NATIVE(mock_pose.eyes[1].fov_left_right_up_down[1] = 2);
    BAD_NATIVE(mock_pose.eyes[0].fov_left_right_up_down[0] = 1);
    BAD_NATIVE(mock_pose.eyes[1].fov_left_right_up_down[3] = 1);
    BAD_NATIVE(mock_pose.angular_velocity_xyz[0] = NAN);
#undef BAD_NATIVE
#define BAD_SIBLING_NATIVE(change) do { \
    restart_traversal(); observe(0); change; observe(1); \
    CHECK(projection_traversal_pairs == 1 && projection_traversal.native_latched); \
    CHECK(!projection_traversal.native_pair); \
} while (0)
    BAD_SIBLING_NATIVE(mock_pose_result = XR_POSE_BUSY);
    BAD_SIBLING_NATIVE(mock_pose_result = XR_POSE_UNAVAILABLE);
    BAD_SIBLING_NATIVE(++mock_pose.generation);
    BAD_SIBLING_NATIVE(mock_pose.native_views_valid = false);
    BAD_SIBLING_NATIVE(mock_now += PROJ_POSE_MAX_AGE_NS + 1; mock_pose.published_monotonic_ns = mock_now);
#undef BAD_SIBLING_NATIVE
    restart_traversal(); mock_pose.focused = false;
    observe(0); observe(1); CHECK(projection_traversal.native_pair);
    restart_traversal(); observe(0); mock_pose.focused = false;
    observe(1); CHECK(projection_traversal.native_pair);
    restart_traversal(); mock_pose.angular_velocity_valid = false;
    mock_pose.angular_velocity_xyz[0] = NAN;
    observe(0); observe(1); CHECK(projection_traversal.native_pair); // Optional velocity remains independent.

    restart_traversal(); observe(0);
    const uint64_t reset_serial = projection_traversal.serial;
    pc_vr_projection_trace_reset();
    CHECK(!projection_traversal.open && !projection_traversal.native_latched && !projection_traversal.serial);
    restart_traversal(); observe(1); CHECK(!projection_traversal_pairs);
    observe(0); CHECK(projection_traversal.serial > reset_serial);
    // (no deadline: a sibling 5 s on still links, and its pair is not native, the root's sample being too old)
    mock_now += UINT64_C(5000000000); observe(1);
    CHECK(atomic_load(&projection_enabled) && !projection_traversal.open && !projection_traversal.native_pair);
    CHECK(projection_traversal_pairs == 1 && projection_traversal.native_latched);
    const unsigned disabled_reads = pose_reads;
    pc_vr_projection_trace_reset(); observe(0);
    CHECK(pose_reads == disabled_reads && !projection_traversal.native_latched);
    CHECK(!memcmp(before, (void *)(uintptr_t)region, sizeof before));
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

static void test_native_fov_observation(void) {
    fixture_objects();
    for (unsigned i = 0; i < 2; ++i)
        *(uint16_t *)(uintptr_t)(layers[i] + 0x288 + 8) = 1;
    unsigned char before[0x4000]; memcpy(before, (void *)(uintptr_t)region, sizeof before);
    restart_traversal(); observe(0); observe(1);
    CHECK(projection_fov_attempts == 2 && projection_notes[0].native_fov_ready && projection_notes[1].native_fov_ready);
    CHECK(projection_notes[0].basis.offset_sign == -1 && projection_notes[1].basis.offset_sign == 1);
    const uint64_t object = layers[0] + 0x288;
    call_writer(object, true, PROJ_DEVICE_CALLER, object + 0x4c);
    CHECK(!projection_notes[0].logical_seen); // Device-only observation cannot prove writer ordering.
    call_writer(object, false, PROJ_BUILD_CALLER, object + 0xc);
    call_writer(object, true, PROJ_DEVICE_CALLER, object + 0x4c);
    CHECK(projection_notes[0].logical_seen);
    CHECK(!memcmp(before, (void *)(uintptr_t)region, sizeof before));

    restart_traversal(); mock_pose.eyes[0].position_xyz[0] = 0.04f;
    mock_pose.eyes[1].position_xyz[0] = -0.04f; mock_head_pose = mock_pose; observe(0); observe(1);
    CHECK(projection_fov_attempts == 2);
    CHECK(!projection_notes[0].native_fov_ready && !projection_notes[1].native_fov_ready);
    restart_traversal(); *(float *)(uintptr_t)(base + 8) = -1; observe(0); observe(1);
    CHECK(!projection_notes[0].native_fov_ready && !projection_notes[1].native_fov_ready);
    CHECK(!projection_notes[0].basis.right_matches);
    *(float *)(uintptr_t)(base + 8) = 1;
    restart_traversal(); *(float *)(uintptr_t)(layers[0] + 0x26c) += 0.1f; observe(0);
    CHECK(!projection_notes[0].native_fov_ready && !projection_notes[0].basis.parallel_eye);
    *(float *)(uintptr_t)(layers[0] + 0x26c) -= 0.1f;
    restart_traversal(); *(uint16_t *)(uintptr_t)(object + 8) = 0; observe(0);
    CHECK(!projection_notes[0].native_fov_ready); // A future write needs the verified dirty logical stage.
    *(uint16_t *)(uintptr_t)(object + 8) = 1;
    restart_traversal(); *(float *)(uintptr_t)(object + 0x98) = -1; observe(0);
    CHECK(!projection_notes[0].native_fov_ready);
    *(float *)(uintptr_t)(object + 0x98) = 0.125f;
    restart_traversal(); observe(0); ++mock_pose.generation; observe(1);
    CHECK(projection_notes[0].native_fov_ready && !projection_notes[1].native_fov_ready);
    pc_vr_projection_trace_reset(); observe(0); CHECK(projection_fov_attempts == 2);  // (not armed: no dry run)
    CHECK(!memcmp(before, (void *)(uintptr_t)region, sizeof before));
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

static unsigned char experiment_clean[0x4000];
// A fresh start for the writes: the game reset, the clean guest objects, the trace armed again at this world scale.
static void restart_view_at(double units_per_meter) {
    pc_vr_projection_trace_reset();
    memcpy((void *)(uintptr_t)region, experiment_clean, sizeof experiment_clean);
    fixture_init_at(units_per_meter); native_pose();
    mock_generate_projection = true; mock_bad_logical = mock_bad_device = false;
    fail_next_parameter_write = false;
    parameter_write_calls = 0;
}
static void restart_view(void) { restart_view_at(1.0); }
// What the current traversal's pairs came to, from its own state (kept until the next root, a rejected traversal or
// the game reset). Complete: both eyes written and verified, FOV and view. Partial: finished with an eye written, and
// not complete: it gets no certificate pair.
static bool fov_pair_complete(void) {
    const ProjectionTraversal *traversal = &projection_traversal;
    return traversal->fov_finished && !traversal->fov_failed && traversal->native_pair &&
        traversal->fov_written_mask == 3 && traversal->fov_logical_mask == 3 && traversal->fov_device_mask == 3;
}
static bool fov_pair_partial(void) {
    return projection_traversal.fov_finished && projection_traversal.fov_written_mask && !fov_pair_complete();
}
static bool view_pair_complete(void) {
    const ProjectionTraversal *traversal = &projection_traversal;
    return fov_pair_complete() && traversal->view_finished && !traversal->view_failed &&
        traversal->view_written_mask == 3 && traversal->view_verified_mask == 3;
}
static bool view_pair_partial(void) {
    return projection_traversal.view_finished && projection_traversal.view_written_mask && !view_pair_complete();
}
static void acknowledge(unsigned side, bool applied) {
    pc_vr_projection_trace_head_result(layers[side], 5, applied);
}
static void render_observed(unsigned side) {
    const uint64_t object = layers[side] + 0x288;
    call_writer(object, false, PROJ_BUILD_CALLER, object + 0xc);
    // These flag changes belong to the original accessor surrounding the
    // hooked writers, never to the experiment.
    *(uint16_t *)(uintptr_t)(object + 8) = 0x100;
    call_writer(object, true, PROJ_DEVICE_CALLER, object + 0x4c);
    *(uint8_t *)(uintptr_t)(object + 9) = 0;
}

static PcVrNativeViewReference view_reference = {.orientation_xyzw = {0, 0, 0, 1},
    .origin_xyz = {0, 1.5f, 0.3f}, .generation = 3};
static PcVrNativeViewResult prepare_view(unsigned side, const PcVrNativeViewReference *reference,
    PcVrNativeViewToken *token) {
    PcVrHeadLookCamera center, original;
    memcpy(&center, (void *)(uintptr_t)(base + 0x38), sizeof center);
    memcpy(&original, (void *)(uintptr_t)(layers[side] + 0x260), sizeof original);
    return pc_vr_projection_trace_native_view(layers[side], 5, &center, &original, reference, token);
}
// Independent conventional RH look-at construction for the mocked original
// builder. The pure fixture separately compares the checker with Khronos.
static void emulate_view(unsigned side) {
    PcVrHeadLookCamera eye;
    memcpy(&eye, (void *)(uintptr_t)(layers[side] + 0x260), sizeof eye);
    double back[3], right[3], up[3];
    double length = 0;
    for (unsigned i = 0; i < 3; ++i) { back[i] = (double)eye.position[i] - eye.target[i]; length += back[i] * back[i]; }
    length = sqrt(length);
    for (unsigned i = 0; i < 3; ++i) back[i] /= length;
    right[0] = eye.up[1] * back[2] - eye.up[2] * back[1];
    right[1] = eye.up[2] * back[0] - eye.up[0] * back[2];
    right[2] = eye.up[0] * back[1] - eye.up[1] * back[0];
    length = sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    for (unsigned i = 0; i < 3; ++i) right[i] /= length;
    up[0] = back[1] * right[2] - back[2] * right[1];
    up[1] = back[2] * right[0] - back[0] * right[2];
    up[2] = back[0] * right[1] - back[1] * right[0];
    const double *rows[3] = {right, up, back};
    float matrix[12];
    for (unsigned row = 0; row < 3; ++row) {
        double translation = 0;
        for (unsigned col = 0; col < 3; ++col) {
            matrix[row * 4 + col] = (float)rows[row][col];
            translation -= rows[row][col] * eye.position[col];
        }
        matrix[row * 4 + 3] = (float)translation;
    }
    memcpy((void *)(uintptr_t)(layers[side] + 0x230), matrix, sizeof matrix);
}
static PcVrNativeViewToken native_camera(unsigned side) {
    observe(side);
    PcVrNativeViewToken token = {0};
    CHECK(prepare_view(side, &view_reference, &token) == PC_VR_NATIVE_VIEW_APPLIED);
    CHECK(token.note && projection_notes[side].native_fields_applied && !projection_notes[side].native_view_verified);
    CHECK(!projection_notes[side].head_acknowledged); // Never a legacy acknowledgement.
    emulate_view(side);
    CHECK(pc_vr_projection_trace_native_view_built(&token, true));
    return token;
}

// The native FOV: each eye's seven projection parameters written at its projection writer, once the eye's camera is
// its verified native view (test_native_view_experiment), and the writers' results verified.
static void test_fov_experiment(void) {
    fixture_objects();
    for (unsigned i = 0; i < 2; ++i) *(uint16_t *)(uintptr_t)(layers[i] + 0x290) = 1;
    memcpy(experiment_clean, (void *)(uintptr_t)region, sizeof experiment_clean);
    const uint64_t object = layers[0] + 0x288;

    // Actual live order: both cameras precede either projection writer. Admission waits for their verified views.
    restart_view(); native_camera(0); native_camera(1);
    CHECK(!projection_traversal.open && projection_traversal.native_pair);
    render_observed(0); render_observed(1);
    CHECK(projection_traversal.fov_admitted && projection_traversal.fov_written_mask == 3 && fov_pair_complete());
    CHECK(!projection_fov_rejected);
    CHECK(projection_traversal.fov_logical_mask == 3 && projection_traversal.fov_device_mask == 3);
    // (two camera writes, then the two parameter writes)
    CHECK(parameter_write_calls == 4 && last_parameter_write_size == 7 * sizeof(float));
    CHECK(last_parameter_write_address == layers[1] + 0x328);
    CHECK(!memcmp((void *)(uintptr_t)(object + 0x98), experiment_clean + (object + 0x98 - region), 2 * sizeof(float)));
    CHECK(!memcmp((void *)(uintptr_t)(object + 0x8c), experiment_clean + (object + 0x8c - region), 3 * sizeof(float)));
    bool only_owned_output_changed = true;
    for (unsigned byte = 0; byte < sizeof experiment_clean; ++byte) {
        const uint64_t at = region + byte;
        bool allowed = false;
        for (unsigned side = 0; side < 2; ++side) {
            const uint64_t p = layers[side] + 0x288, camera = layers[side] + 0x230;
            allowed |= (at >= p + 8 && at < p + 0x8c) || (at >= p + 0xa0 && at < p + 0xbc) ||
                (at >= camera && at < camera + 0x30 + sizeof(PcVrHeadLookCamera));  // (the view matrix and camera)
        }
        if (!allowed && *(unsigned char *)(uintptr_t)at != experiment_clean[byte]) only_owned_output_changed = false;
    }
    CHECK(only_owned_output_changed); // Bindings, n/f, posture and depth unchanged.
    render_observed(0); render_observed(1);
    CHECK(parameter_write_calls == 4 && fov_pair_complete()); // Never patch a repeated writer again.

    // Interleaved order (each eye's camera, then its writers) retains the root's complete runtime snapshot even if a
    // newer publication exists before the sibling.
    restart_view(); native_camera(0); render_observed(0);
    const XrPoseSnapshot captured = projection_traversal.native_snapshot;
    ++mock_pose.frame_id; mock_pose.eyes[1].fov_left_right_up_down[1] += 0.1f;
    native_camera(1); render_observed(1);
    CHECK(fov_pair_complete() && parameter_write_calls == 4);
    CHECK(!memcmp(projection_notes[1].fov_angles, captured.eyes[1].fov_left_right_up_down, sizeof projection_notes[1].fov_angles));

    // The writer's own guards, after a verified native view: tracking busy or too old, a posture other than 0, a
    // logical stage not dirty, and inconsistent cached parameters leave the parameters unwritten.
    restart_view(); native_camera(0);
    mock_pose_result = XR_POSE_BUSY; render_observed(0); CHECK(!projection_traversal.fov_written_mask);
    restart_view(); native_camera(0);
    mock_now += PROJ_POSE_MAX_AGE_NS + 1; render_observed(0); CHECK(!projection_traversal.fov_written_mask);
    restart_view(); native_camera(0);
    *(uint32_t *)(uintptr_t)(object + 0x8c) = 1;
    render_observed(0); // Only verified posture 0.
    CHECK(!projection_traversal.fov_written_mask && projection_fov_rejected == 1);
    restart_view(); native_camera(0);
    *(uint16_t *)(uintptr_t)(object + 8) = 0;
    render_observed(0); CHECK(!projection_traversal.fov_written_mask);
    restart_view(); native_camera(0);
    *(float *)(uintptr_t)(object + 0xa4) += 0.1f;
    render_observed(0); CHECK(!projection_traversal.fov_written_mask); // Reject inconsistent original caches.
    CHECK(parameter_write_calls == 1 && atomic_load(&projection_fov_experiment));  // (the camera's write only)

    // A rendered first eye cannot be rolled back: a sibling that fails after the root's parameters were written leaves
    // a partial pair (no certificate pair), and the writes go on.
    restart_view(); native_camera(0); render_observed(0);
    const unsigned certificates_before = atomic_load(&render_certificate_count);
    ++mock_pose.generation; observe(1);
    PcVrNativeViewToken token = {0};
    CHECK(prepare_view(1, &view_reference, &token) == PC_VR_NATIVE_VIEW_REJECTED && !token.note);
    render_observed(1);
    CHECK(projection_traversal.fov_written_mask == 1 && fov_pair_partial() && view_pair_partial());
    CHECK(atomic_load(&render_certificate_count) == certificates_before);  // (the root has its own; the sibling none)
    CHECK(atomic_load(&projection_fov_experiment) && atomic_load(&projection_view_experiment));
    restart_view(); native_camera(0); render_observed(0);
    observe(0);  // (a new root abandons it, and starts afresh)
    CHECK(projection_traversal.open && !projection_traversal.fov_admitted && atomic_load(&projection_fov_experiment));

    // A failed parameter write (it touches a prefix) is restored exactly and turns the writes off until the trace is
    // armed again.
    restart_view(); native_camera(0);
    fail_next_parameter_write = true; render_observed(0);
    CHECK(parameter_write_calls == 3 && !projection_traversal.fov_written_mask);
    CHECK(!atomic_load(&projection_fov_experiment));
    CHECK(!memcmp((void *)(uintptr_t)(object + 0x98), experiment_clean + (object + 0x98 - region), 9 * sizeof(float)));
    // The original writers' matrices are verified: a wrong one leaves a partial pair, and the writes go on.
    restart_view(); native_camera(0);
    mock_bad_logical = true; render_observed(0);
    CHECK(projection_traversal.fov_written_mask == 1 && !projection_traversal.fov_logical_mask && fov_pair_partial());
    CHECK(atomic_load(&projection_fov_experiment));
    restart_view(); native_camera(0);
    mock_bad_device = true; render_observed(0);
    CHECK(projection_traversal.fov_logical_mask == 1 && !projection_traversal.fov_device_mask && fov_pair_partial());
    CHECK(atomic_load(&projection_fov_experiment));
    pc_vr_projection_trace_reset();
    mock_generate_projection = false;
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

static void test_native_view_experiment(void) {
    fixture_objects();
    for (unsigned i = 0; i < 2; ++i) *(uint16_t *)(uintptr_t)(layers[i] + 0x290) = 1;
    memcpy(experiment_clean, (void *)(uintptr_t)region, sizeof experiment_clean);
    // (the units per metre the module refuses: not finite, or not above 0; their text forms are the configuration's)
    // Nothing is armed: head look composes the eyes itself (legacy), and nothing is written.
    const double bad_scales[] = {0, -0.0, -1, NAN, INFINITY, -INFINITY};
    for (unsigned i = 0; i < sizeof bad_scales / sizeof bad_scales[0]; ++i) {
        restart_view_at(bad_scales[i]);
        CHECK(!atomic_load(&projection_enabled) && !atomic_load(&projection_view_experiment) &&
              !atomic_load(&projection_fov_experiment) && !atomic_load(&render_collecting));
        CHECK(!pc_vr_world_scale_active() && pc_vr_world_scale(1.25) == 0);
        observe(0); PcVrNativeViewToken legacy_token = {0};
        CHECK(prepare_view(0, &view_reference, &legacy_token) == PC_VR_NATIVE_VIEW_LEGACY && !legacy_token.note);
        CHECK(!parameter_write_calls && !memcmp(experiment_clean, (void *)(uintptr_t)region, sizeof experiment_clean));
    }
    // Live world scale: multiplies, stays within [0.1, 100], and 0 restores the launch value.
    restart_view_at(2);
    CHECK(pc_vr_world_scale_active() && pc_vr_world_scale(1.25) == 2.5 && projection_view_scale == 2.5);
    CHECK(fabs(pc_vr_world_scale(0.8) - 2) < 1e-12);
    for (unsigned i = 0; i < 40; ++i) (void)pc_vr_world_scale(1.25);
    CHECK(projection_view_scale == 100);
    for (unsigned i = 0; i < 60; ++i) (void)pc_vr_world_scale(0.8);
    CHECK(projection_view_scale == 0.1);
    CHECK(pc_vr_world_scale(0) == 2 && projection_view_scale == 2);
    PcVrNativeViewToken token = {0};

    // Pending calibration skips both cameras/FOV, even after the root legacy
    // composition makes an established reference available to its sibling.
    restart_view(); observe(0);
    CHECK(prepare_view(0, NULL, &token) == PC_VR_NATIVE_VIEW_SKIPPED);
    acknowledge(0, true); observe(1);
    CHECK(prepare_view(1, &view_reference, &token) == PC_VR_NATIVE_VIEW_SKIPPED);
    acknowledge(1, true); render_observed(0); render_observed(1);
    CHECK(projection_traversal.view_skipped && !projection_traversal.view_admitted && !projection_view_rejected);
    CHECK(!projection_traversal.fov_admitted && !projection_fov_rejected && !parameter_write_calls && !token.note);

    // Live ordering: both original camera builders return before either
    // logical/device writer. Full canted views survive this closed-pair path.
    restart_view();
    for (unsigned side = 0; side < 2; ++side) {
        mock_pose.eyes[side].orientation_xyzw[1] = (float)sin(side ? -0.08 : 0.08);
        mock_pose.eyes[side].orientation_xyzw[3] = (float)cos(0.08);
    }
    mock_head_pose = mock_pose;
    native_camera(0); native_camera(1);
    CHECK(!projection_traversal.open && projection_traversal.view_verified_mask == 3);
    CHECK(projection_traversal.view_pair.contracts.canted_from_center[0] &&
          projection_traversal.view_pair.contracts.canted_from_center[1]);
    render_observed(0); render_observed(1);
    CHECK(view_pair_complete());
    CHECK(projection_traversal.view_written_mask == 3 && projection_traversal.view_verified_mask == 3);
    CHECK(!projection_view_rejected && parameter_write_calls == 4 && projection_traversal.view_finished);

    // A publication after every caller clock read must remain eligible at
    // root, sibling, native prepare/post-build and both FOV writers. The
    // original rendered sample must still be byte-identical throughout.
    restart_view();
    fprintf(stderr, "[fixture] publication-between-clock-and-pose regression\n");
    const XrPoseSnapshot retained = mock_head_pose;
    mock_publication_step = UINT64_C(1000000);
    native_camera(0); native_camera(1); render_observed(0); render_observed(1);
    CHECK(projection_roots == 1 && projection_traversal.native_latched && projection_traversal.native_pair);
    CHECK(view_pair_complete());
    CHECK(!projection_view_rejected && !projection_fov_rejected);
    CHECK(!memcmp(&retained, &projection_traversal.native_snapshot, sizeof retained));
    CHECK(mock_pose.frame_id > retained.frame_id && mock_pose.published_monotonic_ns > retained.published_monotonic_ns);
    mock_publication_step = 0;
    // Keep genuine future/stale/current-generation rejection. A newer current
    // sample cannot refresh the older rendered sample's age or generation.
    restart_view(); mock_pose.published_monotonic_ns = mock_now + 1;
    observe(0); CHECK(!projection_traversal.native_latched);
    restart_view(); ++mock_pose.generation;
    observe(0); CHECK(!projection_traversal.native_latched);
    restart_view(); mock_publication_step = PROJ_POSE_MAX_AGE_NS + 1;
    observe(0); CHECK(!projection_traversal.native_latched);
    mock_publication_step = 0;

    // Explicit scale is consumed by the integration, never replaced by 1:1.
    restart_view_at(2.0);
    native_camera(0); native_camera(1); render_observed(0); render_observed(1);
    CHECK(projection_traversal.view_pair.game_units_per_meter == 2.0 && view_pair_complete());
    CHECK(fabs((double)projection_traversal.view_pair.eyes[1].position[0] -
        projection_traversal.view_pair.eyes[0].position[0] - 0.128) < 0.00001);

    // Known unsupported perspective state must fail before any native camera
    // write, not produce an avoidable view-only partial at its later writer.
    for (unsigned fault = 0; fault < 4; ++fault) {
        restart_view(); observe(0); token = (PcVrNativeViewToken){0};
        const uint64_t object = layers[0] + 0x288;
        if (fault == 0) *(uint32_t *)(uintptr_t)(object + 0x8c) = 1;
        if (fault == 1) *(uint16_t *)(uintptr_t)(object + 8) = 2;
        if (fault == 2) *(float *)(uintptr_t)(object + 0x90) = NAN;
        if (fault == 3) *(float *)(uintptr_t)(object + 0xa4) += 0.1f;
        CHECK(prepare_view(0, &view_reference, &token) == PC_VR_NATIVE_VIEW_REJECTED);
        CHECK(!parameter_write_calls && !projection_traversal.view_written_mask && !view_pair_partial() && !token.note);
    }

    // Interleaved writers retain the root snapshot even with newer publication.
    restart_view(); native_camera(0); render_observed(0);
    const XrPoseSnapshot captured = projection_traversal.native_snapshot;
    ++mock_pose.frame_id; mock_pose.eyes[1].position_xyz[0] += 0.01f;
    native_camera(1); render_observed(1);
    CHECK(view_pair_complete());
    CHECK(!memcmp(&captured, &projection_traversal.view_pair.snapshot, sizeof captured));

    // Native camera fields alone cannot admit FOV; actual original view is mandatory. The partial pair gets no
    // certificate pair, and the writes go on.
    restart_view(); observe(0); token = (PcVrNativeViewToken){0};
    CHECK(prepare_view(0, &view_reference, &token) == PC_VR_NATIVE_VIEW_APPLIED);
    CHECK(last_parameter_write_size == sizeof(PcVrHeadLookCamera));
    render_observed(0);
    CHECK(!projection_traversal.fov_written_mask && view_pair_partial() && atomic_load(&projection_view_experiment));

    // A failed 36-byte copy touches a prefix, then restores the entire original.
    restart_view(); observe(0); token = (PcVrNativeViewToken){0};
    fail_next_parameter_write = true;
    CHECK(prepare_view(0, &view_reference, &token) == PC_VR_NATIVE_VIEW_REJECTED);
    CHECK(!token.note && !projection_traversal.view_written_mask && !view_pair_partial());
    CHECK(parameter_write_calls == 2 && !atomic_load(&projection_view_experiment));
    CHECK(!memcmp((void *)(uintptr_t)(layers[0] + 0x260),
        experiment_clean + layers[0] + 0x260 - region, sizeof(PcVrHeadLookCamera)));

    // Actual matrix failures, including translation corruption, do not repair
    // the game matrix or admit FOV. A partial pair is terminal for its traversal; the writes go on.
    for (unsigned entry = 0; entry < 12; ++entry) {
        restart_view(); observe(0); token = (PcVrNativeViewToken){0};
        CHECK(prepare_view(0, &view_reference, &token) == PC_VR_NATIVE_VIEW_APPLIED);
        emulate_view(0);
        ((float *)(uintptr_t)(layers[0] + 0x230))[entry] += 0.1f;
        CHECK(!pc_vr_projection_trace_native_view_built(&token, true));
        CHECK(view_pair_partial() && !projection_traversal.view_verified_mask);
        CHECK(!projection_traversal.fov_written_mask);
        CHECK(atomic_load(&projection_view_experiment) && atomic_load(&projection_fov_experiment));
    }
    restart_view(); observe(0); token = (PcVrNativeViewToken){0};
    CHECK(prepare_view(0, &view_reference, &token) == PC_VR_NATIVE_VIEW_APPLIED);
    emulate_view(0); *(float *)(uintptr_t)(layers[0] + 0x260) += 1;
    CHECK(!pc_vr_projection_trace_native_view_built(&token, true));
    CHECK(view_pair_partial() && !projection_traversal.view_verified_mask);
    restart_view(); observe(0); token = (PcVrNativeViewToken){0};
    CHECK(prepare_view(0, &view_reference, &token) == PC_VR_NATIVE_VIEW_APPLIED);
    emulate_view(0);
    CHECK(!pc_vr_projection_trace_native_view_built(&token, false));
    CHECK(view_pair_partial() && !projection_traversal.view_verified_mask);

    restart_view(); native_camera(0); observe(1); acknowledge(1, false);
    render_observed(0); render_observed(1);
    CHECK(view_pair_partial() && projection_traversal.view_written_mask == 1 && !projection_traversal.fov_written_mask);
    restart_view(); native_camera(0); observe(1); // Known sibling has no actual view acknowledgement.
    render_observed(0); CHECK(!projection_traversal.fov_written_mask && view_pair_partial());
    restart_view(); native_camera(0); native_camera(1);
    mock_bad_logical = true; render_observed(0);
    CHECK(view_pair_partial() && fov_pair_partial());
    CHECK(atomic_load(&projection_view_experiment) && atomic_load(&projection_fov_experiment));

    // Changed calibration, frame, stale tokens and original-state mismatches
    // cannot borrow an earlier camera result.
    restart_view(); native_camera(0); observe(1);
    PcVrNativeViewReference changed = view_reference; changed.orientation_xyzw[1] = 0.01f;
    token = (PcVrNativeViewToken){0};
    CHECK(prepare_view(1, &changed, &token) == PC_VR_NATIVE_VIEW_REJECTED);
    CHECK(!token.note && view_pair_partial());
    restart_view(); ++mock_pose.frame_id; observe(0);
    CHECK(prepare_view(0, &view_reference, &token) == PC_VR_NATIVE_VIEW_APPLIED);
    CHECK(projection_traversal.view_written_mask == 1);
    restart_view(); token = native_camera(0);
    CHECK(!pc_vr_projection_trace_native_view_built(&token, true)); // Duplicate completion is not another eye.
    CHECK(projection_traversal.view_verified_mask == 1 && view_pair_partial());
    restart_view(); observe(0); token = (PcVrNativeViewToken){0};
    CHECK(prepare_view(0, &view_reference, &token) == PC_VR_NATIVE_VIEW_APPLIED);
    emulate_view(0); ++token.traversal_serial;
    CHECK(!pc_vr_projection_trace_native_view_built(&token, true));
    CHECK(!projection_traversal.view_verified_mask && view_pair_partial());

    // A sibling 5 s after its root (no deadline) is refused for the root's old sample; the pair is partial and the
    // writes go on.
    restart_view(); native_camera(0); mock_now += UINT64_C(5000000000); observe(1);
    token = (PcVrNativeViewToken){0};
    CHECK(prepare_view(1, &view_reference, &token) == PC_VR_NATIVE_VIEW_REJECTED && !token.note);
    CHECK(view_pair_partial() && atomic_load(&projection_view_experiment));
    pc_vr_projection_trace_reset();
    CHECK(!projection_traversal.view_admitted && !atomic_load(&projection_view_experiment));  // (the game reset)
    mock_generate_projection = false;
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}
static uint64_t render_rinfo[2], render_context, render_cb;
static void fixture_render_info(void) {
    render_rinfo[0] = region + 0x3100; render_rinfo[1] = region + 0x3180;
    render_context = region + 0x3200; render_cb = region + 0x3400;
    for (unsigned side = 0; side < 2; ++side) {
        u64(render_rinfo[side], 7 + side);
        u64(render_rinfo[side] + 0x18, layers[side]);
        u64(render_rinfo[side] + 0x28, layers[side] + 0x228);
        u64(render_rinfo[side] + 0x30, layers[side] + 0x288);
        u64(render_rinfo[side] + 0x48, render_context);
    }
    u64(render_context + 0xb8, render_cb);
}
static void scope_call(unsigned side, bool end, uint64_t caller) {
    Cpu actual = {0}, expected;
    actual.x[0] = layers[side]; actual.x[1] = render_rinfo[side]; actual.x[30] = caller;
    actual.x[5] = 0x12345; actual.sp = region + 0x3c00;
    actual.v[3] = 123; expected = actual;
    fake_original(&expected);
    const unsigned before = originals;
    if (end) render_end_wrapper(&actual); else render_begin_wrapper(&actual);
    CHECK(originals == before + 1 && !memcmp(&actual, &expected, sizeof actual));
}
static void begin_scope(unsigned side) { scope_call(side, false, RENDER_BEGIN_CALLER); }
static void end_scope(unsigned side) { scope_call(side, true, MAIN_BASE + 0xde1560); }
static atomic_bool hold_record_query, record_query_entered, release_record_query;
static void *foreign_recording_thread(void *unused) {
    (void)unused;
    begin_scope(0);
    if (atomic_load(&hold_record_query)) {
        atomic_store(&record_query_entered, true);
        while (!atomic_load(&release_record_query)) Sleep(0);
    }
    const uint64_t id = pc_vr_render_trace_record(render_cb);
    end_scope(0);
    return (void *)(uintptr_t)id;
}
static void *concurrent_device_writer(void *unused) {
    (void)unused;
    const uint64_t projection = layers[1] + 0x288;
    Cpu c = {0};
    c.x[0] = projection; c.x[1] = projection + 0x4c; c.x[2] = projection + 0xc;
    c.x[30] = PROJ_DEVICE_CALLER;
    projection_device_wrapper(&c);
    return NULL;
}
static atomic_bool certificate_reader_ready, certificate_reader_stop, certificate_reader_failed;
static atomic_uint certificate_reader_observed;
static unsigned certificate_reader_first;  // (this test's first certificate: the earlier tests' have other scales)
static void *certificate_reader(void *unused) {
    (void)unused;
    atomic_store(&certificate_reader_ready, true);
    unsigned iterations = 0;
    while (!atomic_load(&certificate_reader_stop) || iterations < 10000) {
        const unsigned count = atomic_load_explicit(&render_certificate_count, memory_order_acquire);
        for (unsigned id = certificate_reader_first; id <= count; ++id) {
            PcVrRenderCertificate cert;
            if (!pc_vr_render_trace_resolve(id, &cert) || cert.id != id || !cert.traversal ||
                !cert.camera_hash || !cert.view_hash || !cert.logical_hash || !cert.device_hash ||
                cert.native_index > 1 || cert.units_per_meter != 1.0) atomic_store(&certificate_reader_failed, true);
        }
        atomic_store(&certificate_reader_observed, count);
        ++iterations;
    }
    return atomic_load(&certificate_reader_failed) ? (void *)1 : NULL;
}
// A draw's certificate worked out in full, as after any change through the module's own entries (they move
// render_state_version); the fixture's direct changes to the module's state and to guest memory leave it alone.
static uint64_t record_worked_out(uint64_t command_buffer) {
    atomic_fetch_add(&render_state_version, 1);
    return pc_vr_render_trace_record(command_buffer);
}
static void test_render_trace(void) {
    PcVrRenderCertificate unchanged, out;
    memset(&unchanged, 0xa5, sizeof unchanged); out = unchanged;
    CHECK(!pc_vr_render_trace_record(1));
    CHECK(!pc_vr_render_trace_resolve(0, &out) && !memcmp(&out, &unchanged, sizeof out));
    CHECK(!pc_vr_render_trace_resolve(1, NULL));

    fixture_objects();
    for (unsigned i = 0; i < 2; ++i) *(uint16_t *)(uintptr_t)(layers[i] + 0x290) = 1;
    memcpy(experiment_clean, (void *)(uintptr_t)region, sizeof experiment_clean);
    restart_view();
    CHECK(pc_vr_render_trace_enabled() && projection_registered && overrides == 4);
    // (the earlier tests published certificates of their own: this test's are counted from here)
    const unsigned published_before = atomic_load(&render_certificate_count);
    fixture_render_info(); native_camera(0); native_camera(1);
    begin_scope(0);
    CHECK(!pc_vr_render_trace_record(render_cb)); // Lazy projection is not built yet.
    render_observed(0);
    const uint64_t first = pc_vr_render_trace_record(render_cb);
    CHECK(first == published_before + 1 && pc_vr_render_trace_resolve(first, &out));
    CHECK(out.note == projection_notes[0].note && out.traversal == projection_traversal.serial && out.native_index == 0);
    const PcVrRenderCertificate first_copy = out;
    CHECK(!projection_traversal.fov_finished); // No invented sibling-complete requirement at recording.
    render_observed(1);
    CHECK(atomic_load(&render_certificate_count) == published_before + 2 && view_pair_complete());
    CHECK(pc_vr_render_trace_record(render_cb) == first);

    // Exact same-eye recording is valid on a worker; construction remains on
    // its original thread. Retain all other generation/fingerprint guards.
    pthread_t foreign;
    void *foreign_result = (void *)1;
    const int created = pthread_create(&foreign, NULL, foreign_recording_thread, NULL);
    const int joined = created ? -1 : pthread_join(foreign, &foreign_result);
    CHECK(!created && !joined && (uint64_t)(uintptr_t)foreign_result == first);
    ProjectionNote *current_note = &projection_notes[0];
    // The worker captures a scope, then waits while the generation becomes
    // stale under the same lock used by recording. It must not acquire an ID.
    atomic_store(&hold_record_query, true);
    const int stale_created = pthread_create(&foreign, NULL, foreign_recording_thread, NULL);
    if (stale_created) CHECK(false);
    while (!atomic_load(&record_query_entered)) Sleep(0);
    CHECK(!stale_created);
    pthread_mutex_lock(&projection_lock); ++current_note->traversal_serial; pthread_mutex_unlock(&projection_lock);
    atomic_store(&release_record_query, true);
    const int stale_joined = pthread_join(foreign, &foreign_result);
    CHECK(!stale_joined && !foreign_result);
    atomic_store(&hold_record_query, false);
    pthread_mutex_lock(&projection_lock); --current_note->traversal_serial; pthread_mutex_unlock(&projection_lock);
    // Each guard of the full evaluation, one at a time (record_worked_out: the fixture's own changes below do not
    // move the version, so a kept answer would hide them).
    atomic_store(&projection_enabled, false);
    CHECK(!record_worked_out(render_cb));
    atomic_store(&projection_enabled, true);
    projection_traversal.view_admitted = false;
    CHECK(!record_worked_out(render_cb)); projection_traversal.view_admitted = true;
    projection_traversal.view_failed = true;
    CHECK(!record_worked_out(render_cb)); projection_traversal.view_failed = false;
    projection_traversal.fov_failed = true;
    CHECK(!record_worked_out(render_cb)); projection_traversal.fov_failed = false;
    projection_traversal.view_skipped = true;
    CHECK(!record_worked_out(render_cb)); projection_traversal.view_skipped = false;
    current_note->native_view_verified = false;
    CHECK(!record_worked_out(render_cb)); current_note->native_view_verified = true;
    ++current_note->sequence;
    CHECK(!record_worked_out(render_cb)); --current_note->sequence;
    CHECK(record_worked_out(render_cb) == first);
    CHECK(!pc_vr_render_trace_record(render_cb + 8));  // (another command buffer: always worked out)
    u64(render_context + 0xb8, render_cb + 8); CHECK(!record_worked_out(render_cb));
    u64(render_context + 0xb8, render_cb);
    u64(render_rinfo[0] + 0x28, base); CHECK(!record_worked_out(render_cb));
    u64(render_rinfo[0] + 0x28, layers[0] + 0x228);
    CHECK(record_worked_out(render_cb) == first);

    // A layer's finishing draws carry their kind, and whether the layer is an eye's, while their call runs.
    CHECK(!pc_vr_layer_draw_recording());
    const uint32_t outer_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_FINAL);
    CHECK(!outer_draw && pc_vr_layer_draw_recording() == (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE));
    const uint32_t inner_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_PLACEMENT);
    CHECK(inner_draw == (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE) &&
        pc_vr_layer_draw_recording() == (PC_VR_LAYER_DRAW_PLACEMENT | PC_VR_LAYER_DRAW_EYE));
    rt_layer_draw_end(inner_draw); rt_layer_draw_end(outer_draw);
    CHECK(!pc_vr_layer_draw_recording());
    // The eye's layer without a certificate (its view not verified this frame) is still the eye's layer.
    current_note->native_view_verified = false;
    CHECK(!record_worked_out(render_cb));
    const uint32_t unverified_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_FINAL);
    CHECK(pc_vr_layer_draw_recording() == (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE));
    rt_layer_draw_end(unverified_draw); current_note->native_view_verified = true;
    // Before any eye layer is known (after a load), a layer may be an eye's.
    ProjectionNote known_notes[2];
    pthread_mutex_lock(&projection_lock);
    memcpy(known_notes, projection_notes, sizeof known_notes); memset(projection_notes, 0, sizeof projection_notes);
    pthread_mutex_unlock(&projection_lock);
    const uint32_t unknown_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_FINAL);
    CHECK(pc_vr_layer_draw_recording() == (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_MAYBE_EYE));
    rt_layer_draw_end(unknown_draw);
    pthread_mutex_lock(&projection_lock);
    memcpy(projection_notes, known_notes, sizeof known_notes);
    pthread_mutex_unlock(&projection_lock);

    // An unrelated nested selection suppresses the valid outer selection.
    u64(render_rinfo[1] + 0x28, base); begin_scope(1);
    const uint32_t other_layer_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_FINAL);
    CHECK(pc_vr_layer_draw_recording() == PC_VR_LAYER_DRAW_FINAL);  // (another layer's: no eye flag)
    rt_layer_draw_end(other_layer_draw);
    CHECK(!pc_vr_render_trace_record(render_cb)); end_scope(1);
    CHECK(record_worked_out(render_cb) == first);
    u64(render_rinfo[1] + 0x28, layers[1] + 0x228);
    scope_call(1, false, MAIN_BASE + 4); // Unqualified nested callback also suppresses.
    CHECK(!pc_vr_render_trace_record(render_cb)); end_scope(1);
    CHECK(record_worked_out(render_cb) == first);

    begin_scope(1);
    float *matrix = (float *)(uintptr_t)(layers[1] + 0x230);
    const float saved = matrix[0]; matrix[0] += 0.1f;
    CHECK(!pc_vr_render_trace_record(render_cb)); matrix[0] = saved;
    const uint64_t second = record_worked_out(render_cb);
    CHECK(second == first + 1); end_scope(1);
    CHECK(record_worked_out(render_cb) == first); end_scope(0);
    CHECK(!pc_vr_render_trace_record(render_cb));

    // A mismatched nested end never searches/pops the valid outer entry.
    begin_scope(0); begin_scope(1); const unsigned depth = render_tls()->depth;
    end_scope(0); CHECK(render_tls()->poisoned && render_tls()->depth == depth);
    CHECK(!pc_vr_render_trace_record(render_cb)); end_scope(1); end_scope(0);
    CHECK(render_tls()->poisoned && render_tls()->depth == depth);
    atomic_fetch_add(&render_epoch, 1); CHECK(!pc_vr_render_trace_record(render_cb));
    CHECK(!render_tls()->poisoned && !render_tls()->depth);
    for (unsigned i = 0; i <= RENDER_SCOPE_CAP; ++i) begin_scope(i & 1);
    CHECK(render_tls()->poisoned && render_tls()->depth == RENDER_SCOPE_CAP);
    CHECK(!pc_vr_render_trace_record(render_cb));
    atomic_fetch_add(&render_epoch, 1);

    // Each observed writer invalidates the current eligibility even for an
    // unsupported caller. The already recorded historical certificate survives.
    begin_scope(0); pc_vr_render_trace_invalidate_camera(layers[0] + 0x228);
    CHECK(!pc_vr_render_trace_record(render_cb)); end_scope(0);
    CHECK(pc_vr_render_trace_resolve(first, &out) && !memcmp(&out, &first_copy, sizeof out));
    begin_scope(1);
    atomic_store(&hold_original, true);
    pthread_t writer;
    const int writer_created = pthread_create(&writer, NULL, concurrent_device_writer, NULL);
    CHECK(!writer_created);
    while (!atomic_load(&original_entered)) Sleep(0);
    // The wrapper has invalidated eligibility before entering the paused
    // original writer. Concurrent recording cannot inspect half-written data.
    CHECK(!projection_notes[1].render_device_current && !projection_notes[1].render_certificate);
    CHECK(!pc_vr_render_trace_record(render_cb));
    atomic_store(&release_original, true);
    const int writer_joined = pthread_join(writer, NULL);
    CHECK(!writer_joined);
    atomic_store(&hold_original, false);
    CHECK(!pc_vr_render_trace_record(render_cb)); end_scope(1);
    CHECK(pc_vr_render_trace_resolve(second, &out));

    // Publish more certificates while another thread repeatedly resolves them.
    certificate_reader_first = (unsigned)first;
    pthread_t reader; CHECK(!pthread_create(&reader, NULL, certificate_reader, NULL));
    while (!atomic_load(&certificate_reader_ready)) Sleep(0);
    for (unsigned pair = 0; pair < 7; ++pair) {
        restart_view(); fixture_render_info();
        native_camera(0); native_camera(1); render_observed(0); render_observed(1);
        while (atomic_load(&certificate_reader_observed) < atomic_load(&render_certificate_count)) Sleep(0);
    }
    atomic_store(&certificate_reader_stop, true);
    void *reader_result = (void *)1;
    CHECK(!pthread_join(reader, &reader_result) && !reader_result);
    const unsigned published = atomic_load(&render_certificate_count);
    CHECK(published == published_before + 16);
    // After the game's reset nothing is recorded and the render scopes pass straight through, exactly once; the
    // published certificates stay resolvable for the command lists still queued.
    pc_vr_projection_trace_reset();
    CHECK(pc_vr_render_trace_enabled() && !atomic_load(&render_collecting));
    CHECK(!pc_vr_render_trace_record(render_cb));
    scope_call(0, false, RENDER_BEGIN_CALLER); scope_call(0, true, MAIN_BASE + 0xde1524);
    CHECK(pc_vr_render_trace_resolve(first, &out) && !memcmp(&out, &first_copy, sizeof out));
    out = unchanged;
    CHECK(!pc_vr_render_trace_resolve(published + 1, &out) && !memcmp(&out, &unchanged, sizeof out));

    // With no budget, slots recycle and certificates carry the traversal's latched snapshot.
    restart_view();
    CHECK(pc_vr_render_trace_enabled() && atomic_load(&render_collecting));
    const unsigned before = atomic_load(&render_certificate_count);
    unsigned complete_pairs = 0;
    for (unsigned pair = 0; pair < 40; ++pair) {
        memcpy((void *)(uintptr_t)region, experiment_clean, sizeof experiment_clean); fixture_render_info();
        native_camera(0); native_camera(1); render_observed(0); render_observed(1);
        complete_pairs += view_pair_complete();
    }
    CHECK(complete_pairs == 40);
    CHECK(atomic_load(&projection_view_experiment) && atomic_load(&projection_fov_experiment));
    const unsigned after = atomic_load(&render_certificate_count);
    CHECK(after == before + 80);
    PcVrRenderCertificate latest = unchanged;
    CHECK(pc_vr_render_trace_resolve(after, &latest) && latest.id == after && latest.native_index == 1 &&
          latest.traversal == projection_traversal.serial &&
          !memcmp(&latest.native_snapshot, &projection_traversal.native_snapshot, sizeof latest.native_snapshot));
    out = unchanged;
    CHECK(!pc_vr_render_trace_resolve(after - RENDER_CERTIFICATE_SLOTS, &out) && !memcmp(&out, &unchanged, sizeof out));
    CHECK(pc_vr_render_trace_resolve(after - RENDER_CERTIFICATE_SLOTS + 1, &out) &&
          out.id == after - RENDER_CERTIFICATE_SLOTS + 1);
    begin_scope(0); CHECK(pc_vr_render_trace_record(render_cb) == after - 1); end_scope(0);
    // A scope keeps its answer for a command buffer until what the certificate depends on changes. The game rewriting
    // the layer's render info in guest memory meanwhile (here its command buffer) is not seen by a kept answer.
    begin_scope(0);
    CHECK(pc_vr_render_trace_record(render_cb) == after - 1);  // (worked out)
    u64(render_context + 0xb8, render_cb + 8);
    for (unsigned draw = 0; draw < 5; ++draw) CHECK(pc_vr_render_trace_record(render_cb) == after - 1);  // (kept)
    CHECK(!record_worked_out(render_cb));  // (worked out, it would be none)
    u64(render_context + 0xb8, render_cb);
    CHECK(!pc_vr_render_trace_record(render_cb));  // (that answer kept in turn)
    CHECK(!pc_vr_render_trace_record(render_cb + 8));  // (another command buffer: worked out, none)
    CHECK(pc_vr_render_trace_record(render_cb) == after - 1);  // (asked again for this one: worked out)
    // A change through any of the module's own entries moves the version: the next draw is worked out again, even
    // when the change is not this note's (here a camera that is neither eye's).
    u64(render_context + 0xb8, render_cb + 8);
    pc_vr_render_trace_invalidate_camera(base);
    CHECK(!pc_vr_render_trace_record(render_cb));
    u64(render_context + 0xb8, render_cb);
    pc_vr_render_trace_invalidate_camera(layers[0] + 0x228);
    CHECK(!pc_vr_render_trace_record(render_cb));  // (this note's: no certificate now)
    pthread_mutex_lock(&projection_lock);  // (the note put back as it was, without moving the version)
    projection_notes[0].render_view_current = true; projection_notes[0].render_certificate = after - 1;
    pthread_mutex_unlock(&projection_lock);
    CHECK(!pc_vr_render_trace_record(render_cb));  // (kept)
    CHECK(record_worked_out(render_cb) == after - 1);
    end_scope(0);
    // A new scope starts without an answer.
    begin_scope(1);
    CHECK(!render_scopes.scopes[render_scopes.depth - 1].answered && pc_vr_render_trace_record(render_cb) == after);
    end_scope(1);
    // A partial traversal (one camera written, then abandoned by the next root) yields no certificate and does not
    // halt the writes.
    const unsigned before_partial = atomic_load(&render_certificate_count);
    memcpy((void *)(uintptr_t)region, experiment_clean, sizeof experiment_clean); fixture_render_info();
    native_camera(0);
    memcpy((void *)(uintptr_t)region, experiment_clean, sizeof experiment_clean); fixture_render_info();
    native_camera(0); native_camera(1); render_observed(0); render_observed(1);
    CHECK(view_pair_complete() && atomic_load(&render_certificate_count) == before_partial + 2);
    CHECK(atomic_load(&projection_view_experiment));
    pc_vr_projection_trace_reset();
    CHECK(!atomic_load(&projection_enabled) && !atomic_load(&render_collecting));
    mock_generate_projection = false;
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

// The display's pair of eye layers (pc_vr_projection_trace_eye_layer): its root and sibling calls make both layers
// the eyes' before any eye note (no camera binding yet), and another layer's draws then carry no eye flag.
static uint32_t scope_eye_layer(unsigned side) {
    begin_scope(side);
    const uint32_t answer = render_scope_eye_layer();
    end_scope(side);
    return answer;
}
static bool display_pair_is(uint64_t first_layer, uint64_t second_layer) {
    return projection_display_pair.layers[0] == first_layer && projection_display_pair.layers[1] == second_layer;
}
static void *other_thread_eye_layer(void *unused) {
    (void)unused;
    pc_vr_projection_trace_eye_layer(layers[1], frames[1]);
    return NULL;
}
static void test_display_pair(void) {
    fixture_objects(); fixture_init(); fixture_render_info();
    unsigned char before[0x4000]; memcpy(before, (void *)(uintptr_t)region, sizeof before);
    CHECK(!projection_notes[0].note && !projection_notes[1].note && display_pair_is(0, 0));
    CHECK(scope_eye_layer(0) == PC_VR_LAYER_DRAW_MAYBE_EYE);  // (nothing known yet)
    // Root, then sibling on the same thread: the pair, with the version moved on, and nothing else of the trace's.
    const uint64_t version = atomic_load(&render_state_version);
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]);
    CHECK(projection_display_pair.open && display_pair_is(0, 0));
    pc_vr_projection_trace_eye_layer(layers[1], frames[1]);
    CHECK(display_pair_is(layers[0], layers[1]) && !projection_display_pair.open && projection_display_pairs == 1);
    CHECK(atomic_load(&render_state_version) > version);
    CHECK(!projection_notes[0].note && !projection_notes[1].note && !projection_note_count && !projection_roots);
    CHECK(!memcmp(before, (void *)(uintptr_t)region, sizeof before));  // (no guest writes)
    CHECK(scope_eye_layer(0) == PC_VR_LAYER_DRAW_EYE && scope_eye_layer(1) == PC_VR_LAYER_DRAW_EYE);
    begin_scope(0);
    const uint32_t eye_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_FINAL);
    CHECK(pc_vr_layer_draw_recording() == (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE));
    rt_layer_draw_end(eye_draw); end_scope(0);
    // Another layer, and an eye layer drawn through another camera or projection: not an eye's.
    const uint64_t other_layer = region + 0x2800, other_rinfo = region + 0x3500;
    u64(other_rinfo, 9); u64(other_rinfo + 0x18, other_layer); u64(other_rinfo + 0x28, other_layer + 0x228);
    u64(other_rinfo + 0x30, other_layer + 0x288); u64(other_rinfo + 0x48, render_context);
    Cpu other_scope = {0};
    other_scope.x[0] = other_layer; other_scope.x[1] = other_rinfo; other_scope.x[30] = RENDER_BEGIN_CALLER;
    render_begin_wrapper(&other_scope);
    CHECK(render_scope_eye_layer() == 0);
    other_scope.x[30] = MAIN_BASE + 0xde1560; render_end_wrapper(&other_scope);
    CHECK(!render_tls()->poisoned && !render_tls()->depth);
    u64(render_rinfo[0] + 0x28, base); CHECK(scope_eye_layer(0) == 0); u64(render_rinfo[0] + 0x28, layers[0] + 0x228);
    u64(render_rinfo[1] + 0x30, layers[0] + 0x288); CHECK(scope_eye_layer(1) == 0);
    u64(render_rinfo[1] + 0x30, layers[1] + 0x288);
    // The same pair again changes nothing: the version stands, no new pair is counted.
    const uint64_t kept = atomic_load(&render_state_version);
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]); pc_vr_projection_trace_eye_layer(layers[1], frames[1]);
    CHECK(atomic_load(&render_state_version) == kept && projection_display_pairs == 1 &&
          display_pair_is(layers[0], layers[1]));
    // A call that is neither a root's nor a sibling's (another layer's, through the same builder) leaves an open root.
    const uint64_t unrelated_frame = region + 0x3600;
    u64(unrelated_frame, region + 0x3900); u64(unrelated_frame + 8, MAIN_BASE + 0x1234);
    fixture_init(); pc_vr_projection_trace_eye_layer(layers[0], frames[0]);
    pc_vr_projection_trace_eye_layer(other_layer, unrelated_frame);
    pc_vr_projection_trace_eye_layer(layers[1], frames[1]);
    CHECK(display_pair_is(layers[0], layers[1]));
    // Each broken stack or link test forms no pair (from a trace with none known), one cause at a time.
#define NO_PAIR(at, value) do { \
    const uint64_t saved_value = *(uint64_t *)(uintptr_t)(at); \
    fixture_init(); u64((at), (value)); \
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]); pc_vr_projection_trace_eye_layer(layers[1], frames[1]); \
    CHECK(display_pair_is(0, 0) && !projection_display_pairs); \
    CHECK(scope_eye_layer(0) == PC_VR_LAYER_DRAW_MAYBE_EYE); \
    u64((at), saved_value); \
} while (0)
    NO_PAIR(frames[1], frames[0] + 16);                   // (the sibling saved another frame pointer than the root's)
    NO_PAIR(frames[1] + 8, PROJ_SIBLING_RETURN + 4);      // (the sibling does not return into the root's call)
    NO_PAIR(frames[0] + 8, PROJ_ROOT_RETURN + 4);         // (no root call)
    NO_PAIR(layers[0] + 0x208, 0);                        // (no +0x208 link from the root to the sibling)
    NO_PAIR(layers[0] + 0x208, layers[1] + 8);
    NO_PAIR(layers[1] + 0x210, region + 0x3100);          // (another configuration)
    NO_PAIR(layers[1] + 0x228, PROJ_EYE_VTABLE + 8);      // (the sibling's camera not an eye camera)
#undef NO_PAIR
    // The same side twice forms no pair.
    fixture_init(); *(uint16_t *)(uintptr_t)(layers[1] + 0x90) = 0;
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]); pc_vr_projection_trace_eye_layer(layers[1], frames[1]);
    CHECK(display_pair_is(0, 0) && !projection_display_pair.open);
    *(uint16_t *)(uintptr_t)(layers[1] + 0x90) = 0x1000;
    // The sibling first, or on another thread than the root's, forms none either.
    fixture_init();
    pc_vr_projection_trace_eye_layer(layers[1], frames[1]); pc_vr_projection_trace_eye_layer(layers[0], frames[0]);
    CHECK(display_pair_is(0, 0) && projection_display_pair.open);
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, other_thread_eye_layer, NULL));
    CHECK(!pthread_join(thread, NULL));
    CHECK(display_pair_is(0, 0) && !projection_display_pair.open);
    // A different pair replaces the one known: the root now links to a third eye layer on the other side.
    fixture_init();
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]); pc_vr_projection_trace_eye_layer(layers[1], frames[1]);
    const uint64_t third_layer = other_layer;
    u64(third_layer + 0x288, PROJ_VTABLE); u64(third_layer + 0x220, third_layer + 0x288);
    u64(third_layer + 0x228, PROJ_EYE_VTABLE); u64(third_layer + 0x210, region + 0x3000);
    *(uint16_t *)(uintptr_t)(third_layer + 0x90) = 0x1000;
    u64(layers[0] + 0x208, third_layer);
    const uint64_t replaced = atomic_load(&render_state_version);
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]); pc_vr_projection_trace_eye_layer(third_layer, frames[1]);
    CHECK(display_pair_is(layers[0], third_layer) && projection_display_pairs == 2);
    CHECK(atomic_load(&render_state_version) > replaced);
    CHECK(scope_eye_layer(1) == 0);  // (the old sibling's layer, no longer an eye's)
    u64(layers[0] + 0x208, layers[1]);
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]); pc_vr_projection_trace_eye_layer(layers[1], frames[1]);
    CHECK(display_pair_is(layers[0], layers[1]) && scope_eye_layer(1) == PC_VR_LAYER_DRAW_EYE);
    // Once an eye note is known the notes decide alone (the display also shows the pause menu's own pair): the note's
    // layer is an eye's, the pair's other layer, without a note, is not.
    native_pose(); observe(0);
    CHECK(projection_notes[0].note && scope_eye_layer(0) == PC_VR_LAYER_DRAW_EYE);
    CHECK(!projection_notes[1].note && scope_eye_layer(1) == 0);
    // ... and the display's calls no longer replace the pair (the pause menu's own pair goes through them too).
    u64(layers[0] + 0x208, third_layer);
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]); pc_vr_projection_trace_eye_layer(third_layer, frames[1]);
    CHECK(display_pair_is(layers[0], layers[1]) && !projection_display_pair.open);
    u64(layers[0] + 0x208, layers[1]);
    // The game's reset forgets the pair, and so does arming afresh.
    pc_vr_projection_trace_reset();
    CHECK(display_pair_is(0, 0) && !projection_display_pair.open);
    fixture_init();
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]); pc_vr_projection_trace_eye_layer(layers[1], frames[1]);
    CHECK(display_pair_is(layers[0], layers[1]));
    fixture_init();
    CHECK(display_pair_is(0, 0) && !projection_display_pairs);
    pc_vr_projection_trace_reset();
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}
#endif

// The PC VR play configuration (pc_vr_projection_trace_init; runtime/pc_vr_config.h): PC VR off runs nothing; on runs
// all of it, continuously, at the configured world scale (14.53 game units per metre, or the setting's), and an alias
// given another value switches nothing off.
static void test_play_configuration(void) {
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL});  // (the configuration's own scale)
    pc_vr_projection_trace_reset();
    mock_pc_vr = false; pc_vr_projection_trace_init();
    CHECK(!atomic_load(&projection_enabled) && !atomic_load(&projection_fov_experiment));
    CHECK(!atomic_load(&projection_view_experiment) && !atomic_load(&render_collecting));
    mock_pc_vr = true;
    pc_vr_projection_trace_init();
    CHECK(atomic_load(&projection_enabled) && atomic_load(&projection_fov_experiment));
    CHECK(atomic_load(&projection_view_experiment) && pc_vr_render_trace_enabled() && atomic_load(&render_collecting));
    CHECK(projection_view_scale == 14.53 && pc_vr_world_scale_active());
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1",
        "SAVAGEWIND_PC_VR_NATIVE_VIEW_UNITS_PER_METER", "20", NULL});
    pc_vr_projection_trace_init();
    CHECK(projection_view_scale == 20 && atomic_load(&render_collecting));
    CHECK(pc_vr_world_scale(1.25) == 25 && pc_vr_world_scale(0) == 20);  // (the live scale's launch value)
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1", "SAVAGEWIND_PC_VR_PROJECTION_TRACE", "0",
        "SAVAGEWIND_PC_VR_NATIVE_RENDER_TRACE", "0", "SAVAGEWIND_PC_VR_EYE_IMAGES", "0", NULL});
    pc_vr_projection_trace_init();
    CHECK(atomic_load(&projection_enabled) && pc_vr_render_trace_enabled() && atomic_load(&render_collecting));
    CHECK(projection_view_scale == 14.53);
    pc_vr_projection_trace_reset();
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL});
}

int main(void) {
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL});
    test_math(); test_init();
#ifdef _WIN32
    test_observation();
    test_traversal();
    test_native_fov_observation();
    test_fov_experiment();
    test_native_view_experiment();
    test_render_trace();
    test_display_pair();
#endif
    test_play_configuration();
    printf("PC VR projection/native-view fixture: %u checks passed (synthetic guest pages only)\n", checks);
    return 0;
}

// hle/labo_guest_memory.h: a new access generation per call keeps its page cache cold here.
uint64_t plat_guest_access_generation(void) { static uint64_t generation; return ++generation; }
