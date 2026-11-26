// Asset-free checks of the frame record, the eyes' native view and FOV writes and the render certificates
// (runtime/hle/pc_vr_projection_trace.c, D115 step 5c). Synthetic guest objects only; the real game library, runtime
// and headset are never opened.
#include <assert.h>
#include <pthread.h>
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
// Every guest write goes through WriteProcessMemory (labo_guest_copy): counted here, and failed on request (a refused
// write: nothing copied).
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
        if (copied) *copied = 0;
        return FALSE;
    }
    return WriteProcessMemory(process, address, in, bytes, copied);
}
#define WriteProcessMemory fixture_write_process_memory
#endif
#include "test_game_sites_182.h"
#include "../runtime/pc_vr_main.h"
static int fixture_mutex_lock(pthread_mutex_t *mutex);
#define pthread_mutex_lock fixture_mutex_lock
#include "../runtime/hle/pc_vr_projection_trace.c"
#undef pthread_mutex_lock
#include "../runtime/pc_vr_guards.c"  // (the refusal counts the trace keeps)
#ifdef _WIN32
#undef WriteProcessMemory
#endif
#include "test_pc_vr_settings.h"

// Count only the camera lock, on the thread measuring its lookups. Acquisitions always proceed normally.
static _Thread_local unsigned *projection_lock_counter;
static int fixture_mutex_lock(pthread_mutex_t *mutex) {
    if (projection_lock_counter && mutex == &projection_lock) ++*projection_lock_counter;
    return pthread_mutex_lock(mutex);
}

static unsigned checks, originals, overrides, lookups;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "projection check failed line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static bool mock_pc_vr = true, missing_device, missing_render;
static uint64_t mock_now = UINT64_C(10000000000);
static XrPoseSnapshot mock_pose;
static bool mock_generate_projection;
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
// The head view's publication (runtime/hle/pc_vr_cull_view.c has its own fixture): the calls and the last base camera.
static unsigned cull_view_publications;
static uint64_t cull_view_last_base;
void pc_vr_cull_view_publish(uint64_t base_camera, const PcVrHeadLookCamera eyes[2],
                             const float fov_left_right_up_down[2][4]) {
    (void)eyes; (void)fov_left_right_up_down;
    ++cull_view_publications;
    cull_view_last_base = base_camera;
}
const GameLibTables *gamelib_tables(void) { return &game_tables; }
Module *loader_module(const char *name) { return !strcmp(name, "main") ? &loaded_main : NULL; }
bool pc_vr_main_compatible(void) { return pc_vr_main_layout(gamelib_tables(), loader_module("main")); }
void rt_fatal_trace(void) { fprintf(stderr, "unexpected fatal failure\n"); exit(99); }
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
// The projection trace armed as the play configuration arms it (pc_vr_projection_trace_init), at this world scale (the
// configuration's parser keeps it finite and above 0).
static void fixture_init_at(double units_per_meter) {
    process_configuration.settings.native_view_units_per_meter = units_per_meter;
    pc_vr_projection_trace_init();
}
static void fixture_init(void) { fixture_init_at(1.0); }

// The refusal counts (pc_vr_guards.h): marked, then compared with the mark.
static uint64_t guard_marks[PC_VR_GUARD_COUNT];
static void guards_mark(void) {
    for (unsigned reason = 0; reason < PC_VR_GUARD_COUNT; ++reason)
        guard_marks[reason] = atomic_load(&pc_vr_guard_counts[reason]);
}
// Since the mark: this reason counted this many times, and no other reason at all (PC_VR_GUARD_COUNT: nothing counted).
static bool guards_counted(PcVrGuard counted_reason, uint64_t times) {
    for (unsigned reason = 0; reason < PC_VR_GUARD_COUNT; ++reason)
        if (atomic_load(&pc_vr_guard_counts[reason]) - guard_marks[reason] != (reason == counted_reason ? times : 0))
            return false;
    return true;
}

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

// The game's two projection writers, as the fixture's originals: the logical matrix from the parameters (+0x98), and
// the device matrix from the logical one and the depth terms (+0x90).
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
        memcpy((void *)(uintptr_t)c->x[1], matrix, sizeof matrix);
    } else if (c->x[30] == PROJ_DEVICE_CALLER && c->x[1] == object + 0x4c) {
        memcpy(matrix, (void *)(uintptr_t)(object + 0xc), sizeof matrix);
        const float *depth = (const float *)(uintptr_t)(object + 0x90);
        matrix[8] *= depth[0]; matrix[9] *= depth[0];
        matrix[10] = depth[0] * (matrix[10] + depth[1] * matrix[14]);
        matrix[11] = matrix[11] * depth[0] + depth[1] * matrix[15];
        memcpy((void *)(uintptr_t)c->x[1], matrix, sizeof matrix);
    }
}

// Arming: PC VR off, a game version without PC VR's sites, or a missing original writer or render callback arms
// nothing; arming again never wraps again.
static void test_init(void) {
    mock_pc_vr = false; pc_vr_projection_trace_init(); CHECK(!overrides);  // (PC VR off: nothing)
    mock_pc_vr = true;
    // (a game version without PC VR's sites, or a main laid out otherwise: nothing registered)
    const uint64_t builder = g_site.pcvr_matrix_builder;
    g_site.pcvr_matrix_builder = 0; fixture_init(); CHECK(!overrides && !lookups);
    g_site.pcvr_matrix_builder = builder;
    loaded_main.size = 0x1e98000; fixture_init(); CHECK(!overrides && !lookups);
    loaded_main.size = TEST_MAIN_SIZE_182;
    // (an original projection writer or render scope callback missing: all four looked up, none registered, nothing
    // armed; there is no part of the chain without the rest)
    missing_device = true; fixture_init();
    CHECK(!overrides && lookups == 4 && !atomic_load(&projection_enabled));
    missing_device = false; missing_render = true; fixture_init();
    CHECK(!overrides && lookups == 8 && !atomic_load(&projection_enabled) && !pc_vr_render_trace_enabled());
    missing_render = false; fixture_init();
    CHECK(overrides == 4 && projection_registered && atomic_load(&projection_enabled) && pc_vr_render_trace_enabled());
    CHECK(atomic_load(&render_collecting) && projection_view_scale == 1.0 && !projection_record.serial);
    fixture_init(); CHECK(overrides == 4 && lookups == 12); // no recursive re-wrapping
}

