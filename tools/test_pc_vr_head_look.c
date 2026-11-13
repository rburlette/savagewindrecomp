// Asset-free pinned head-look integration contract. Includes the implementation
// so the mocked dispatch, XR snapshot and guest pages exercise the real gate.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static bool fail_query, fail_read, fail_write, fail_working_set;
static BOOL WINAPI fixture_query_working_set(HANDLE process, PVOID info, DWORD bytes) {
    return fail_working_set ? FALSE : QueryWorkingSetEx(process, info, bytes);
}
static SIZE_T WINAPI fixture_virtual_query(LPCVOID address,
                                           PMEMORY_BASIC_INFORMATION out, SIZE_T bytes) {
    return fail_query ? 0 : VirtualQuery(address, out, bytes);
}
static BOOL WINAPI fixture_read_process_memory(HANDLE process, LPCVOID address,
                                               LPVOID out, SIZE_T bytes, SIZE_T *copied) {
    return fail_read ? FALSE : ReadProcessMemory(process, address, out, bytes, copied);
}
static BOOL WINAPI fixture_write_process_memory(HANDLE process, LPVOID address,
                                                LPCVOID in, SIZE_T bytes, SIZE_T *copied) {
    return fail_write ? FALSE : WriteProcessMemory(process, address, in, bytes, copied);
}
#define VirtualQuery fixture_virtual_query
#define ReadProcessMemory fixture_read_process_memory
#define WriteProcessMemory fixture_write_process_memory
#undef QueryWorkingSetEx
#define QueryWorkingSetEx fixture_query_working_set
#endif
#include "test_game_sites_182.h"
#include "../runtime/hle/pc_vr_head_look.c"
#include "../runtime/pc_vr_main.h"
#include "test_pc_vr_settings.h"
#define MAIN_SIZE TEST_MAIN_SIZE_182
#ifdef _WIN32
#undef VirtualQuery
#undef ReadProcessMemory
#undef WriteProcessMemory
#undef QueryWorkingSetEx
#endif

static unsigned checks, override_calls, lookup_calls, original_calls, pose_reads;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "head-look check failed line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

static bool mock_pc_vr;
static uint64_t mock_ns = UINT64_C(1000000000000000); // QPC/log domain.
static uint64_t mock_pose_ns = UINT64_C(10000000000); // SDL/XR pose domain.
static uint64_t mock_clock_step, mock_original_cost_ns;
static XrPoseReadResult mock_pose_result = XR_POSE_VALID;
static XrPoseSnapshot mock_pose;
static XrCalibration mock_calibration = {.epoch = 1, .heading_xyzw = {0, 0, 0, 1}};
static uint32_t module_count = 1;
static GameLibModule game_module = {.name = "main", .base = MAIN_BASE, .size = MAIN_SIZE,
    .build_id = "de0b6ac4efc1dda7f80f6d7598ec7a7d00000000000000000000000000000000"};
static GameLibTables game_tables = {.modules = &game_module, .nmodules = &module_count};
static Module loaded_main = {.name = "main", .base = MAIN_BASE, .size = MAIN_SIZE};
static uint64_t override_address;
static HostFn override_function;
static PcVrHeadLookCamera original_seen;
static PcVrNativeViewResult mock_native_mode;
static PcVrHeadLookCamera native_delivered;
static unsigned native_prepare_calls, native_post_calls, legacy_applied_acks, native_original_count;
static bool native_pending, mock_native_post_ok = true, mock_change_calibration, native_saw_reference;
static bool mock_root;

bool pc_vr_enabled(void) { return mock_pc_vr; }
bool pc_vr_main_compatible(void) { return pc_vr_main_layout(gamelib_tables(), loader_module("main")); }
void pc_vr_projection_trace_init(void) {}
bool pc_vr_projection_trace_root(uint64_t frame) { (void)frame; return mock_root; }
void pc_vr_projection_trace_eye_layer(uint64_t layer, uint64_t frame_pointer) { (void)layer; (void)frame_pointer; }
void pc_vr_render_trace_invalidate_camera(uint64_t camera) { (void)camera; }
void pc_vr_projection_trace_eye(uint64_t layer, uint64_t base, uint64_t sequence,
    uint64_t frame, uint64_t generation, bool valid, uint64_t frame_pointer, const XrPoseSnapshot *head_pose) {
    (void)layer; (void)base; (void)sequence; (void)frame; (void)generation; (void)valid;
    (void)frame_pointer;
    CHECK(!valid || (head_pose && head_pose->frame_id == frame && head_pose->generation == generation));
}
void pc_vr_projection_trace_head_result(uint64_t layer, uint64_t sequence, bool applied) {
    (void)layer; (void)sequence;
    if (applied) ++legacy_applied_acks;
}
PcVrNativeViewResult pc_vr_projection_trace_native_view(uint64_t layer, uint64_t sequence,
    const PcVrHeadLookCamera *center, const PcVrHeadLookCamera *eye,
    const PcVrNativeViewReference *reference, PcVrNativeViewToken *token) {
    (void)center;
    if (mock_native_mode == PC_VR_NATIVE_VIEW_LEGACY) return mock_native_mode;
    ++native_prepare_calls;
    native_saw_reference = reference != NULL;
    if (mock_native_mode == PC_VR_NATIVE_VIEW_APPLIED) {
        CHECK(reference != NULL);
        native_delivered = *eye;
        native_delivered.target[0] += 1.25f;
        CHECK(labo_guest_copy(layer + 0x260, &native_delivered, sizeof native_delivered, true));
        *token = (PcVrNativeViewToken){.note = 1, .traversal_serial = 1,
            .layer = layer, .sequence = sequence, .reference = *reference};
        native_original_count = original_calls;
        native_pending = true;
    }
    return mock_native_mode;
}
bool pc_vr_projection_trace_native_view_built(const PcVrNativeViewToken *token, bool current) {
    ++native_post_calls;
    CHECK(token->note && native_pending && original_calls == native_original_count + 1);
    CHECK(!memcmp(&original_seen, &native_delivered, sizeof native_delivered));
    CHECK(current == !mock_change_calibration);
    native_pending = false;
    return current && mock_native_post_ok;
}
const GameLibTables *gamelib_tables(void) { return &game_tables; }
Module *loader_module(const char *name) { return !strcmp(name, "main") ? &loaded_main : NULL; }
uint64_t plat_now_ns(void) {
    const uint64_t result = mock_ns;
    mock_ns += mock_clock_step;
    return result;
}
uint64_t xr_runtime_pose_clock_ns(void) { return mock_pose_ns; }
XrPoseReadResult xr_runtime_get_pose(XrPoseSnapshot *out) {
    ++pose_reads;
    if (mock_pose_result == XR_POSE_VALID) *out = mock_pose;
    return mock_pose_result;
}
static void mock_original(Cpu *c) {
    ++original_calls;
    mock_ns += mock_original_cost_ns;
    if (valid_camera_address(c->x[0])) (void)read_camera(c->x[0], &original_seen);
    if (native_pending && mock_change_calibration) ++reference_generation;
    c->x[5] ^= UINT64_C(0x54321);
    c->sp -= 32;
}
GuestFn dispatch_lookup(uint64_t address) {
    ++lookup_calls;
    return address == BUILDER ? mock_original : NULL;
}
void hle_override(uint64_t address, HostFn fn, const char *name) {
    CHECK(++override_calls == 1);
    CHECK(address == BUILDER && !strcmp(name, "PcVrHeadLookBuilder"));
    override_address = address;
    override_function = fn;
}
void rt_fatal_trace(void) { abort(); }
// The diagnostic SAVAGEWIND_PC_VR_HEAD_LOOK_TIMING in the process's configuration (pc_vr_config.h); head look itself
// is PC VR's play configuration (mock_pc_vr here).
static void set_timing_optin(const char *value) {
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1", "SAVAGEWIND_PC_VR_HEAD_LOOK_TIMING", value, NULL});
}

static void set_pose(float x, float y, float z, float w, uint64_t generation) {
    mock_pose_result = XR_POSE_VALID;
    memset(&mock_pose, 0, sizeof mock_pose);
    mock_pose.orientation_xyzw[0] = x;
    mock_pose.orientation_xyzw[1] = y;
    mock_pose.orientation_xyzw[2] = z;
    mock_pose.orientation_xyzw[3] = w;
    mock_pose.generation = generation;
    mock_pose.frame_id = 100 + generation;
    mock_pose.published_monotonic_ns = mock_pose_ns;
    mock_pose.active = mock_pose.focused = mock_pose.orientation_valid =
        mock_pose.orientation_tracked = true;
    mock_pose.calibration = mock_calibration;
}

static void advance_clocks(uint64_t duration_ns) {
    mock_ns += duration_ns;
    mock_pose_ns += duration_ns;
}

static void call_builder(uint64_t object, uint64_t output, uint64_t caller) {
    Cpu actual = {0}, expected = {0};
    actual.x[0] = expected.x[0] = object;
    actual.x[1] = expected.x[1] = output;
    actual.x[5] = expected.x[5] = 3;
    actual.x[30] = expected.x[30] = caller;
    actual.sp = expected.sp = 0x1000;
    mock_original(&expected);
    const unsigned prior = original_calls;
    override_function(&actual);
    CHECK(original_calls == prior + 1);
    CHECK(!memcmp(&actual, &expected, sizeof actual));
}

static bool same_camera(const PcVrHeadLookCamera *a, const PcVrHeadLookCamera *b) {
    return !memcmp(a, b, sizeof *a);
}

static bool near_camera(const PcVrHeadLookCamera *a, const PcVrHeadLookCamera *b) {
    for (unsigned i = 0; i < 3; ++i)
        if (fabsf(a->position[i] - b->position[i]) > 0.002f || fabsf(a->target[i] - b->target[i]) > 0.002f ||
            fabsf(a->up[i] - b->up[i]) > 0.002f) return false;
    return true;
}