#ifdef _WIN32
// The synthetic guest page: a base camera, the root eye layer (side 0, layers[0]) linked to its sibling (side 1,
// layers[1]), a third eye layer of the same composition (side 1, not linked), the root's and sibling's frame records,
// and the eye layers' render info.
static uint64_t region, base, layers[2], third_layer, frames[2];
static uint64_t render_rinfo[2], render_context, render_cb;
static unsigned char clean_region[0x4000];
static const PcVrHeadLookCamera base_original = {{10, 2, 4}, {10, 2, 0}, {0, 1, 0}};
static void u64(uint64_t at, uint64_t value) { memcpy((void *)(uintptr_t)at, &value, sizeof value); }
static void u16(uint64_t at, uint16_t value) { memcpy((void *)(uintptr_t)at, &value, sizeof value); }
// The game's copy of the base camera into an eye layer, moved 0.04 to one side along the base's right (+x).
static void put_eye_copy(uint64_t layer, int offset_sign) {
    PcVrHeadLookCamera eye = base_original;
    eye.position[0] += 0.04f * offset_sign; eye.target[0] += 0.04f * offset_sign;
    memcpy((void *)(uintptr_t)(layer + 0x260), &eye, sizeof eye);
}
static void game_copies_eyes(void) { put_eye_copy(layers[0], -1); put_eye_copy(layers[1], 1); }
static PcVrHeadLookCamera layer_view(uint64_t layer) {
    PcVrHeadLookCamera view;
    memcpy(&view, (void *)(uintptr_t)(layer + 0x260), sizeof view);
    return view;
}
static void eye_layer_objects(uint64_t layer, bool second_side) {
    const uint64_t object = layer + 0x288;
    u64(layer + 0x80, base); u64(layer + 0x220, object); u64(object, PROJ_VTABLE);
    u64(layer + 0x228, PROJ_EYE_VTABLE); u64(layer + 0x210, region + 0x3000);
    u16(layer + 0x90, second_side ? 0x1000 : 0);
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
static void fixture_objects(void) {
    region = MAIN_BASE + UINT64_C(0x26000000);
    CHECK(VirtualAlloc((void *)(uintptr_t)region, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) == (void *)(uintptr_t)region);
    base = region + 0x100;
    layers[0] = region + 0x1000; layers[1] = region + 0x2000; third_layer = region + 0x2800;
    frames[0] = region + 0x3800; frames[1] = region + 0x3700;
    u64(frames[0], region + 0x3900); u64(frames[0] + 8, PROJ_ROOT_RETURN);
    u64(frames[1], frames[0]); u64(frames[1] + 8, PROJ_SIBLING_RETURN);
    u64(base, PROJ_BASE_VTABLE);
    const float right[3] = {1, 0, 0};
    memcpy((void *)(uintptr_t)(base + 0x38), &base_original, sizeof base_original);
    memcpy((void *)(uintptr_t)(base + 8), right, sizeof right);
    eye_layer_objects(layers[0], false); eye_layer_objects(layers[1], true); eye_layer_objects(third_layer, true);
    u64(layers[0] + 0x208, layers[1]);
    game_copies_eyes(); put_eye_copy(third_layer, 1);
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
    memcpy(clean_region, (void *)(uintptr_t)region, sizeof clean_region);
}
static bool region_clean(void) { return !memcmp(clean_region, (void *)(uintptr_t)region, sizeof clean_region); }

static void native_pose(void) {
    mock_pose = (XrPoseSnapshot){.frame_id = 77, .game_present_id = 35, .generation = 3,
        .published_monotonic_ns = mock_now, .predicted_display_time_ns = INT64_C(9000000000),
        .orientation_xyzw = {0, 0, 0, 1}, .angular_velocity_xyz = {0.1f, 0.2f, 0.3f},
        .angular_velocity_valid = true, .active = true, .focused = true, .orientation_valid = true,
        .orientation_tracked = true, .views_valid = true, .native_views_valid = true,
        .eyes = {{{0, 0, 0, 1}, {-0.032f, 1.5f, 0.3f}, {-0.8f, 0.9f, 0.85f, -0.75f}},
                 {{0, 0, 0, 1}, {0.032f, 1.5f, 0.3f}, {-0.9f, 0.8f, 0.85f, -0.75f}}},
        .calibration = {.epoch = 3, .heading_xyzw = {0, 0, 0, 1}, .origin_xyz = {0, 1.5f, 0.3f}}};
}

// A fresh start: the game reset, the clean guest objects, the trace armed again at this world scale, the counts marked.
static void restart_at(double units_per_meter) {
    pc_vr_projection_trace_reset();
    memcpy((void *)(uintptr_t)region, clean_region, sizeof clean_region);
    fixture_init_at(units_per_meter); native_pose();
    mock_generate_projection = true;
    fail_next_parameter_write = false;
    parameter_write_calls = 0;
    guards_mark();
}
static void restart(void) { restart_at(1.0); }

static RecordEye *record_eye(unsigned side) { return &projection_record.eyes[side]; }

// Head look's eye call (pc_vr_head_look.c eye_before_original): the game's camera builder first takes the eye's
// certificate away, then the trace is given the base camera as it is now, whether the eye is its fresh copy, and the
// frame's pose (the root's call; head look gives the sibling's none).
static bool eye_call(uint64_t layer, uint64_t frame, bool fresh, const XrPoseSnapshot *pose) {
    pc_vr_render_trace_invalidate_camera(layer + 0x228);
    PcVrHeadLookCamera current;
    memcpy(&current, (void *)(uintptr_t)(base + 0x38), sizeof current);
    return pc_vr_projection_trace_eye(layer, base, &current, fresh, pose, frame);
}
static bool root_call(void) { return eye_call(layers[0], frames[0], true, &mock_pose); }
static bool sibling_call(void) { return eye_call(layers[1], frames[1], true, NULL); }
// After the game's look-at builder ran for the eye.
static void camera_built(unsigned side) { pc_vr_projection_trace_eye_built(layers[side]); }

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
static void builder_call(uint64_t layer) {
    call_writer(layer + 0x288, false, PROJ_BUILD_CALLER, layer + 0x288 + 0xc);
}
static void device_call(uint64_t layer) {
    call_writer(layer + 0x288, true, PROJ_DEVICE_CALLER, layer + 0x288 + 0x4c);
}
// The game's projection writers for the eye: the logical stage, then the device stage.
static void projection_built(unsigned side) { builder_call(layers[side]); device_call(layers[side]); }
// One whole frame as the game runs it: the game copies both eyes from the base camera, both cameras are built (the
// root's, then its sibling's), then both projections.
static void whole_frame(void) {
    game_copies_eyes();
    CHECK(root_call()); camera_built(0);
    CHECK(sibling_call()); camera_built(1);
    projection_built(0); projection_built(1);
}

// The eye's certificate: its record (frame) serial, the root's pose, its runtime eye, its world scale, the depth terms
// of its device matrix.
static void check_certificate(unsigned side, uint64_t serial, const XrPoseSnapshot *pose, unsigned native_index,
                              double units_per_meter) {
    PcVrRenderCertificate certificate;
    const uint64_t id = record_eye(side)->certificate;
    CHECK(id && pc_vr_render_trace_resolve(id, &certificate) && certificate.id == id);
    const uint64_t object = layers[side] + 0x288;
    const float *device = (const float *)(uintptr_t)(object + 0x4c);
    CHECK(certificate.traversal == serial && certificate.native_index == native_index);
    CHECK(certificate.units_per_meter == units_per_meter);
    CHECK(certificate.depth_terms[0] == device[10] && certificate.depth_terms[1] == device[11] &&
          certificate.depth_terms[2] == device[14] && certificate.depth_terms[3] == device[15]);
    CHECK(!memcmp(&certificate.native_snapshot, pose, sizeof *pose));
    // (the eye's camera as built: its view at camera +8 and its device projection; no offset asked for here)
    CHECK(!memcmp(certificate.view, (const void *)(uintptr_t)(layers[side] + 0x228 + 8), sizeof certificate.view));
    CHECK(!memcmp(certificate.device_projection, device, sizeof certificate.device_projection));
    CHECK(certificate.jitter_ndc[0] == 0 && certificate.jitter_ndc[1] == 0);
}
// The eye's projection parameters are the pose's FOV for its runtime eye, near and far kept.
static bool fov_written(uint64_t layer, const XrPoseSnapshot *pose, unsigned native_index) {
    const uint64_t object = layer + 0x288;
    float original[9], expected[9];
    memcpy(original, clean_region + (object + 0x98 - region), sizeof original);
    return pc_vr_projection_from_fov(pose->eyes[native_index].fov_left_right_up_down, original, expected) &&
        !memcmp((void *)(uintptr_t)(object + 0x98), expected, sizeof expected);
}

// Temporal anti-aliasing per eye (DLAA, DLSS): with the renderer asking for an offset (pc_vr_render_trace_jitter), each
// eye's projection is written with its record's offset (pc_vr_projection_jitter) over the sequence length asked for, the
// frustum check expects it (no refusal), and the certificate carries it with the device projection built from it;
// without a size, or with a size past 16 bits, no offset. Link's position comes from the root's call, the same for both
// eyes.
static void test_temporal_offset(void) {
    fixture_objects();
    const uint64_t player_pointer = region + 0x3a00, player = region + 0x3b00;
    const float link[3] = {-672.5f, 171.25f, 1816};
    u64(player_pointer, player);
    memcpy((void *)(uintptr_t)(player + 0x88), link, sizeof link);
    memcpy(clean_region, (void *)(uintptr_t)region, sizeof clean_region);
    const uint64_t saved_player_info = g_site.player_info;
    g_site.player_info = player_pointer;
    restart();
    whole_frame();
    CHECK(guards_counted(PC_VR_GUARD_COUNT, 0));
    for (unsigned side = 0; side < 2; ++side) {
        check_certificate(side, projection_record.serial, &mock_pose, record_eye(side)->native_index, 1.0);
        PcVrRenderCertificate certificate;
        CHECK(pc_vr_render_trace_resolve(record_eye(side)->certificate, &certificate));
        CHECK(certificate.has_link && !memcmp(certificate.link_position, link, sizeof link));
    }
    const unsigned width = 1361, height = 1469, phases = 18;  // (EyeScale 2.5 rendered 1.5 times smaller)
    pc_vr_render_trace_jitter(width, height, phases);
    for (unsigned frame = 0; frame < 19; ++frame) {
        whole_frame();
        CHECK(guards_counted(PC_VR_GUARD_COUNT, 0));
        float offset[2];
        CHECK(pc_vr_projection_jitter(projection_record.serial, phases, width, height, offset));
        for (unsigned side = 0; side < 2; ++side) {
            const unsigned native_index = record_eye(side)->native_index;
            const uint64_t object = layers[side] + 0x288;
            float original[9], expected[9], plain[16];
            memcpy(original, clean_region + (object + 0x98 - region), sizeof original);
            CHECK(pc_vr_projection_from_fov(mock_pose.eyes[native_index].fov_left_right_up_down, original, expected));
            CHECK(pc_vr_projection_logical(expected, plain));
            pc_vr_projection_offset_centre(expected, offset);
            CHECK(!memcmp((void *)(uintptr_t)(object + 0x98), expected, sizeof expected));
            PcVrRenderCertificate certificate;
            CHECK(record_eye(side)->certificate && pc_vr_render_trace_resolve(record_eye(side)->certificate, &certificate));
            CHECK(certificate.jitter_ndc[0] == offset[0] && certificate.jitter_ndc[1] == offset[1]);
            CHECK(!memcmp(certificate.device_projection, (const void *)(uintptr_t)(object + 0x4c),
                          sizeof certificate.device_projection));
            CHECK(fabsf(certificate.device_projection[2] - (plain[2] - offset[0])) < 1e-6f &&
                  fabsf(certificate.device_projection[6] - (plain[6] - offset[1])) < 1e-6f);
            CHECK(certificate.has_link && !memcmp(certificate.link_position, link, sizeof link));
            // (the pose and FOV for OpenXR as the runtime gave them)
            CHECK(!memcmp(&certificate.native_snapshot, &mock_pose, sizeof mock_pose));
        }
    }
    pc_vr_render_trace_jitter(70000, height, phases);  // (past 16 bits: no offset)
    CHECK(!atomic_load(&projection_jitter));
    pc_vr_render_trace_jitter(0, 0, 0);
    whole_frame();
    CHECK(guards_counted(PC_VR_GUARD_COUNT, 0));
    CHECK(fov_written(layers[0], &mock_pose, record_eye(0)->native_index) &&
          fov_written(layers[1], &mock_pose, record_eye(1)->native_index));
    check_certificate(0, projection_record.serial, &mock_pose, record_eye(0)->native_index, 1.0);
    // (no player: no Link)
    g_site.player_info = 0;
    whole_frame();
    PcVrRenderCertificate certificate;
    CHECK(pc_vr_render_trace_resolve(record_eye(1)->certificate, &certificate) && !certificate.has_link);
    g_site.player_info = saved_player_info;
    pc_vr_projection_trace_reset();
    mock_generate_projection = false;
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

// The projection writers, from any caller, run their originals exactly once and write nothing while no eye of a
// record has its view; while the trace is not armed an eye call does nothing at all.
static void test_writers_alone(void) {
    fixture_objects(); restart();
    mock_generate_projection = false;  // (the originals write nothing here, so any write would be the trace's)
    for (unsigned side = 0; side < 2; ++side) {
        const uint64_t object = layers[side] + 0x288;
        builder_call(layers[side]); device_call(layers[side]);
        call_writer(object, false, PROJ_BUILD_CALLER + 4, object + 0xc);
        call_writer(object, false, PROJ_BUILD_CALLER, object + 0x10);
        call_writer(object, true, PROJ_DEVICE_CALLER + 4, object + 0x4c);
        call_writer(object, true, PROJ_DEVICE_CALLER, object + 0x50);
    }
    CHECK(!parameter_write_calls && guards_counted(PC_VR_GUARD_COUNT, 0) && region_clean());
    pc_vr_projection_trace_reset();
    CHECK(!root_call() && !sibling_call() && !projection_record.serial && !parameter_write_calls);
    CHECK(guards_counted(PC_VR_GUARD_COUNT, 0) && region_clean());
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

// A frame: the root's call opens the record and writes its eye's native view; the sibling's writes the other eye from
// the same record and pose; each eye gets its certificate once its view is built and its FOV written, checked and
// converted.
static void test_frame_record(void) {
    fixture_objects(); restart();
    CHECK(pc_vr_projection_trace_root(frames[0]) && !pc_vr_projection_trace_root(frames[1]));
    const XrPoseSnapshot root_pose = mock_pose;
    CHECK(root_call());
    const uint64_t serial = projection_record.serial;
    CHECK(serial && projection_record.open && guards_counted(PC_VR_GUARD_COUNT, 0));
    // (one 36-byte write: the pair's eye for this layer's runtime eye, into the eye camera)
    CHECK(parameter_write_calls == 1 && last_parameter_write_size == sizeof(PcVrHeadLookCamera) &&
          last_parameter_write_address == layers[0] + 0x260);
    PcVrHeadLookCamera view = layer_view(layers[0]);
    CHECK(!memcmp(&view, &projection_record.pair.eyes[0], sizeof view));
    CHECK(fabsf(view.position[0] - (10 - 0.032f)) < 1e-5f && view.position[1] == 2 && view.position[2] == 4);
    CHECK(fabsf(view.target[0] - (10 - 0.032f)) < 1e-5f && fabsf(view.target[2]) < 1e-5f);
    CHECK(record_eye(0)->layer == layers[0] && record_eye(0)->serial == serial && record_eye(0)->native_index == 0);
    CHECK(record_eye(0)->view_written && !record_eye(0)->view_current && !record_eye(0)->certificate);
    CHECK(!memcmp(&projection_record.pose, &root_pose, sizeof root_pose));
    CHECK(fabs(pc_vr_camera_focus_distance() - 4) < 1e-9);  // (the base camera's target distance at scale 1)
    // A newer pose before the sibling's call changes nothing: the sibling uses the record's.
    XrPoseSnapshot newer = mock_pose;
    ++newer.frame_id; ++newer.generation; newer.eyes[1].position_xyz[0] += 0.01f;
    newer.eyes[1].fov_left_right_up_down[1] += 0.1f;
    CHECK(eye_call(layers[1], frames[1], true, &newer));
    CHECK(!projection_record.open && projection_record.serial == serial && guards_counted(PC_VR_GUARD_COUNT, 0));
    CHECK(parameter_write_calls == 2 && last_parameter_write_size == sizeof(PcVrHeadLookCamera) &&
          last_parameter_write_address == layers[1] + 0x260);
    view = layer_view(layers[1]);
    CHECK(!memcmp(&view, &projection_record.pair.eyes[1], sizeof view));
    CHECK(fabsf(view.position[0] - (10 + 0.032f)) < 1e-5f);
    CHECK(record_eye(1)->serial == serial && record_eye(1)->native_index == 1);
    CHECK(!memcmp(&record_eye(1)->pose, &root_pose, sizeof root_pose));
    // No certificate before the view is built and the projection written.
    camera_built(0); camera_built(1);
    CHECK(record_eye(0)->view_current && record_eye(1)->view_current && !record_eye(0)->certificate);
    builder_call(layers[0]);
    CHECK(record_eye(0)->fov_written && record_eye(0)->logical_current && !record_eye(0)->certificate);
    CHECK(parameter_write_calls == 3 && last_parameter_write_size == 7 * sizeof(float) &&
          last_parameter_write_address == layers[0] + 0x288 + 0xa0);
    device_call(layers[0]);
    builder_call(layers[1]); device_call(layers[1]);
    CHECK(parameter_write_calls == 4 && guards_counted(PC_VR_GUARD_COUNT, 0));
    CHECK(fov_written(layers[0], &root_pose, 0) && fov_written(layers[1], &root_pose, 1));
    check_certificate(0, serial, &root_pose, 0, 1.0);
    check_certificate(1, serial, &root_pose, 1, 1.0);
    CHECK(record_eye(0)->certificate + 1 == record_eye(1)->certificate);

    // The runtime eye is told by the side of the base's right the game's copy sits on, not by the layer's side: here
    // the root layer's copy is to the right (runtime eye 1), its sibling's to the left.
    restart();
    put_eye_copy(layers[0], 1); put_eye_copy(layers[1], -1);
    CHECK(root_call() && sibling_call());
    CHECK(record_eye(0)->native_index == 1 && record_eye(1)->native_index == 0);
    view = layer_view(layers[0]); CHECK(!memcmp(&view, &projection_record.pair.eyes[1], sizeof view));
    view = layer_view(layers[1]); CHECK(!memcmp(&view, &projection_record.pair.eyes[0], sizeof view));
    camera_built(0); camera_built(1); projection_built(0); projection_built(1);
    CHECK(fov_written(layers[0], &mock_pose, 1) && fov_written(layers[1], &mock_pose, 0));
    check_certificate(0, projection_record.serial, &mock_pose, 1, 1.0);
    check_certificate(1, projection_record.serial, &mock_pose, 0, 1.0);

    // Canted eyes (each with its own orientation) are given as OpenXR gives them.
    restart();
    for (unsigned side = 0; side < 2; ++side) {
        mock_pose.eyes[side].orientation_xyzw[1] = (float)sin(side ? -0.08 : 0.08);
        mock_pose.eyes[side].orientation_xyzw[3] = (float)cos(0.08);
    }
    whole_frame();
    CHECK(projection_record.pair.contracts.canted_from_center[0] && projection_record.pair.contracts.canted_from_center[1]);
    CHECK(record_eye(0)->certificate && record_eye(1)->certificate && guards_counted(PC_VR_GUARD_COUNT, 0));
    pc_vr_projection_trace_reset();
    mock_generate_projection = false;
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

// A refused root call: counted once by its reason, nothing written (the eye copies as they were before the call), no
// record open and no view for the eye; the sibling's call after it is not counted, and neither eye gets a certificate.
static PcVrHeadLookCamera copies_before[2];
static void copies_mark(void) { copies_before[0] = layer_view(layers[0]); copies_before[1] = layer_view(layers[1]); }
static bool copies_unchanged(void) {
    const PcVrHeadLookCamera now[2] = {layer_view(layers[0]), layer_view(layers[1])};
    return !memcmp(now, copies_before, sizeof now);
}
static void check_root_refused(PcVrGuard reason, unsigned writes) {
    CHECK(guards_counted(reason, 1) && !projection_record.open && parameter_write_calls == writes);
    CHECK(!record_eye(0)->view_written && !record_eye(0)->certificate && copies_unchanged());
    guards_mark();
    CHECK(!sibling_call() && guards_counted(PC_VR_GUARD_COUNT, 0) && !record_eye(1)->view_written);
    CHECK(copies_unchanged() && parameter_write_calls == writes);
    camera_built(0); camera_built(1); projection_built(0); projection_built(1);
    CHECK(!record_eye(0)->certificate && !record_eye(1)->certificate);
    CHECK(guards_counted(PC_VR_GUARD_COUNT, 0) && parameter_write_calls == writes);
}

// Each refusal of the root's call, one cause at a time.
static void test_root_refusals(void) {
    fixture_objects();
#define ROOT_REFUSED(change, call, reason, writes) do { \
    restart(); change; copies_mark(); CHECK(!(call)); check_root_refused((reason), (writes)); \
} while (0)
    // (the eye is not the game's fresh copy of the base camera)
    ROOT_REFUSED((void)0, eye_call(layers[0], frames[0], false, &mock_pose), PC_VR_GUARD_VIEW_NOT_FRESH, 0);
    // (no pose: head look has none with native views; it checks the flags, D116: the values are not checked)
    ROOT_REFUSED((void)0, eye_call(layers[0], frames[0], true, NULL), PC_VR_GUARD_VIEW_NO_POSE, 0);
    // (the pair cannot be built: both eyes at one place, no baseline)
    ROOT_REFUSED(memcpy(mock_pose.eyes[1].position_xyz, mock_pose.eyes[0].position_xyz, sizeof mock_pose.eyes[1].position_xyz),
                 root_call(), PC_VR_GUARD_VIEW_PAIR, 0);
    // (the root's layer, configuration or sibling not valid objects, or the layer not of this base camera)
    ROOT_REFUSED(u64(layers[0] + 0x210, 0), root_call(), PC_VR_GUARD_TRAVERSAL, 0);
    ROOT_REFUSED(u64(layers[0] + 0x210, region + 0x3004), root_call(), PC_VR_GUARD_TRAVERSAL, 0);
    ROOT_REFUSED(u64(layers[0] + 0x208, 0), root_call(), PC_VR_GUARD_TRAVERSAL, 0);
    ROOT_REFUSED(u64(layers[0] + 0x208, layers[0]), root_call(), PC_VR_GUARD_TRAVERSAL, 0);
    ROOT_REFUSED(u64(layers[0] + 0x208, layers[1] + 4), root_call(), PC_VR_GUARD_TRAVERSAL, 0);
    ROOT_REFUSED(u64(layers[0] + 0x288, PROJ_VTABLE + 8), root_call(), PC_VR_GUARD_TRAVERSAL, 0);
    ROOT_REFUSED(u64(layers[0] + 0x220, layers[0] + 0x290), root_call(), PC_VR_GUARD_TRAVERSAL, 0);
    ROOT_REFUSED(u64(layers[0] + 0x228, PROJ_EYE_VTABLE + 8), root_call(), PC_VR_GUARD_TRAVERSAL, 0);
    ROOT_REFUSED(u64(layers[0] + 0x80, base + 8), root_call(), PC_VR_GUARD_TRAVERSAL, 0);
    ROOT_REFUSED(u64(base, PROJ_BASE_VTABLE + 8), root_call(), PC_VR_GUARD_TRAVERSAL, 0);
    // (the game's eye copy not the base camera moved sideways along its right: off the right axis, not moved, or the
    // base's stored right row not its camera's; that it is a copy at all is head look's fresh test)
    ROOT_REFUSED((((float *)(uintptr_t)(layers[0] + 0x260))[1] += 0.1f, ((float *)(uintptr_t)(layers[0] + 0x260))[4] += 0.1f),
                 root_call(), PC_VR_GUARD_VIEW_PLACEMENT, 0);
    ROOT_REFUSED(put_eye_copy(layers[0], 0), root_call(), PC_VR_GUARD_VIEW_PLACEMENT, 0);
    ROOT_REFUSED(*(float *)(uintptr_t)(base + 8) = -1, root_call(), PC_VR_GUARD_VIEW_PLACEMENT, 0);
    // (the camera write refused: one attempt, nothing copied, not tried again)
    ROOT_REFUSED(fail_next_parameter_write = true, root_call(), PC_VR_GUARD_WRITE_FAILED, 1);
    // (a frame record that cannot be read, or that is neither eye's call: no root)
    const uint64_t invalid_frames[] = {0, GUEST_HI - 8, frames[0] + 8, region + 0x4000};
    for (unsigned i = 0; i < sizeof invalid_frames / sizeof invalid_frames[0]; ++i) {
        CHECK(!pc_vr_projection_trace_root(invalid_frames[i]));
        ROOT_REFUSED((void)0, eye_call(layers[0], invalid_frames[i], true, &mock_pose),
                     PC_VR_GUARD_TRAVERSAL, 0);
    }
    const uint64_t invalid_parents[] = {0, frames[0], frames[0] - 16, frames[0] + 8};
    for (unsigned i = 0; i < sizeof invalid_parents / sizeof invalid_parents[0]; ++i) {
        ROOT_REFUSED(u64(frames[0], invalid_parents[i]), root_call(), PC_VR_GUARD_TRAVERSAL, 0);
        CHECK(!pc_vr_projection_trace_root(frames[0]));
    }
    ROOT_REFUSED(u64(frames[0] + 8, PROJ_ROOT_RETURN + 4), root_call(), PC_VR_GUARD_TRAVERSAL, 0);
#undef ROOT_REFUSED
    // A call whose frame record cannot be read also closes a record left open.
    restart(); CHECK(root_call() && projection_record.open);
    CHECK(!eye_call(layers[1], frames[0] + 8, true, NULL) && !projection_record.open);
    CHECK(guards_counted(PC_VR_GUARD_TRAVERSAL, 1));
    pc_vr_projection_trace_reset();
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

static void *other_thread_sibling(void *unused) {
    (void)unused;
    return (void *)(uintptr_t)sibling_call();
}
static bool sibling_on_other_thread(void) {
    pthread_t thread;
    void *given = (void *)1;
    CHECK(!pthread_create(&thread, NULL, other_thread_sibling, NULL) && !pthread_join(thread, &given));
    return given != NULL;
}

// Each refusal of the sibling's call, one cause at a time: counted once, the record closed, the sibling's eye not
// written and without a certificate, while the root's eye keeps its own; a second sibling call is not counted.
static void test_sibling_refusals(void) {
    fixture_objects();
#define SIBLING_REFUSED(change, call, reason, writes) do { \
    restart(); CHECK(root_call()); camera_built(0); change; guards_mark(); copies_mark(); \
    CHECK(!(call)); \
    CHECK(guards_counted((reason), 1) && !projection_record.open && parameter_write_calls == (writes)); \
    CHECK(!record_eye(1)->view_written && copies_unchanged()); \
    projection_built(0); projection_built(1); \
    CHECK(record_eye(0)->certificate && !record_eye(1)->certificate); \
    guards_mark(); CHECK(!sibling_call() && guards_counted(PC_VR_GUARD_COUNT, 0)); \
} while (0)
    // (not the root's sibling layer, its configuration changed, the base camera moved since the root, the root's link
    // to it gone, the root's call no longer on the stack, or a sibling call saving another frame than the root's)
    SIBLING_REFUSED((void)0, eye_call(third_layer, frames[1], true, NULL), PC_VR_GUARD_TRAVERSAL, 1);
    SIBLING_REFUSED(u64(layers[1] + 0x210, region + 0x3100), sibling_call(), PC_VR_GUARD_TRAVERSAL, 1);
    SIBLING_REFUSED(*(float *)(uintptr_t)(base + 0x38 + 12) += 0.5f, sibling_call(), PC_VR_GUARD_TRAVERSAL, 1);
    SIBLING_REFUSED(u64(layers[0] + 0x208, third_layer), sibling_call(), PC_VR_GUARD_TRAVERSAL, 1);
    SIBLING_REFUSED(u64(frames[0] + 8, PROJ_ROOT_RETURN + 4), sibling_call(), PC_VR_GUARD_TRAVERSAL, 1);
    SIBLING_REFUSED(u64(frames[1], frames[0] + 16), sibling_call(), PC_VR_GUARD_TRAVERSAL, 1);
    // (another thread than the root's)
    SIBLING_REFUSED((void)0, sibling_on_other_thread(), PC_VR_GUARD_TRAVERSAL, 1);
    // A linked sibling whose eye is not the game's fresh copy: view-not-fresh.
    SIBLING_REFUSED((void)0, eye_call(layers[1], frames[1], false, NULL), PC_VR_GUARD_VIEW_NOT_FRESH, 1);
    // Its copy on the root's side of the base's right: placement.
    SIBLING_REFUSED(put_eye_copy(layers[1], -1), sibling_call(), PC_VR_GUARD_VIEW_PLACEMENT, 1);
    // Its camera write refused (one attempt).
    SIBLING_REFUSED(fail_next_parameter_write = true, sibling_call(), PC_VR_GUARD_WRITE_FAILED, 2);
#undef SIBLING_REFUSED
    // A sibling layer that is not an eye layer (its camera not of the eye camera class): not the sibling, and the
    // layer is no eye layer of the record.
    restart(); CHECK(root_call()); u64(layers[1] + 0x228, PROJ_EYE_VTABLE + 8); guards_mark(); copies_mark();
    CHECK(!sibling_call() && guards_counted(PC_VR_GUARD_TRAVERSAL, 1) && !projection_record.open);
    CHECK(copies_unchanged() && parameter_write_calls == 1 && !record_eye_layer(layers[1]));
    // The same side as the root's: not the sibling, and the root's eye, written this frame, is left as it was.
    restart(); CHECK(root_call()); u16(layers[1] + 0x90, 0); guards_mark();
    CHECK(!sibling_call() && guards_counted(PC_VR_GUARD_TRAVERSAL, 1) && !projection_record.open);
    CHECK(record_eye(0)->layer == layers[0] && record_eye(0)->view_written && !record_eye(1)->layer);
    // A sibling layer whose objects cannot be read (its side unknown) leaves both eyes as they were.
    restart(); CHECK(root_call()); u64(layers[1] + 0x228, PROJ_EYE_VTABLE + 8);
    CHECK(!sibling_call() && record_eye(0)->layer == layers[0] && record_eye(0)->view_written);
    // A sibling call with no record open (none yet, or after the sibling took it) is not counted and writes nothing;
    // a duplicate after the sibling took the record leaves the eye this frame wrote as it was.
    restart(); CHECK(!sibling_call() && guards_counted(PC_VR_GUARD_COUNT, 0) && !parameter_write_calls);
    CHECK(record_eye(1)->layer == layers[1] && !record_eye(1)->view_written);
    restart(); CHECK(root_call() && sibling_call() && parameter_write_calls == 2);
    CHECK(!eye_call(layers[1], frames[1], true, NULL) && guards_counted(PC_VR_GUARD_COUNT, 0));
    CHECK(parameter_write_calls == 2 && record_eye(1)->view_written && record_eye(0)->view_written);
    pc_vr_projection_trace_reset();
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

// A record's serial is never reused: each root call takes a new one (refused or not), a new root abandons an open
// record, and arming again or the game's reset never starts the serials over.
static void test_record_serials(void) {
    fixture_objects(); restart();
    CHECK(root_call());
    const uint64_t first = projection_record.serial;
    CHECK(first && projection_record.open);
    game_copies_eyes();
    CHECK(root_call() && projection_record.open);  // (the same stack and objects: a new frame)
    const uint64_t second = projection_record.serial;
    CHECK(second > first && record_eye(0)->serial == second);
    CHECK(sibling_call() && record_eye(1)->serial == second && guards_counted(PC_VR_GUARD_COUNT, 0));
    game_copies_eyes();
    CHECK(!eye_call(layers[0], frames[0], true, NULL) && projection_record.serial > second);  // (refused: still one)
    const uint64_t refused = projection_record.serial;
    fixture_init();
    CHECK(!projection_record.serial && !record_eye(0)->layer && !record_eye(1)->layer);
    game_copies_eyes(); CHECK(root_call() && projection_record.serial > refused);
    const uint64_t rearmed = projection_record.serial;
    restart();
    CHECK(!projection_record.serial && root_call() && projection_record.serial > rearmed);
    pc_vr_projection_trace_reset();
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

static void *other_thread_camera_built(void *unused) {
    (void)unused;
    camera_built(0);
    return NULL;
}
static void *other_thread_builder(void *unused) {
    (void)unused;
    builder_call(layers[0]);
    return NULL;
}

// The eye's view is current once the game's look-at builder ran for it: only for an eye written this frame, and only
// on its camera thread.
static void test_eye_built(void) {
    fixture_objects(); restart();
    CHECK(root_call());
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, other_thread_camera_built, NULL) && !pthread_join(thread, NULL));
    CHECK(record_eye(0)->view_written && !record_eye(0)->view_current);
    pc_vr_projection_trace_eye_built(third_layer); CHECK(!record_eye(0)->view_current);
    camera_built(0); CHECK(record_eye(0)->view_current);
    CHECK(!eye_call(layers[1], frames[1], false, NULL));  // (the sibling refused: not written)
    camera_built(1); CHECK(record_eye(1)->layer == layers[1] && !record_eye(1)->view_current);
    pc_vr_projection_trace_reset();
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

// The native FOV: written once per eye per frame, only for an eye whose view is current, on its camera thread; then
// the one post-condition (the game's logical matrix gives the eye's frustum) and the device stage, after which the
// certificate is published.
static void test_native_fov(void) {
    fixture_objects();
    // Once per eye per frame: the projection built again writes nothing more, and its certificate is taken away (the
    // logical stage no longer checked), not given back by the device stage alone.
    restart(); whole_frame();
    CHECK(parameter_write_calls == 4 && record_eye(0)->certificate && record_eye(1)->certificate);
    builder_call(layers[0]);
    CHECK(parameter_write_calls == 4 && !record_eye(0)->certificate && !record_eye(0)->logical_current);
    device_call(layers[0]);
    CHECK(!record_eye(0)->certificate && record_eye(1)->certificate && guards_counted(PC_VR_GUARD_COUNT, 0));
    // A writer called from another caller, or with another output, is not the eye's: no write, nothing counted, and the
    // exact call after it still writes.
    restart();
    CHECK(root_call()); camera_built(0);
    call_writer(layers[0] + 0x288, false, PROJ_BUILD_CALLER + 4, layers[0] + 0x288 + 0xc);
    call_writer(layers[0] + 0x288, false, PROJ_BUILD_CALLER, layers[0] + 0x288 + 0x10);
    CHECK(parameter_write_calls == 1 && !record_eye(0)->fov_attempted && guards_counted(PC_VR_GUARD_COUNT, 0));
    projection_built(0);
    CHECK(parameter_write_calls == 2 && record_eye(0)->certificate);
    // The view not built yet: fov-precondition, once (no second attempt this frame), no certificate.
    restart();
    CHECK(root_call()); projection_built(0);
    CHECK(guards_counted(PC_VR_GUARD_FOV_PRECONDITION, 1) && parameter_write_calls == 1);
    camera_built(0); projection_built(0);
    CHECK(guards_counted(PC_VR_GUARD_FOV_PRECONDITION, 1) && parameter_write_calls == 1 && !record_eye(0)->certificate);
    // The eye's camera built again since (its view no longer current): fov-precondition.
    restart();
    CHECK(root_call()); camera_built(0); pc_vr_render_trace_invalidate_camera(layers[0] + 0x228);
    projection_built(0);
    CHECK(guards_counted(PC_VR_GUARD_FOV_PRECONDITION, 1) && !record_eye(0)->certificate);
    // On another thread than the eye's camera thread: fov-precondition.
    restart();
    CHECK(root_call()); camera_built(0);
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, other_thread_builder, NULL) && !pthread_join(thread, NULL));
    device_call(layers[0]);
    CHECK(guards_counted(PC_VR_GUARD_FOV_PRECONDITION, 1) && parameter_write_calls == 1 && !record_eye(0)->certificate);
    // A FOV the game's projection cannot hold (reversed): fov-precondition.
    restart();
    mock_pose.eyes[0].fov_left_right_up_down[0] = 0.9f; mock_pose.eyes[0].fov_left_right_up_down[1] = -0.8f;
    CHECK(root_call()); camera_built(0); projection_built(0);
    CHECK(guards_counted(PC_VR_GUARD_FOV_PRECONDITION, 1) && parameter_write_calls == 1 && !record_eye(0)->certificate);
    // An eye whose view was refused: its projection is left as the game makes it, and nothing is counted.
    restart();
    CHECK(root_call()); camera_built(0); CHECK(!eye_call(layers[1], frames[1], false, NULL));
    guards_mark(); camera_built(1); projection_built(1);
    CHECK(guards_counted(PC_VR_GUARD_COUNT, 0) && parameter_write_calls == 1 && !record_eye(1)->certificate);
    // The parameter write refused: write-failed (one attempt), no certificate.
    restart();
    CHECK(root_call()); camera_built(0);
    fail_next_parameter_write = true; projection_built(0);
    CHECK(guards_counted(PC_VR_GUARD_WRITE_FAILED, 1) && parameter_write_calls == 2);
    CHECK(!record_eye(0)->fov_written && !record_eye(0)->certificate);
    // The device stage alone (no logical stage this frame) publishes nothing.
    restart();
    CHECK(root_call()); camera_built(0);
    const unsigned published = atomic_load(&render_certificate_count);
    device_call(layers[0]);
    CHECK(!record_eye(0)->certificate && atomic_load(&render_certificate_count) == published);
    CHECK(guards_counted(PC_VR_GUARD_COUNT, 0) && parameter_write_calls == 1);
    pc_vr_projection_trace_reset();
    mock_generate_projection = false;
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
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
// The certificate a draw of this eye's layer recorded now gets.
static uint64_t draw_certificate(unsigned side) {
    begin_scope(side);
    const uint64_t id = pc_vr_render_trace_record(render_cb);
    end_scope(side);
    return id;
}

// A draw recorded after the next frame's root call: the other eye's entry is still this frame's, so its draw keeps its
// certificate; the root's eye has none until its new view and FOV are given.
static void test_draws_across_frames(void) {
    fixture_objects(); restart(); whole_frame();
    const uint64_t first_left = record_eye(0)->certificate, first_right = record_eye(1)->certificate;
    const uint64_t first_serial = projection_record.serial;
    CHECK(draw_certificate(0) == first_left && draw_certificate(1) == first_right);
    game_copies_eyes(); ++mock_pose.frame_id;
    CHECK(root_call());
    const uint64_t next_serial = projection_record.serial;
    CHECK(next_serial > first_serial);
    CHECK(draw_certificate(1) == first_right && !draw_certificate(0));
    camera_built(0); CHECK(!draw_certificate(0));
    projection_built(0);
    const uint64_t next_left = record_eye(0)->certificate;
    CHECK(next_left > first_right && draw_certificate(0) == next_left && draw_certificate(1) == first_right);
    PcVrRenderCertificate certificate;
    CHECK(pc_vr_render_trace_resolve(next_left, &certificate) && certificate.traversal == next_serial &&
          certificate.native_snapshot.frame_id == mock_pose.frame_id);
    CHECK(pc_vr_render_trace_resolve(first_right, &certificate) && certificate.traversal == first_serial &&
          certificate.native_snapshot.frame_id == mock_pose.frame_id - 1);
    CHECK(sibling_call() && !draw_certificate(1));  // (the sibling's call replaces its eye)
    camera_built(1); projection_built(1);
    CHECK(draw_certificate(1) == next_left + 1 && draw_certificate(0) == next_left);
    pc_vr_projection_trace_reset();
    mock_generate_projection = false;
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

// Two frames' draws can be outstanding at once, and a projection built again issues its eye a new id. The older frame's
// draws still resolve after both eyes of the newer frame were certified a second time: six ids, none expired.
static void test_certificates_outstanding(void) {
    fixture_objects(); restart(); whole_frame();
    const uint64_t drawing[2] = {record_eye(0)->certificate, record_eye(1)->certificate};
    const uint64_t drawing_pose = mock_pose.frame_id;
    ++mock_pose.frame_id;
    whole_frame();
    const uint64_t recording[2] = {record_eye(0)->certificate, record_eye(1)->certificate};
    for (unsigned side = 0; side < 2; ++side) device_call(layers[side]);
    guards_mark();
    for (unsigned side = 0; side < 2; ++side) {
        const uint64_t rebuilt = record_eye(side)->certificate;
        PcVrRenderCertificate certificate;
        CHECK(drawing[side] && recording[side] > drawing[1] && rebuilt > recording[1]);
        CHECK(pc_vr_render_trace_resolve(drawing[side], &certificate) && certificate.id == drawing[side] &&
              certificate.native_snapshot.frame_id == drawing_pose);
        const unsigned native_index = certificate.native_index;
        CHECK(pc_vr_render_trace_resolve(recording[side], &certificate) && certificate.id == recording[side] &&
              certificate.native_index == native_index && certificate.native_snapshot.frame_id == mock_pose.frame_id);
        CHECK(pc_vr_render_trace_resolve(rebuilt, &certificate) && certificate.id == rebuilt &&
              certificate.native_index == native_index && certificate.native_snapshot.frame_id == mock_pose.frame_id);
    }
    CHECK(guards_counted(PC_VR_GUARD_COUNT, 0));
    pc_vr_projection_trace_reset();
    mock_generate_projection = false;
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

// Keep both eyes of each of the last two pairs, even while the newer pair is partly built.
// An exact view shared by pairs with competing midpoints is ambiguous.
static void test_projection_view_midpoints(void) {
    fixture_objects(); restart();
    float left[12] = {1, 0, 0, -9.97f, 0, 1, 0, -2, 0, 0, 1, -4};
    float right[12] = {1, 0, 0, -10.03f, 0, 1, 0, -2, 0, 0, 1, -4};
    float midpoint[3] = {0};
    CHECK(root_call());
    memcpy((void *)(uintptr_t)(layers[0] + 0x230), left, sizeof left);
    CHECK(!pc_vr_projection_view_midpoint(left, midpoint));
    camera_built(0);
    CHECK(sibling_call());
    memcpy((void *)(uintptr_t)(layers[1] + 0x230), right, sizeof right);
    camera_built(1);
    unsigned acquired = 0;
    projection_lock_counter = &acquired;
    CHECK(pc_vr_projection_view_midpoint(left, midpoint) && !memcmp(midpoint, base_original.position, sizeof midpoint));
    CHECK(pc_vr_projection_view_midpoint(right, midpoint) && !memcmp(midpoint, base_original.position, sizeof midpoint));
    const unsigned first_pair = acquired;
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        CHECK(pc_vr_projection_view_midpoint(left, midpoint));
        CHECK(pc_vr_projection_view_midpoint(right, midpoint));
    }
    CHECK(acquired == first_pair);
    projection_lock_counter = NULL;

    game_copies_eyes(); ++mock_pose.frame_id;
    mock_pose.eyes[0].position_xyz[1] += 0.25f;
    mock_pose.eyes[1].position_xyz[1] += 0.25f;
    CHECK(root_call());
    float first_left[12];
    memcpy(first_left, left, sizeof left);
    left[7] -= 0.25f;
    memcpy((void *)(uintptr_t)(layers[0] + 0x230), left, sizeof left);
    camera_built(0);
    const float moved_midpoint[3] = {10, 2.25f, 4};
    CHECK(pc_vr_projection_view_midpoint(left, midpoint) && !memcmp(midpoint, moved_midpoint, sizeof midpoint));
    CHECK(pc_vr_projection_view_midpoint(first_left, midpoint) && !memcmp(midpoint, base_original.position, sizeof midpoint));
    CHECK(pc_vr_projection_view_midpoint(right, midpoint) && !memcmp(midpoint, base_original.position, sizeof midpoint));
    const float moved_y = left[7];
    left[7] = nextafterf(moved_y, 0.0f);
    CHECK(!pc_vr_projection_view_midpoint(left, midpoint));
    left[7] = moved_y;
    camera_built(1); // The older right eye can finish building again before the new sibling arrives.
    CHECK(pc_vr_projection_view_midpoint(left, midpoint) && !memcmp(midpoint, moved_midpoint, sizeof midpoint));
    CHECK(sibling_call());
    camera_built(1); // The new pair's right matrix deliberately duplicates the old right.
    CHECK(!pc_vr_projection_view_midpoint(right, midpoint) && !memcmp(midpoint, moved_midpoint, sizeof midpoint));
    acquired = 0;
    projection_lock_counter = &acquired;
    CHECK(!pc_vr_projection_view_midpoint(right, midpoint) && !memcmp(midpoint, moved_midpoint, sizeof midpoint));
    CHECK(acquired == 0);
    projection_lock_counter = NULL;
    for (unsigned i = 0; i < 5; ++i) camera_built(1);
    CHECK(pc_vr_projection_view_midpoint(first_left, midpoint) && !memcmp(midpoint, base_original.position, sizeof midpoint));
    CHECK(!pc_vr_projection_view_midpoint(right, midpoint)); // Rebuilding one eye cannot expire the older pair.

    game_copies_eyes(); ++mock_pose.frame_id;
    mock_pose.eyes[0].position_xyz[1] += 0.25f;
    mock_pose.eyes[1].position_xyz[1] += 0.25f;
    CHECK(root_call());
    float third_left[12];
    memcpy(third_left, left, sizeof left);
    third_left[7] -= 0.25f;
    memcpy((void *)(uintptr_t)(layers[0] + 0x230), third_left, sizeof third_left);
    camera_built(0);
    const float third_midpoint[3] = {10, 2.5f, 4};
    CHECK(pc_vr_projection_view_midpoint(third_left, midpoint) && !memcmp(midpoint, third_midpoint, sizeof midpoint));
    CHECK(!pc_vr_projection_view_midpoint(first_left, midpoint));
    CHECK(pc_vr_projection_view_midpoint(left, midpoint) && !memcmp(midpoint, moved_midpoint, sizeof midpoint));
    CHECK(pc_vr_projection_view_midpoint(right, midpoint) && !memcmp(midpoint, moved_midpoint, sizeof midpoint));
    pc_vr_projection_trace_reset();
    CHECK(!pc_vr_projection_view_midpoint(right, midpoint));
    CHECK(!pc_vr_projection_view_midpoint(third_left, midpoint));
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

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
static unsigned certificate_reader_first;  // the recorded ID the writer will outlive
static void *certificate_reader(void *unused) {
    (void)unused;
    atomic_store(&certificate_reader_ready, true);
    unsigned iterations = 0;
    bool checked_expiry = false;
    while (!atomic_load(&certificate_reader_stop) || iterations < 10000) {
        const unsigned count = atomic_load_explicit(&render_certificate_count, memory_order_acquire);
        // The writer waits for this reader between frames, so at most two newer ids can be issued mid-read.
        for (unsigned id = count - 1; id <= count; ++id) {
            PcVrRenderCertificate cert;
            if (!pc_vr_render_trace_resolve(id, &cert) || cert.id != id || !cert.traversal ||
                cert.native_index > 1 || cert.units_per_meter != 1.0) atomic_store(&certificate_reader_failed, true);
        }
        if (!checked_expiry && count - certificate_reader_first >= RENDER_CERTIFICATE_SLOTS) {
            PcVrRenderCertificate untouched = {.id = UINT64_MAX}, expired = untouched;
            if (pc_vr_render_trace_resolve(certificate_reader_first, &expired) ||
                memcmp(&expired, &untouched, sizeof expired)) atomic_store(&certificate_reader_failed, true);
            checked_expiry = true;
        }
        atomic_store(&certificate_reader_observed, count);
        ++iterations;
    }
    return atomic_load(&certificate_reader_failed) ? (void *)1 : NULL;
}
static void set_eye_certificate(unsigned side, uint64_t id) {
    pthread_mutex_lock(&projection_lock);
    record_eye(side)->view_current = true; record_eye(side)->certificate = id;
    record_eyes_publish_locked();
    pthread_mutex_unlock(&projection_lock);
}
// A worker whose layer's render info changes inside its scope: a draw recorded before the scope's end keeps the scope
// as it began; the end finds the change, and no later draw on the thread gets a certificate. Returns the certificate
// both draws in the changed scope got, or 0 if they differed or a later draw got one.
static void *changed_scope_thread(void *unused) {
    (void)unused;
    begin_scope(0);
    const uint64_t before = pc_vr_render_trace_record(render_cb);
    u64(render_rinfo[0] + 0x28, base);
    const uint64_t changed = pc_vr_render_trace_record(render_cb);
    end_scope(0);
    u64(render_rinfo[0] + 0x28, layers[0] + 0x228);
    const uint64_t later = draw_certificate(0);
    return (void *)(uintptr_t)(before == changed && !later ? before : 0);
}
// Render scopes and certificates: a draw recorded in an eye layer's render scope, through its own camera and
// projection, into the scope's command buffer gets the eye's certificate; the store is read concurrently and recycles
// its slots; layer draws carry their kind and whether the layer is an eye's.
static void test_render_trace(void) {
    PcVrRenderCertificate unchanged, out;
    memset(&unchanged, 0xa5, sizeof unchanged); out = unchanged;
    pc_vr_projection_trace_reset();
    CHECK(!pc_vr_render_trace_record(1));
    CHECK(!pc_vr_render_trace_resolve(0, &out) && !memcmp(&out, &unchanged, sizeof out));
    CHECK(!pc_vr_render_trace_resolve(1, NULL));

    fixture_objects(); restart();
    CHECK(pc_vr_render_trace_enabled() && projection_registered && overrides == 4);
    // (the earlier tests published certificates of their own: this test's are counted from here)
    const unsigned published_before = atomic_load(&render_certificate_count);
    game_copies_eyes(); CHECK(root_call()); camera_built(0); CHECK(sibling_call()); camera_built(1);
    begin_scope(0);
    CHECK(!pc_vr_render_trace_record(render_cb)); // (the projection not built yet)
    projection_built(0);
    const uint64_t first = pc_vr_render_trace_record(render_cb);
    CHECK(first == published_before + 1 && pc_vr_render_trace_resolve(first, &out));
    CHECK(out.native_snapshot.frame_id == mock_pose.frame_id && out.traversal == projection_record.serial && out.native_index == 0);
    const PcVrRenderCertificate first_copy = out;
    projection_built(1);
    CHECK(atomic_load(&render_certificate_count) == published_before + 2);
    CHECK(pc_vr_render_trace_record(render_cb) == first);

    // The same eye's draw recorded on a worker gets its certificate (construction stays on the camera thread).
    pthread_t foreign;
    void *foreign_result = (void *)1;
    const int created = pthread_create(&foreign, NULL, foreign_recording_thread, NULL);
    const int joined = created ? -1 : pthread_join(foreign, &foreign_result);
    CHECK(!created && !joined && (uint64_t)(uintptr_t)foreign_result == first);
    // The worker opens its scope, then waits while the eye's camera is built again (which takes the eye's certificate
    // away): it gets no certificate.
    atomic_store(&hold_record_query, true);
    const int stale_created = pthread_create(&foreign, NULL, foreign_recording_thread, NULL);
    CHECK(!stale_created);
    while (!atomic_load(&record_query_entered)) Sleep(0);
    pc_vr_render_trace_invalidate_camera(layers[0] + 0x228);
    atomic_store(&release_record_query, true);
    const int stale_joined = pthread_join(foreign, &foreign_result);
    CHECK(!stale_joined && !foreign_result);
    atomic_store(&hold_record_query, false);
    set_eye_certificate(0, first);  // (the eye put back as it was)
    // Each condition of the lookup, one at a time.
    atomic_store(&projection_enabled, false);
    CHECK(!pc_vr_render_trace_record(render_cb));
    atomic_store(&projection_enabled, true);
    set_eye_certificate(0, 0);
    CHECK(!pc_vr_render_trace_record(render_cb));
    set_eye_certificate(0, first);
    CHECK(pc_vr_render_trace_record(render_cb) == first);
    CHECK(!pc_vr_render_trace_record(render_cb + 8));  // (another command buffer)
    const int changed_created = pthread_create(&foreign, NULL, changed_scope_thread, NULL);
    const int changed_joined = changed_created ? -1 : pthread_join(foreign, &foreign_result);
    CHECK(!changed_created && !changed_joined && (uint64_t)(uintptr_t)foreign_result == first);
    CHECK(pc_vr_render_trace_record(render_cb) == first);  // (this thread's scope untouched)

    // A layer's finishing draws carry their kind, and whether the layer is an eye's, while their call runs.
    CHECK(!pc_vr_layer_draw_recording());
    const uint32_t outer_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_FINAL);
    CHECK(!outer_draw && pc_vr_layer_draw_recording() == (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE));
    const uint32_t inner_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_PLACEMENT);
    CHECK(inner_draw == (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE) &&
        pc_vr_layer_draw_recording() == (PC_VR_LAYER_DRAW_PLACEMENT | PC_VR_LAYER_DRAW_EYE));
    rt_layer_draw_end(inner_draw); rt_layer_draw_end(outer_draw);
    CHECK(!pc_vr_layer_draw_recording());
    // The effect system's draws add their flag to what is set, and end gives back what was set before.
    const uint32_t effect_layer_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_FINAL);
    const uint32_t effect_draw = rt_effect_draw_begin();
    CHECK(effect_draw == (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE) &&
        pc_vr_layer_draw_recording() == (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE | PC_VR_LAYER_DRAW_EFFECT));
    rt_layer_draw_end(effect_draw);
    CHECK(pc_vr_layer_draw_recording() == (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE));
    rt_layer_draw_end(effect_layer_draw);
    const uint32_t lone_effect_draw = rt_effect_draw_begin();  // (in a layer's draw steps, outside its finishing draws)
    CHECK(!lone_effect_draw && pc_vr_layer_draw_recording() == PC_VR_LAYER_DRAW_EFFECT);
    rt_layer_draw_end(lone_effect_draw);
    CHECK(!pc_vr_layer_draw_recording());
    // The eye's layer without a certificate (its view not given this frame) is still the eye's layer.
    set_eye_certificate(0, 0);
    CHECK(!pc_vr_render_trace_record(render_cb));
    const uint32_t uncertified_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_FINAL);
    CHECK(pc_vr_layer_draw_recording() == (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE));
    rt_layer_draw_end(uncertified_draw); set_eye_certificate(0, first);
    // The eye's layer drawn with the game's own camera (its view not current this frame): unbound.
    pthread_mutex_lock(&projection_lock); record_eye(0)->view_current = false; pthread_mutex_unlock(&projection_lock);
    const uint32_t unbound_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_FINAL);
    CHECK(pc_vr_layer_draw_recording() == (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE | PC_VR_LAYER_DRAW_UNBOUND));
    rt_layer_draw_end(unbound_draw);
    pthread_mutex_lock(&projection_lock); record_eye(0)->view_current = true; pthread_mutex_unlock(&projection_lock);
    // Before any eye layer is known (after a load), a layer may be an eye's.
    RecordEye known_eyes[2];
    pthread_mutex_lock(&projection_lock);
    memcpy(known_eyes, projection_record.eyes, sizeof known_eyes);
    memset(projection_record.eyes, 0, sizeof projection_record.eyes);
    pthread_mutex_unlock(&projection_lock);
    const uint32_t unknown_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_FINAL);
    CHECK(pc_vr_layer_draw_recording() ==
          (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_MAYBE_EYE | PC_VR_LAYER_DRAW_UNBOUND));
    rt_layer_draw_end(unknown_draw);
    pthread_mutex_lock(&projection_lock);
    memcpy(projection_record.eyes, known_eyes, sizeof known_eyes);
    pthread_mutex_unlock(&projection_lock);

    // An unrelated nested selection suppresses the valid outer selection.
    u64(render_rinfo[1] + 0x28, base); begin_scope(1);
    const uint32_t other_layer_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_FINAL);
    CHECK(pc_vr_layer_draw_recording() == PC_VR_LAYER_DRAW_FINAL);  // (another layer's: no eye flag)
    rt_layer_draw_end(other_layer_draw);
    CHECK(!pc_vr_render_trace_record(render_cb)); end_scope(1);
    CHECK(pc_vr_render_trace_record(render_cb) == first);
    u64(render_rinfo[1] + 0x28, layers[1] + 0x228);
    scope_call(1, false, MAIN_BASE + 4); // Unqualified nested callback also suppresses.
    CHECK(!pc_vr_render_trace_record(render_cb)); end_scope(1);
    CHECK(pc_vr_render_trace_record(render_cb) == first);

    begin_scope(1);
    const uint64_t second = pc_vr_render_trace_record(render_cb);
    CHECK(second == first + 1); end_scope(1);
    CHECK(pc_vr_render_trace_record(render_cb) == first); end_scope(0);
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

    // The eye's camera built takes its certificate away; the certificate already published stays as it was.
    begin_scope(0); pc_vr_render_trace_invalidate_camera(layers[0] + 0x228);
    CHECK(!pc_vr_render_trace_record(render_cb)); end_scope(0);
    CHECK(pc_vr_render_trace_resolve(first, &out) && !memcmp(&out, &first_copy, sizeof out));
    // The device stage on another thread takes the certificate away before its original runs, so a draw recorded
    // meanwhile gets none; once it has run from the same checked logical matrix, the eye has a new certificate.
    begin_scope(1);
    atomic_store(&hold_original, true);
    pthread_t writer;
    const int writer_created = pthread_create(&writer, NULL, concurrent_device_writer, NULL);
    CHECK(!writer_created);
    while (!atomic_load(&original_entered)) Sleep(0);
    CHECK(!record_eye(1)->device_current && !record_eye(1)->certificate);
    CHECK(!pc_vr_render_trace_record(render_cb));
    atomic_store(&release_original, true);
    const int writer_joined = pthread_join(writer, NULL);
    CHECK(!writer_joined);
    atomic_store(&hold_original, false);
    const uint64_t republished = pc_vr_render_trace_record(render_cb);
    CHECK(republished == atomic_load(&render_certificate_count) && republished > second); end_scope(1);
    CHECK(pc_vr_render_trace_resolve(second, &out) && pc_vr_render_trace_resolve(republished, &out) &&
          out.traversal == first_copy.traversal && out.native_index == 1);
    // A qualified eye whose device matrix cannot be read cannot publish an aim-depth transform.
    pthread_mutex_lock(&projection_lock);
    RecordEye unreadable = *record_eye(1);
    unreadable.certificate = 0;
    unreadable.layer = 0;  // Its device matrix address is outside guest memory.
    render_publish_locked(&unreadable);
    pthread_mutex_unlock(&projection_lock);
    CHECK(!unreadable.certificate && atomic_load(&render_certificate_count) == republished);

    // Publish more certificates while another thread repeatedly resolves them.
    certificate_reader_first = (unsigned)first;
    const unsigned before_reader = atomic_load(&render_certificate_count);
    pthread_t reader; CHECK(!pthread_create(&reader, NULL, certificate_reader, NULL));
    while (!atomic_load(&certificate_reader_ready)) Sleep(0);
    for (unsigned frame = 0; frame < 7; ++frame) {
        ++mock_pose.frame_id;
        whole_frame();
        while (atomic_load(&certificate_reader_observed) < atomic_load(&render_certificate_count)) Sleep(0);
    }
    atomic_store(&certificate_reader_stop, true);
    void *reader_result = (void *)1;
    CHECK(!pthread_join(reader, &reader_result) && !reader_result);
    const unsigned published = atomic_load(&render_certificate_count);
    CHECK(published == before_reader + 14 && guards_counted(PC_VR_GUARD_CERTIFICATE_EXPIRED, 1));
    PcVrRenderCertificate retained[RENDER_CERTIFICATE_SLOTS];
    for (unsigned age = 0; age < RENDER_CERTIFICATE_SLOTS; ++age)
        CHECK(pc_vr_render_trace_resolve(published - age, &retained[age]) && retained[age].id == published - age);
    // Reset stops recording. Command lists still queued retain exactly the same lookup results.
    pc_vr_projection_trace_reset();
    CHECK(pc_vr_render_trace_enabled() && !atomic_load(&render_collecting));
    CHECK(!pc_vr_render_trace_record(render_cb));
    scope_call(0, false, RENDER_BEGIN_CALLER); scope_call(0, true, MAIN_BASE + 0xde1524);
    for (unsigned age = 0; age < RENDER_CERTIFICATE_SLOTS; ++age)
        CHECK(pc_vr_render_trace_resolve(published - age, &out) && !memcmp(&out, &retained[age], sizeof out));
    guards_mark();
    out = unchanged;
    CHECK(!pc_vr_render_trace_resolve(0, &out) && !memcmp(&out, &unchanged, sizeof out));
    CHECK(!pc_vr_render_trace_resolve(published + 1, &out) && !memcmp(&out, &unchanged, sizeof out));
    CHECK(!pc_vr_render_trace_resolve(first, NULL) && guards_counted(PC_VR_GUARD_COUNT, 0));
    CHECK(!pc_vr_render_trace_resolve(first, &out) && !memcmp(&out, &unchanged, sizeof out));
    CHECK(guards_counted(PC_VR_GUARD_CERTIFICATE_EXPIRED, 1));

    // The first id outside the retained window fails; the oldest id inside still carries its own data.
    restart();
    CHECK(pc_vr_render_trace_enabled() && atomic_load(&render_collecting));
    const unsigned before = atomic_load(&render_certificate_count);
    for (unsigned frame = 0; frame < 40; ++frame) { ++mock_pose.frame_id; whole_frame(); }
    CHECK(guards_counted(PC_VR_GUARD_COUNT, 0));
    const unsigned after = atomic_load(&render_certificate_count);
    CHECK(after == before + 80);
    PcVrRenderCertificate latest = unchanged;
    CHECK(pc_vr_render_trace_resolve(after, &latest) && latest.id == after && latest.native_index == 1 &&
          latest.traversal == projection_record.serial &&
          !memcmp(&latest.native_snapshot, &projection_record.pose, sizeof latest.native_snapshot));
    out = unchanged;
    CHECK(!pc_vr_render_trace_resolve(after - RENDER_CERTIFICATE_SLOTS, &out) && !memcmp(&out, &unchanged, sizeof out));
    CHECK(pc_vr_render_trace_resolve(after - RENDER_CERTIFICATE_SLOTS + 1, &out) &&
          out.id == after - RENDER_CERTIFICATE_SLOTS + 1);
    CHECK(guards_counted(PC_VR_GUARD_CERTIFICATE_EXPIRED, 1));
    guards_mark();
    CHECK(draw_certificate(0) == after - 1);
    // A draw into a command buffer other than the one its scope began with gets none (the scope's render info
    // rewritten in the middle is found at its end: changed_scope_thread).
    begin_scope(0);
    CHECK(pc_vr_render_trace_record(render_cb) == after - 1);
    CHECK(!pc_vr_render_trace_record(render_cb + 8));  // (another command buffer)
    CHECK(pc_vr_render_trace_record(render_cb) == after - 1);
    // A camera or projection built that is neither eye's changes nothing; a build of an eye's camera or projection
    // takes its certificate away.
    pc_vr_render_trace_invalidate_camera(base);
    pc_vr_render_trace_invalidate_camera(third_layer + 0x228);
    render_invalidate_projection(base + 0x288, false);
    render_invalidate_projection(third_layer + 0x288, true);
    pc_vr_render_trace_invalidate_camera(0x228);  // (no layer: never an eye's)
    CHECK(pc_vr_render_trace_record(render_cb) == after - 1);
    pc_vr_render_trace_invalidate_camera(layers[0] + 0x228);
    CHECK(!pc_vr_render_trace_record(render_cb) && !record_eye(0)->view_current);
    set_eye_certificate(0, after - 1);
    CHECK(pc_vr_render_trace_record(render_cb) == after - 1);
    render_invalidate_projection(layers[0] + 0x288, true);
    CHECK(!pc_vr_render_trace_record(render_cb) && !record_eye(0)->device_current && record_eye(0)->logical_current);
    set_eye_certificate(0, after - 1);
    render_invalidate_projection(layers[0] + 0x288, false);
    CHECK(!pc_vr_render_trace_record(render_cb) && !record_eye(0)->logical_current);
    end_scope(0);
    CHECK(draw_certificate(1) == after);
    // A frame whose root is abandoned by the next root (one camera written) yields no certificate of its own.
    const unsigned before_partial = atomic_load(&render_certificate_count);
    game_copies_eyes(); CHECK(root_call());
    whole_frame();
    CHECK(atomic_load(&render_certificate_count) == before_partial + 2 && guards_counted(PC_VR_GUARD_COUNT, 0));
    pc_vr_projection_trace_reset();
    CHECK(!atomic_load(&projection_enabled) && !atomic_load(&render_collecting));
    mock_generate_projection = false;
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