#ifdef _WIN32
static uint64_t controller, weak, actor, base, other_base, layer, eye, layer2, eye2;
static PcVrHeadLookCamera base_initial, eye_initial, eye2_initial;
static void write_u64(uint64_t at, uint64_t value) { *(uint64_t *)(uintptr_t)at = value; }
static void write_camera(uint64_t at, const PcVrHeadLookCamera *value) {
    memcpy((void *)(uintptr_t)(at + 0x38), value, sizeof *value);
}
static PcVrHeadLookCamera camera_at(uint64_t at) {
    PcVrHeadLookCamera result;
    CHECK(read_camera(at, &result));
    return result;
}
static void restore_eye(void) { write_camera(eye, &eye_initial); }
static void setup_guest(void) {
    const uint64_t address = MAIN_BASE + UINT64_C(0x22000000);
    void *region = VirtualAlloc((void *)(uintptr_t)address, 0x5000,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(region == (void *)(uintptr_t)address);
    controller = address + 0x100;
    weak = address + 0x180;
    actor = address + 0x300;
    base = address + 0x1800;
    other_base = address + 0x1a00;
    layer = address + 0x2200;
    eye = layer + 0x228;
    layer2 = address + 0x3000;
    eye2 = layer2 + 0x228;
    write_u64(controller + 0x28, weak);
    write_u64(weak + 8, actor);
    write_u64(actor, ACTOR_VTABLE);
    write_u64(actor + 0x1088, base);
    write_u64(base, BASE_VTABLE);
    write_u64(other_base, BASE_VTABLE);
    write_u64(eye, EYE_VTABLE);
    write_u64(layer + 0x80, base);
    write_u64(eye2, EYE_VTABLE);
    write_u64(layer2 + 0x80, base);
    *(uint16_t *)(uintptr_t)(layer2 + 0x90) = 0x1000;
    base_initial = (PcVrHeadLookCamera){.position = {0, 2, 5},
        .target = {0, 2, 0}, .up = {0, 1, 0}};
    eye_initial = (PcVrHeadLookCamera){.position = {0.03f, 2, 5},
        .target = {0.03f, 2, 0}, .up = {0, 1, 0}};
    eye2_initial = (PcVrHeadLookCamera){.position = {-0.03f, 2, 5},
        .target = {-0.03f, 2, 0}, .up = {0, 1, 0}};
    write_camera(base, &base_initial);
    write_camera(other_base, &base_initial);
    restore_eye();
    write_camera(eye2, &eye2_initial);
}
#endif

int main(void) {
    // Without PC VR, no head look, whatever the diagnostics ask (with it, head look is part of the play configuration:
    // no switch of its own turns it off).
    set_timing_optin("1");
    mock_pc_vr = false;
    pc_vr_head_look_init();
    CHECK(!pc_vr_head_look_enabled() && !atomic_load(&timing_enabled) &&
          !registered && !override_calls);
    set_timing_optin(NULL);
    mock_pc_vr = true;
    const uint64_t builder = g_site.pcvr_matrix_builder;
    g_site.pcvr_matrix_builder = 0;  // (a game version without PC VR's sites)
    pc_vr_head_look_init();
    CHECK(!pc_vr_head_look_enabled() && !override_calls);
    g_site.pcvr_matrix_builder = builder;
    loaded_main.size--;
    pc_vr_head_look_init();
    CHECK(!pc_vr_head_look_enabled() && !override_calls);
    loaded_main.size++;
    pc_vr_head_look_init();
    CHECK(pc_vr_head_look_enabled() && registered && override_calls == 1);
    CHECK(override_address == BUILDER && override_function == head_look_builder);
    CHECK(lookup_calls == 1);
    CHECK(mock_ns > mock_pose_ns + UINT64_C(1000000000000));
    CHECK(!summary_active && summaries == 0);

#ifdef _WIN32
    setup_guest();
    // No validated controller binding: base and eye calls forward unchanged.
    set_pose(0, 0, 0, 1, 7);
    call_builder(base, base + 8, BASE_CALLER);
    call_builder(eye, eye + 8, EYE_CALLER);
    mock_ns += UINT64_C(300000000000); // Long menu time consumes no budget.
    call_builder(eye, eye + 8, EYE_CALLER);
    CHECK(!summary_active && summaries == 0);
    PcVrHeadLookCamera eye_result = camera_at(eye);
    CHECK(same_camera(&eye_result, &eye_initial) && !reference_valid);
    pc_vr_head_look_observe_controller(controller);
    CHECK(binding_valid && binding.base == base && binding.actor == actor);
    // Wrong builder LR and wrong x1 never acquire a head latch.
    call_builder(base, base + 8, MAIN_BASE + 0x1000);
    CHECK(!latch_valid);
    call_builder(base, base + 16, BASE_CALLER);
    CHECK(!latch_valid);
    call_builder(base, base + 8, BASE_CALLER);
    CHECK(latch_valid && reference_valid && reference_generation == 1); // the XR core's calibration
    CHECK(summary_active && summaries == 0); // Divergent QPC cannot age XR pose.
    unsigned reads_before_other = pose_reads;
    call_builder(other_base, other_base + 8, BASE_CALLER);
    CHECK(latch_valid && pose_reads == reads_before_other);
    write_u64(layer2 + 0x80, other_base);
    call_builder(eye2, eye2 + 8, EYE_CALLER);
    PcVrHeadLookCamera eye2_result = camera_at(eye2);
    CHECK(latch_valid && same_camera(&eye2_result, &eye2_initial));
    write_u64(layer2 + 0x80, base);
    call_builder(eye, eye + 8, EYE_CALLER);
    CHECK(reference_valid && reference_generation == 1);
    eye_result = camera_at(eye);
    CHECK(same_camera(&eye_result, &eye_initial)); // Head at the calibration heading.
    PcVrHeadLookCamera base_result = camera_at(base);
    CHECK(same_camera(&base_result, &base_initial));

    // A +90 degree LOCAL yaw rotates viewing direction around the unchanged
    // base position. The original eye offset remains present and rotates too.
    advance_clocks(UINT64_C(10000000));
    set_pose(0, 0.70710678f, 0, 0.70710678f, 7);
    mock_pose.focused = false; // VISIBLE tracking still rotates the rendered view.
    call_builder(base, base + 8, BASE_CALLER);
    CHECK(latch_valid);
    unsigned reads_before_eyes = pose_reads;
    restore_eye();
    write_camera(eye2, &eye2_initial);
    unsigned char actor_before[0x1090];
    unsigned char base_matrix_before[0x30];
    memcpy(actor_before, (void *)(uintptr_t)actor, sizeof actor_before);
    memcpy(base_matrix_before, (void *)(uintptr_t)(base + 8), sizeof base_matrix_before);
    call_builder(eye, eye + 8, EYE_CALLER);
    eye_result = camera_at(eye);
    base_result = camera_at(base);
    CHECK(same_camera(&base_result, &base_initial));
    CHECK(!same_camera(&eye_result, &eye_initial));
    CHECK(fabsf(eye_result.position[0]) < 0.002f);
    CHECK(fabsf(eye_result.position[2] - 4.97f) < 0.002f);
    CHECK(fabsf(eye_result.target[0] + 5.0f) < 0.002f);
    CHECK(same_camera(&eye_result, &original_seen));
    PcVrHeadLookCamera rotated_once = eye_result;
    // The second eye uses the same base latch even if the XR publication has
    // changed between eye builders. Opposite stereo offset remains present.
    set_pose(0, 0, 0, 1, 7);
    call_builder(eye2, eye2 + 8, EYE_CALLER);
    eye2_result = camera_at(eye2);
    PcVrHeadLookCamera expected_eye2;
    CHECK(pc_vr_head_look_apply_eye(&base_initial, &eye2_initial,
                                     latch_relative_q, &expected_eye2));
    CHECK(same_camera(&eye2_result, &expected_eye2));
    CHECK(latch_eye_count == 2 && latch_side_mask == 3);
    CHECK(pose_reads == reads_before_eyes);
    CHECK(!memcmp(actor_before, (void *)(uintptr_t)actor, sizeof actor_before));
    CHECK(!memcmp(base_matrix_before, (void *)(uintptr_t)(base + 8),
                  sizeof base_matrix_before));
    // No fresh base->eye copy: repeated exact call must not accumulate.
    call_builder(eye, eye + 8, EYE_CALLER);
    eye_result = camera_at(eye);
    CHECK(same_camera(&eye_result, &rotated_once));
    // A rejected unrefreshed eye invalidates that pair; the next base builder
    // supplies a fresh latch before the next source-copied eye.
    set_pose(0, 0.70710678f, 0, 0.70710678f, 7);
    call_builder(base, base + 8, BASE_CALLER);
    restore_eye();
    call_builder(eye, eye + 8, EYE_CALLER);
    eye_result = camera_at(eye);
    CHECK(same_camera(&eye_result, &rotated_once));

    restore_eye();
    call_builder(eye, eye + 8, MAIN_BASE + 0x9999);
    eye_result = camera_at(eye);
    CHECK(same_camera(&eye_result, &eye_initial));
    write_u64(eye, EYE_VTABLE + 8);
    call_builder(eye, eye + 8, EYE_CALLER);
    eye_result = camera_at(eye);
    CHECK(same_camera(&eye_result, &eye_initial));
    write_u64(eye, EYE_VTABLE);
    write_u64(layer + 0x80, eye);
    call_builder(eye, eye + 8, EYE_CALLER);
    eye_result = camera_at(eye);
    CHECK(same_camera(&eye_result, &eye_initial));
    write_u64(layer + 0x80, base);
    write_u64(actor, ACTOR_VTABLE + 8);
    call_builder(eye, eye + 8, EYE_CALLER);
    eye_result = camera_at(eye);
    CHECK(same_camera(&eye_result, &eye_initial));
    write_u64(actor, ACTOR_VTABLE);

    // BUSY drops this base latch but retains neutral. No eye write is allowed.
    mock_pose_result = XR_POSE_BUSY;
    call_builder(base, base + 8, BASE_CALLER);
    CHECK(reference_valid && !latch_valid && reference_generation == 1);
    call_builder(eye, eye + 8, EYE_CALLER);
    eye_result = camera_at(eye);
    CHECK(same_camera(&eye_result, &eye_initial));
    mock_pose_result = XR_POSE_UNAVAILABLE;
    call_builder(base, base + 8, BASE_CALLER);
    CHECK(!reference_valid && !latch_valid);
    // D053: tracking recovery (a new generation) keeps the calibration, so a
    // head still turned 90 degrees stays turned instead of becoming forward.
    advance_clocks(UINT64_C(10000000));
    set_pose(0, 0.70710678f, 0, 0.70710678f, 8);
    call_builder(base, base + 8, BASE_CALLER);
    CHECK(latch_valid && reference_valid && reference_generation == 1);
    restore_eye();
    call_builder(eye, eye + 8, EYE_CALLER);
    eye_result = camera_at(eye);
    CHECK(same_camera(&eye_result, &rotated_once));
    // So does a rebinding (a new section): only the local copy is cleared.
    clear_reference();
    call_builder(base, base + 8, BASE_CALLER); restore_eye();
    call_builder(eye, eye + 8, EYE_CALLER);
    eye_result = camera_at(eye);
    CHECK(same_camera(&eye_result, &rotated_once));
    // A new epoch (the runtime's recenter) captured with the head turned looks ahead.
    mock_calibration = (XrCalibration){.epoch = 2, .heading_xyzw = {0, 0.70710678f, 0, 0.70710678f}};
    set_pose(0, 0.70710678f, 0, 0.70710678f, 8);
    call_builder(base, base + 8, BASE_CALLER); restore_eye();
    call_builder(eye, eye + 8, EYE_CALLER);
    CHECK(reference_valid && reference_generation == 2);
    eye_result = camera_at(eye);
    CHECK(near_camera(&eye_result, &eye_initial));
    mock_calibration = (XrCalibration){.epoch = 3, .heading_xyzw = {0, 0, 0, 1}};
    // Valid but stale publication cannot continue an old reference.
    advance_clocks(POSE_MAX_AGE_NS + 1);
    call_builder(base, base + 8, BASE_CALLER);
    CHECK(!reference_valid && !latch_valid);
    set_pose(0, 0, 0, 1, 9);
    call_builder(base, base + 8, BASE_CALLER);
    restore_eye();
    call_builder(eye, eye + 8, EYE_CALLER);
    CHECK(reference_valid && reference_generation == 3);
    // A later controller/base placement becomes the new head-look pivot.
    base_initial.position[0] = base_initial.target[0] = 1.0f;
    eye_initial.position[0] = eye_initial.target[0] = 1.03f;
    write_camera(base, &base_initial);
    restore_eye();
    memcpy(actor_before, (void *)(uintptr_t)actor, sizeof actor_before);
    advance_clocks(UINT64_C(10000000));
    set_pose(0, 0.70710678f, 0, 0.70710678f, 9);
    call_builder(base, base + 8, BASE_CALLER);
    call_builder(eye, eye + 8, EYE_CALLER);
    eye_result = camera_at(eye);
    CHECK(fabsf(eye_result.position[0] - 1.0f) < 0.002f);
    CHECK(fabsf(eye_result.target[0] + 4.0f) < 0.002f);
    base_result = camera_at(base);
    CHECK(same_camera(&base_result, &base_initial));
    CHECK(!memcmp(actor_before, (void *)(uintptr_t)actor, sizeof actor_before));
    // A publication from the future in SDL's own clock domain must fail.
    mock_pose.published_monotonic_ns = mock_pose_ns + 1;
    call_builder(base, base + 8, BASE_CALLER);
    CHECK(!reference_valid && !latch_valid);
    set_pose(0, 0, 0, 1, 10);
    call_builder(base, base + 8, BASE_CALLER);
    CHECK(latch_valid && reference_valid);
    mock_pose_ns += POSE_MAX_AGE_NS + 1; // Eye freshness uses SDL, not QPC.
    restore_eye();
    call_builder(eye, eye + 8, EYE_CALLER);
    eye_result = camera_at(eye);
    CHECK(!latch_valid && !reference_valid);
    CHECK(same_camera(&eye_result, &eye_initial));
    // No head look before the XR core's first calibration.
    mock_calibration.epoch = 0;
    set_pose(0, 0.70710678f, 0, 0.70710678f, 10);
    call_builder(base, base + 8, BASE_CALLER);
    CHECK(!latch_valid && !reference_valid);
    restore_eye();
    call_builder(eye, eye + 8, EYE_CALLER);
    eye_result = camera_at(eye);
    CHECK(same_camera(&eye_result, &eye_initial));
    mock_calibration.epoch = 3;
    // World-scale rounding from separately translated float positions must
    // still pass the source-backed fresh-copy invariant.
    PcVrHeadLookCamera far_base = {.position = {4000.0f, 2, 4005.0f},
        .target = {4000.0f, 2, 4000.0f}, .up = {0, 1, 0}};
    PcVrHeadLookCamera far_eye = {.position = {4000.03003f, 2, 4005.03003f},
        .target = {4000.03003f, 2, 4000.03003f}, .up = {0, 1, 0}};
    CHECK(freshly_copied_eye(&far_base, &far_eye));
    far_eye.target[0] += 0.1f;
    CHECK(!freshly_copied_eye(&far_base, &far_eye));

    // Cached/alternate base preparation must not stop head look. Each real
    // root reads the game's current base and captures one complete XR sample;
    // its sibling retains that sample across a publication or base callback.
    base_initial = (PcVrHeadLookCamera){.position = {0, 2, 5},
        .target = {0, 2, 0}, .up = {0, 1, 0}};
    eye_initial.position[0] = eye_initial.target[0] = 0.03f;
    write_camera(base, &base_initial);
    for (unsigned i = 0; i < 20; ++i) {
        advance_clocks(POSE_MAX_AGE_NS + 1);
        set_pose(0, 0.70710678f, 0, 0.70710678f, 11);
        ++mock_pose.frame_id;
        const uint64_t sequence_before = latch_sequence;
        restore_eye(); write_camera(eye2, &eye2_initial);
        mock_root = true;
        const uint64_t root_eye = i & 1 ? eye2 : eye;
        const uint64_t sibling_eye = i & 1 ? eye : eye2;
        call_builder(root_eye, root_eye + 8, EYE_CALLER);
        CHECK(latch_root_owned && latch_valid && latch_sequence == sequence_before + 1);
        const XrPoseSnapshot root_pose = latch_pose;
        set_pose(0, 0, 0, 1, 11); mock_pose.frame_id += 10;
        // Neither the alternate LR nor a normal base rebuild may overwrite
        // the open root. The original builder still runs exactly once.
        call_builder(base, base + 8, LAYER_BASE_CALLER);
        if (!(i % 3)) call_builder(base, base + 8, BASE_CALLER);
        CHECK(!memcmp(&root_pose, &latch_pose, sizeof root_pose));
        mock_root = false;
        call_builder(sibling_eye, sibling_eye + 8, EYE_CALLER);
        CHECK(latch_valid && latch_eye_count == 2 && latch_side_mask == 3);
        CHECK(!memcmp(&root_pose, &latch_pose, sizeof root_pose));
        base_result = camera_at(base);
        CHECK(same_camera(&base_result, &base_initial));
    }
    // A rejected root has no old latch to reuse and no per-eye recovery.
    mock_root = true; mock_pose_result = XR_POSE_BUSY; restore_eye();
    call_builder(eye, eye + 8, EYE_CALLER);
    CHECK(!latch_valid && !latch_root_owned);
    mock_root = false; mock_pose_result = XR_POSE_VALID;
    write_camera(eye2, &eye2_initial);
    call_builder(eye2, eye2 + 8, EYE_CALLER);
    CHECK(!latch_valid);
    // Reject malformed original copies before any new root pose read.
    mock_root = true; restore_eye();
    *(float *)(uintptr_t)(eye + 0x44) += 1;
    const unsigned reads_before_bad_copy = pose_reads;
    call_builder(eye, eye + 8, EYE_CALLER);
    CHECK(!latch_valid && pose_reads == reads_before_bad_copy);
    mock_root = false;
    // An intervening callback preserves identity, but cannot hide tracking
    // loss, recenter, contention or a new base placement from the pair.
    for (unsigned mode = 0; mode < 5; ++mode) {
        set_pose(0, 0.70710678f, 0, 0.70710678f, 11);
        mock_pose_result = XR_POSE_VALID;
        write_camera(base, &base_initial); restore_eye();
        mock_root = true;
        call_builder(eye, eye + 8, EYE_CALLER);
        CHECK(latch_root_owned && latch_eye_count == 1);
        mock_root = false;
        if (mode == 0) mock_pose_result = XR_POSE_BUSY;
        if (mode == 1) mock_pose.active = false;
        if (mode == 2) ++mock_pose.generation;
        if (mode == 3) ++mock_pose.calibration.epoch;
        if (mode == 4) *(float *)(uintptr_t)(base + 0x38) += 1;
        call_builder(base, base + 8, BASE_CALLER);
        CHECK(!latch_valid);
    }
    write_camera(base, &base_initial);
    mock_pose_result = XR_POSE_VALID;

    // The optional timer excludes the original generated builder from wrapper
    // self-cost, samples that original only once per 64 generic calls, and
    // discards prebinding menu traffic before its bounded report window.
    pc_vr_head_look_reset();
    set_timing_optin("1");
    pc_vr_head_look_init();
    CHECK(pc_vr_head_look_enabled() && atomic_load(&timing_enabled));
    CHECK(override_calls == 1 && lookup_calls == 1);
    mock_clock_step = 100;
    mock_original_cost_ns = UINT64_C(1000000);
    for (unsigned i = 0; i < 65; ++i)
        call_builder(base, base + 8, MAIN_BASE + 0x9999);
    CHECK(!timing_active && atomic_load(&timing_generic) == 65);
    pc_vr_head_look_observe_controller(controller);
    CHECK(timing_active && atomic_load(&timing_generic) == 0);
    CHECK(atomic_load(&timing_observe_calls) == 1);
    for (unsigned i = 0; i < 64; ++i)
        call_builder(base, base + 8, MAIN_BASE + 0x9999);
    CHECK(atomic_load(&timing_generic) == 64);
    CHECK(atomic_load(&timing_original_samples) == 1);
    CHECK(atomic_load(&timing_original_sample_ns) >= mock_original_cost_ns);
    CHECK(atomic_load(&timing_original_sample_ns) < 2 * mock_original_cost_ns);
    CHECK(atomic_load(&timing_base_self_ns) == 0 &&
          atomic_load(&timing_eye_self_ns) == 0);
    // Both the expected and actual original mock call read_camera; neither is
    // wrapper work, and the first binding's prewindow checks were discarded.
    CHECK(atomic_load(&timing_memory_calls[LABO_GUEST_QUERY]) == 0 &&
          atomic_load(&timing_memory_calls[LABO_GUEST_READ]) == 0 &&
          atomic_load(&timing_memory_calls[LABO_GUEST_WRITE]) == 0 &&
          atomic_load(&timing_memory_calls[LABO_GUEST_WORKING_SET]) == 0);
    set_pose(0, 0, 0, 1, 11);
    restore_eye();
    call_builder(base, base + 8, BASE_CALLER);
    call_builder(eye, eye + 8, EYE_CALLER);
    CHECK(atomic_load(&timing_generic) == 66);
    CHECK(atomic_load(&timing_base_candidates) == 1 &&
          atomic_load(&timing_eye_candidates) == 1);
    CHECK(atomic_load(&timing_bound_base) == 1 &&
          atomic_load(&timing_bound_eye) == 1);
    CHECK(atomic_load(&timing_base_self_ns) > 0 &&
          atomic_load(&timing_eye_self_ns) > 0);
    CHECK(atomic_load(&timing_original_samples) == 2); // Calls 0 and 64.
    CHECK(atomic_load(&timing_memory_calls[LABO_GUEST_QUERY]) == 0);
    CHECK(atomic_load(&timing_memory_calls[LABO_GUEST_WORKING_SET]) == 17);
    CHECK(atomic_load(&timing_memory_calls[LABO_GUEST_READ]) == 16);
    CHECK(atomic_load(&timing_memory_calls[LABO_GUEST_WRITE]) == 1);
    CHECK(atomic_load(&timing_memory_ns[LABO_GUEST_QUERY]) == 0);
    CHECK(atomic_load(&timing_memory_ns[LABO_GUEST_WORKING_SET]) == 1700);
    CHECK(atomic_load(&timing_memory_ns[LABO_GUEST_READ]) == 1600);
    CHECK(atomic_load(&timing_memory_ns[LABO_GUEST_WRITE]) == 100);
    CHECK(!atomic_load(&timing_guard_rejected) && !timing_memory_scope);
    mock_ns += UINT64_C(1000000000);
    pc_vr_head_look_observe_controller(controller);
    CHECK(timing_windows == 1 && atomic_load(&timing_generic) == 0);
    CHECK(atomic_load(&timing_base_self_ns) == 0 &&
          atomic_load(&timing_eye_self_ns) == 0);
    for (unsigned i = 0; i < LABO_GUEST_TIMING_OPERATIONS; ++i)
        CHECK(!atomic_load(&timing_memory_calls[i]) &&
              !atomic_load(&timing_memory_ns[i]) && !atomic_load(&timing_memory_failed[i]));

    // Failed APIs and early guard rejection still contribute to attribution.
    // Guards must remain in place; the diagnostic never copies rejected pages.
    uint64_t scratch = 0;
    timing_memory_scope = true;
    CHECK(!labo_guest_copy(GUEST_HI, &scratch, sizeof scratch, false));
    CHECK(atomic_load(&timing_guard_rejected) == 1 &&
          !atomic_load(&timing_memory_calls[LABO_GUEST_QUERY]));
    fail_query = fail_working_set = true;
    CHECK(!labo_guest_copy(base, &scratch, sizeof scratch, false));
    fail_query = fail_working_set = false;
    CHECK(atomic_load(&timing_memory_calls[LABO_GUEST_QUERY]) == 1 &&
          atomic_load(&timing_memory_failed[LABO_GUEST_QUERY]) == 1 &&
          atomic_load(&timing_guard_rejected) == 2);
    CHECK(atomic_load(&timing_memory_calls[LABO_GUEST_WORKING_SET]) == 1 &&
          atomic_load(&timing_memory_failed[LABO_GUEST_WORKING_SET]) == 1);
    fail_read = true;
    CHECK(!labo_guest_copy(base, &scratch, sizeof scratch, false));
    fail_read = false;
    CHECK(atomic_load(&timing_memory_calls[LABO_GUEST_READ]) == 1 &&
          atomic_load(&timing_memory_failed[LABO_GUEST_READ]) == 1);
    fail_write = true;
    CHECK(!labo_guest_copy(base, &scratch, sizeof scratch, true));
    fail_write = false;
    CHECK(atomic_load(&timing_memory_calls[LABO_GUEST_WRITE]) == 1 &&
          atomic_load(&timing_memory_failed[LABO_GUEST_WRITE]) == 1);
    const uint64_t guard_page = MAIN_BASE + UINT64_C(0x22004000);
    DWORD prior_protect;
    MEMORY_BASIC_INFORMATION guard_info;
    CHECK(VirtualProtect((void *)(uintptr_t)guard_page, 0x1000,
                         PAGE_READWRITE | PAGE_GUARD, &prior_protect));
    CHECK(!labo_guest_copy(guard_page, &scratch, sizeof scratch, false));
    CHECK(VirtualQuery((void *)(uintptr_t)guard_page, &guard_info, sizeof guard_info) == sizeof guard_info &&
          (guard_info.Protect & PAGE_GUARD));
    CHECK(VirtualProtect((void *)(uintptr_t)guard_page, 0x1000,
                         PAGE_NOACCESS, &prior_protect));
    CHECK(!labo_guest_copy(guard_page, &scratch, sizeof scratch, true));
    CHECK(VirtualFree((void *)(uintptr_t)guard_page, 0x1000, MEM_DECOMMIT));
    CHECK(!labo_guest_copy(guard_page, &scratch, sizeof scratch, false));
    CHECK(atomic_load(&timing_memory_calls[LABO_GUEST_QUERY]) == 4 &&
          atomic_load(&timing_memory_calls[LABO_GUEST_WORKING_SET]) == 6 &&
          atomic_load(&timing_memory_calls[LABO_GUEST_READ]) == 1 &&
          atomic_load(&timing_memory_calls[LABO_GUEST_WRITE]) == 1 &&
          atomic_load(&timing_guard_rejected) == 5);
    timing_memory_scope = false;
    timing_windows = SUMMARY_CAP;
    mock_ns += UINT64_C(1000000000);
    pc_vr_head_look_observe_controller(controller);
    CHECK(timing_windows == SUMMARY_CAP);
    pc_vr_head_look_reset();
    CHECK(!atomic_load(&timing_enabled) && !timing_active &&
          atomic_load(&timing_generic) == 0);
    for (unsigned i = 0; i < LABO_GUEST_TIMING_OPERATIONS; ++i)
        CHECK(!atomic_load(&timing_memory_calls[i]) &&
              !atomic_load(&timing_memory_ns[i]) && !atomic_load(&timing_memory_failed[i]));
    CHECK(!atomic_load(&timing_guard_rejected) && !timing_memory_scope);
    mock_clock_step = mock_original_cost_ns = 0;
    set_timing_optin(NULL);

    // Native integration changes only the opted-in branch. A skipped native
    // view still executes legacy composition; applied native fields never receive
    // that composition a second time, and post acknowledgement is after the
    // original call with its CPU effects preserved exactly once.
    pc_vr_head_look_init(); pc_vr_head_look_observe_controller(controller);
    write_camera(base, &base_initial); restore_eye();
    set_pose(0, 0, 0, 1, 20);
    call_builder(base, base + 8, BASE_CALLER);
    mock_native_mode = PC_VR_NATIVE_VIEW_SKIPPED;
    const unsigned initial_acks = legacy_applied_acks;
    call_builder(eye, eye + 8, EYE_CALLER);
    CHECK(native_saw_reference && native_prepare_calls == 1 && !native_post_calls);
    CHECK(reference_valid && legacy_applied_acks == initial_acks + 1);
    set_pose(0, 0.70710678f, 0, 0.70710678f, 20);
    call_builder(base, base + 8, BASE_CALLER); restore_eye();
    mock_native_mode = PC_VR_NATIVE_VIEW_APPLIED;
    call_builder(eye, eye + 8, EYE_CALLER);
    PcVrHeadLookCamera delivered = camera_at(eye);
    CHECK(native_saw_reference && native_post_calls == 1 && !native_pending);
    CHECK(!memcmp(&delivered, &native_delivered, sizeof delivered));
    CHECK(legacy_applied_acks == initial_acks + 1 && latch_valid);

    // A rejected preparation writes no native fields and does not silently
    // compose a legacy camera while claiming a native result.
    call_builder(base, base + 8, BASE_CALLER); restore_eye();
    mock_native_mode = PC_VR_NATIVE_VIEW_REJECTED;
    call_builder(eye, eye + 8, EYE_CALLER); delivered = camera_at(eye);
    CHECK(!memcmp(&delivered, &eye_initial, sizeof delivered));
    CHECK(!latch_valid && native_post_calls == 1 && legacy_applied_acks == initial_acks + 1);

    // Failed actual view and a calibration change across the original call
    // invalidate the latch; neither invokes the original a second time.
    call_builder(base, base + 8, BASE_CALLER); restore_eye();
    mock_native_mode = PC_VR_NATIVE_VIEW_APPLIED; mock_native_post_ok = false;
    call_builder(eye, eye + 8, EYE_CALLER);
    CHECK(!latch_valid && native_post_calls == 2);
    mock_native_post_ok = true;
    call_builder(base, base + 8, BASE_CALLER); restore_eye();
    mock_change_calibration = true;
    call_builder(eye, eye + 8, EYE_CALLER);
    CHECK(!latch_valid && native_post_calls == 3 && !native_pending);
    mock_change_calibration = false; mock_native_mode = PC_VR_NATIVE_VIEW_LEGACY;
    CHECK(VirtualFree((void *)(uintptr_t)(MAIN_BASE + UINT64_C(0x22000000)),
                      0, MEM_RELEASE));
#endif

    pc_vr_head_look_reset();
    CHECK(!pc_vr_head_look_enabled() && !reference_valid && !binding_valid);
    pc_vr_head_look_init();
    CHECK(pc_vr_head_look_enabled() && override_calls == 1 && lookup_calls == 1);
    printf("pc vr head look: %u checks passed\n", checks);
    return 0;
}

// hle/labo_guest_memory.h: a new access generation per call keeps its page cache cold here.
uint64_t plat_guest_access_generation(void) { static uint64_t generation; return ++generation; }
bool plat_data_watch(const uint64_t addr[4]) { (void)addr; return false; }