// A camera or projection built that is not an eye's (the game builds every camera and projection through the same two
// builders) takes no lock: with the trace's lock held elsewhere, those calls still finish, and change nothing.
static atomic_bool non_eye_done;
static void *non_eye_builds(void *unused) {
    (void)unused;
    pc_vr_render_trace_invalidate_camera(base);
    pc_vr_render_trace_invalidate_camera(third_layer + 0x228);
    builder_call(third_layer); device_call(third_layer);
    call_writer(base + 0x288, false, PROJ_BUILD_CALLER, base + 0x288 + 0xc);
    atomic_store(&non_eye_done, true);
    return NULL;
}
static void test_non_eye_builds(void) {
    fixture_objects(); restart(); whole_frame();
    const uint64_t left = record_eye(0)->certificate, right = record_eye(1)->certificate;
    const unsigned writes = parameter_write_calls;
    pthread_mutex_lock(&projection_lock);
    pthread_t thread;
    const int created = pthread_create(&thread, NULL, non_eye_builds, NULL);
    for (unsigned waited = 0; !created && waited < 5000 && !atomic_load(&non_eye_done); ++waited) Sleep(1);
    const bool done = atomic_load(&non_eye_done);
    pthread_mutex_unlock(&projection_lock);
    CHECK(!created && !pthread_join(thread, NULL) && done);
    CHECK(record_eye(0)->certificate == left && record_eye(1)->certificate == right);
    CHECK(draw_certificate(0) == left && draw_certificate(1) == right);
    CHECK(parameter_write_calls == writes && guards_counted(PC_VR_GUARD_COUNT, 0));
    pc_vr_projection_trace_reset();
    mock_generate_projection = false;
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

// The world scale: the record builds both eyes' views at the scale of its root's call, the certificates carry it, and
// the live scale multiplies, stays within [0.1, 100] and returns to the launch value at 0; nothing while not armed.
static void test_world_scale(void) {
    fixture_objects();
    restart_at(2);
    whole_frame();
    PcVrHeadLookCamera left = layer_view(layers[0]), right = layer_view(layers[1]);
    CHECK(projection_record.pair.game_units_per_meter == 2.0);
    CHECK(fabs((double)right.position[0] - left.position[0] - 0.128) < 0.00001);
    check_certificate(0, projection_record.serial, &mock_pose, 0, 2.0);
    CHECK(fabs(pc_vr_camera_focus_distance() - 2) < 1e-9);  // (4 game units at 2 per metre)
    CHECK(pc_vr_world_scale_active() && pc_vr_world_scale(1.25) == 2.5 && projection_view_scale == 2.5);
    whole_frame();
    left = layer_view(layers[0]); right = layer_view(layers[1]);
    CHECK(fabs((double)right.position[0] - left.position[0] - 0.16) < 0.00001);
    check_certificate(1, projection_record.serial, &mock_pose, 1, 2.5);
    CHECK(fabs(pc_vr_world_scale(0.8) - 2) < 1e-12);
    for (unsigned i = 0; i < 40; ++i) (void)pc_vr_world_scale(1.25);
    CHECK(projection_view_scale == 100);
    for (unsigned i = 0; i < 60; ++i) (void)pc_vr_world_scale(0.8);
    CHECK(projection_view_scale == 0.1);
    CHECK(pc_vr_world_scale(0) == 2 && projection_view_scale == 2);
    pc_vr_projection_trace_reset();
    CHECK(!pc_vr_world_scale_active() && pc_vr_world_scale(1.25) == 0);
    mock_generate_projection = false;
    CHECK(VirtualFree((void *)(uintptr_t)region, 0, MEM_RELEASE));
}

// The display's pair of eye layers (pc_vr_projection_trace_eye_layer): its root and sibling calls make both layers
// the eyes' before the record knows any (no camera binding yet), and another layer's draws then carry no eye flag.
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
    fixture_objects(); fixture_init();
    CHECK(!record_eyes_known_locked() && display_pair_is(0, 0));
    CHECK(scope_eye_layer(0) == (PC_VR_LAYER_DRAW_MAYBE_EYE | PC_VR_LAYER_DRAW_UNBOUND));  // (nothing known yet)
    // Root, then sibling on the same thread: the pair, and nothing else of the trace's.
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]);
    CHECK(projection_display_pair.open && display_pair_is(0, 0));
    pc_vr_projection_trace_eye_layer(layers[1], frames[1]);
    CHECK(display_pair_is(layers[0], layers[1]) && !projection_display_pair.open && projection_display_pairs == 1);
    CHECK(!record_eyes_known_locked() && !projection_record.serial);
    CHECK(region_clean());  // (no guest writes)
    // (eye layers by the display's pair alone: drawn with the game's own camera, unbound)
    CHECK(scope_eye_layer(0) == (PC_VR_LAYER_DRAW_EYE | PC_VR_LAYER_DRAW_UNBOUND) &&
          scope_eye_layer(1) == (PC_VR_LAYER_DRAW_EYE | PC_VR_LAYER_DRAW_UNBOUND));
    begin_scope(0);
    const uint32_t eye_draw = rt_layer_draw_begin(PC_VR_LAYER_DRAW_FINAL);
    CHECK(pc_vr_layer_draw_recording() == (PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE | PC_VR_LAYER_DRAW_UNBOUND));
    rt_layer_draw_end(eye_draw); end_scope(0);
    // Another layer, and an eye layer drawn through another camera or projection: not an eye's.
    const uint64_t other_rinfo = region + 0x3500;
    u64(other_rinfo, 9); u64(other_rinfo + 0x18, third_layer); u64(other_rinfo + 0x28, third_layer + 0x228);
    u64(other_rinfo + 0x30, third_layer + 0x288); u64(other_rinfo + 0x48, render_context);
    Cpu other_scope = {0};
    other_scope.x[0] = third_layer; other_scope.x[1] = other_rinfo; other_scope.x[30] = RENDER_BEGIN_CALLER;
    render_begin_wrapper(&other_scope);
    CHECK(render_scope_eye_layer() == 0);
    other_scope.x[30] = MAIN_BASE + 0xde1560; render_end_wrapper(&other_scope);
    CHECK(!render_tls()->poisoned && !render_tls()->depth);
    u64(render_rinfo[0] + 0x28, base); CHECK(scope_eye_layer(0) == 0); u64(render_rinfo[0] + 0x28, layers[0] + 0x228);
    u64(render_rinfo[1] + 0x30, layers[0] + 0x288); CHECK(scope_eye_layer(1) == 0);
    u64(render_rinfo[1] + 0x30, layers[1] + 0x288);
    // The same pair again changes nothing: no new pair is counted.
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]); pc_vr_projection_trace_eye_layer(layers[1], frames[1]);
    CHECK(projection_display_pairs == 1 && display_pair_is(layers[0], layers[1]));
    // A call that is neither a root's nor a sibling's (another layer's, through the same builder) leaves an open root.
    const uint64_t unrelated_frame = region + 0x3600;
    u64(unrelated_frame, region + 0x3900); u64(unrelated_frame + 8, MAIN_BASE + 0x1234);
    fixture_init(); pc_vr_projection_trace_eye_layer(layers[0], frames[0]);
    pc_vr_projection_trace_eye_layer(third_layer, unrelated_frame);
    pc_vr_projection_trace_eye_layer(layers[1], frames[1]);
    CHECK(display_pair_is(layers[0], layers[1]));
    // Each broken stack or link test forms no pair (from a trace with none known), one cause at a time.
#define NO_PAIR(at, value) do { \
    const uint64_t saved_value = *(uint64_t *)(uintptr_t)(at); \
    fixture_init(); u64((at), (value)); \
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]); pc_vr_projection_trace_eye_layer(layers[1], frames[1]); \
    CHECK(display_pair_is(0, 0) && !projection_display_pairs); \
    CHECK(scope_eye_layer(0) == (PC_VR_LAYER_DRAW_MAYBE_EYE | PC_VR_LAYER_DRAW_UNBOUND)); \
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
    fixture_init(); u16(layers[1] + 0x90, 0);
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]); pc_vr_projection_trace_eye_layer(layers[1], frames[1]);
    CHECK(display_pair_is(0, 0) && !projection_display_pair.open);
    u16(layers[1] + 0x90, 0x1000);
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
    u64(layers[0] + 0x208, third_layer);
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]); pc_vr_projection_trace_eye_layer(third_layer, frames[1]);
    CHECK(display_pair_is(layers[0], third_layer) && projection_display_pairs == 2);
    CHECK(scope_eye_layer(1) == 0);  // (the old sibling's layer, no longer an eye's)
    u64(layers[0] + 0x208, layers[1]);
    pc_vr_projection_trace_eye_layer(layers[0], frames[0]); pc_vr_projection_trace_eye_layer(layers[1], frames[1]);
    CHECK(display_pair_is(layers[0], layers[1]) &&
          scope_eye_layer(1) == (PC_VR_LAYER_DRAW_EYE | PC_VR_LAYER_DRAW_UNBOUND));
    // Once the record knows an eye layer it decides alone (the display also shows the pause menu's own pair): the
    // record's eye layer is an eye's, the pair's other layer, not yet in the record, is not.
    native_pose(); CHECK(root_call());
    // (its view written, not yet built by the game's builder: still the game's camera, unbound)
    CHECK(record_eye(0)->layer == layers[0] &&
          scope_eye_layer(0) == (PC_VR_LAYER_DRAW_EYE | PC_VR_LAYER_DRAW_UNBOUND));
    CHECK(!record_eye(1)->layer && scope_eye_layer(1) == 0);
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
    test_pc_vr_configure((const char *const[]){"vr", "1", NULL});  // (the configuration's own scale)
    pc_vr_projection_trace_reset();
    mock_pc_vr = false; pc_vr_projection_trace_init();
    CHECK(!atomic_load(&projection_enabled) && !atomic_load(&render_collecting));
    mock_pc_vr = true;
    pc_vr_projection_trace_init();
    CHECK(atomic_load(&projection_enabled) && pc_vr_render_trace_enabled() && atomic_load(&render_collecting));
    CHECK(projection_view_scale == 14.53 && pc_vr_world_scale_active());
    test_pc_vr_configure((const char *const[]){"vr", "1",
        "vr_game_units_per_meter", "20", NULL});
    pc_vr_projection_trace_init();
    CHECK(projection_view_scale == 20 && atomic_load(&render_collecting));
    CHECK(pc_vr_world_scale(1.25) == 25 && pc_vr_world_scale(0) == 20);  // (the live scale's launch value)
    test_pc_vr_configure((const char *const[]){"vr", "1", NULL});  // (back to the default scale; nothing switches the
                                                                   // traces off)
    pc_vr_projection_trace_init();
    CHECK(atomic_load(&projection_enabled) && pc_vr_render_trace_enabled() && atomic_load(&render_collecting));
    CHECK(projection_view_scale == 14.53);
    pc_vr_projection_trace_reset();
    test_pc_vr_configure((const char *const[]){"vr", "1", NULL});
}

int main(void) {
    test_pc_vr_configure((const char *const[]){"vr", "1", NULL});
    test_init();
#ifdef _WIN32
    test_writers_alone();
    test_frame_record();
    test_root_refusals();
    test_sibling_refusals();
    test_record_serials();
    test_eye_built();
    test_native_fov();
    test_draws_across_frames();
    test_certificates_outstanding();
    test_projection_view_midpoints();
    test_render_trace();
    test_non_eye_builds();
    test_world_scale();
    test_display_pair();
    test_temporal_offset();
#endif
    test_play_configuration();
    // Every accepted root call published its two eye views with its base camera (for the head view of visibility).
    CHECK(cull_view_publications > 0 && cull_view_last_base != 0);
    printf("PC VR projection/native-view fixture: %u checks passed (synthetic guest pages only)\n", checks);
    return 0;
}

// hle/labo_guest_memory.h: a new access generation per call keeps its page cache cold here.
uint64_t plat_guest_access_generation(void) { static uint64_t generation; return ++generation; }
