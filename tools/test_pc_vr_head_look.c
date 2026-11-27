// Asset-free head-look contract (D115 step 5c). Includes the implementation so the mocked dispatch, XR pose, frame
// record and guest pages exercise the real wrapper: the binding, the fresh-copy test, the frame's pose (held, D101),
// what the frame record is told and when, and the original builder run exactly once on every call.
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
static uint64_t mock_ns = UINT64_C(1000000000000000);
static XrPoseReadResult mock_pose_result = XR_POSE_VALID;
static XrPoseSnapshot mock_pose;
static uint64_t mock_local_definition = 1, mock_calibration_epoch = 1;
static uint32_t module_count = 1;
static GameLibModule game_module = {.name = "main", .base = MAIN_BASE, .size = MAIN_SIZE,
    .build_id = "de0b6ac4efc1dda7f80f6d7598ec7a7d00000000000000000000000000000000"};
static GameLibTables game_tables = {.modules = &game_module, .nmodules = &module_count};
static Module loaded_main = {.name = "main", .base = MAIN_BASE, .size = MAIN_SIZE};
static uint64_t override_address;
static HostFn override_function;
static PcVrHeadLookCamera original_seen;

// The mocks' record: how often each was called, with what, and when (one order stamp across all of them).
static uint64_t mock_order, mock_frame_pointer;
static uint64_t original_order;
static unsigned trace_init_calls;
static unsigned root_calls;
static uint64_t root_frame_pointer;
static unsigned invalidate_calls;
static uint64_t invalidate_camera, invalidate_order;
static unsigned eye_layer_calls;
static uint64_t eye_layer_layer, eye_layer_frame_pointer, eye_layer_order;
static bool mock_root, mock_trace_result = true;
typedef struct {
    unsigned calls;
    uint64_t layer, base, frame_pointer, order;
    PcVrHeadLookCamera base_camera;
    bool fresh, pose_given;
    XrPoseSnapshot pose;
} TraceEyeRecord;
static TraceEyeRecord trace_eye;
static unsigned eye_built_calls;
static uint64_t eye_built_layer, eye_built_order;

bool pc_vr_enabled(void) { return mock_pc_vr; }
bool pc_vr_main_compatible(void) { return pc_vr_main_layout(gamelib_tables(), loader_module("main")); }
void pc_vr_projection_trace_init(void) { ++trace_init_calls; }
void pc_vr_listener_init(void) {}  // (the sound listener: runtime/hle/pc_vr_listener.c, its math its own fixture)
void pc_vr_sound_hrtf_init(void) {}  // (sounds rendered binaurally: runtime/hle/pc_vr_sound_hrtf.c, likewise)
bool pc_vr_pace_pose_wait_on(void) { return false; }  // (the pose wait experiment: off here)
void pc_vr_pace_pose_wait(void) {}
bool pc_vr_projection_trace_root(uint64_t guest_frame_pointer) {
    ++root_calls;
    root_frame_pointer = guest_frame_pointer;
    return mock_root;
}
void pc_vr_projection_trace_eye_layer(uint64_t layer, uint64_t guest_frame_pointer) {
    ++eye_layer_calls;
    eye_layer_layer = layer;
    eye_layer_frame_pointer = guest_frame_pointer;
    eye_layer_order = ++mock_order;
}
void pc_vr_render_trace_invalidate_camera(uint64_t camera) {
    ++invalidate_calls;
    invalidate_camera = camera;
    invalidate_order = ++mock_order;
}
bool pc_vr_projection_trace_eye(uint64_t layer, uint64_t base, const PcVrHeadLookCamera *base_camera, bool fresh,
    const XrPoseSnapshot *pose, uint64_t guest_frame_pointer) {
    ++trace_eye.calls;
    trace_eye.layer = layer;
    trace_eye.base = base;
    trace_eye.base_camera = *base_camera;
    trace_eye.fresh = fresh;
    trace_eye.pose_given = pose != NULL;
    if (pose) trace_eye.pose = *pose;
    else memset(&trace_eye.pose, 0, sizeof trace_eye.pose);
    trace_eye.frame_pointer = guest_frame_pointer;
    trace_eye.order = ++mock_order;
    return mock_trace_result;
}
void pc_vr_projection_trace_eye_built(uint64_t layer) {
    ++eye_built_calls;
    eye_built_layer = layer;
    eye_built_order = ++mock_order;
}
const GameLibTables *gamelib_tables(void) { return &game_tables; }
Module *loader_module(const char *name) { return !strcmp(name, "main") ? &loaded_main : NULL; }
uint64_t plat_now_ns(void) {
    return mock_ns;
}
XrPoseReadResult xr_runtime_get_pose(XrPoseSnapshot *out) {
    ++pose_reads;
    memcpy(out, &mock_pose, sizeof *out);  // (as the runtime: an unavailable read still carries the publication)
    return mock_pose_result;
}
static void mock_original(Cpu *c) {
    ++original_calls;
    original_order = ++mock_order;
    if (valid_camera_address(c->x[0])) (void)read_camera(c->x[0], &original_seen);
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
// A usable published pose (every flag head look asks for set), told apart by its frame id, in the current LOCAL
// definition and calibration epoch.
static void set_pose(uint64_t frame_id) {
    mock_pose_result = XR_POSE_VALID;
    memset(&mock_pose, 0, sizeof mock_pose);
    const float half_yaw = 0.01f * (float)frame_id;
    mock_pose.orientation_xyzw[1] = sinf(half_yaw);
    mock_pose.orientation_xyzw[3] = cosf(half_yaw);
    mock_pose.frame_id = frame_id;
    mock_pose.generation = 1 + frame_id / 16;
    mock_pose.local_definition = mock_local_definition;
    mock_pose.published_monotonic_ns = UINT64_C(1000) * frame_id;
    mock_pose.active = mock_pose.focused = mock_pose.orientation_valid = mock_pose.orientation_tracked =
        mock_pose.views_valid = mock_pose.native_views_valid = true;
    mock_pose.calibration = (XrCalibration){.epoch = mock_calibration_epoch, .heading_xyzw = {0, 0, 0, 1}};
}

static bool same_pose(const XrPoseSnapshot *first, const XrPoseSnapshot *second) {
    return first->frame_id == second->frame_id && first->generation == second->generation &&
        first->local_definition == second->local_definition &&
        first->published_monotonic_ns == second->published_monotonic_ns &&
        !memcmp(first->orientation_xyzw, second->orientation_xyzw, sizeof first->orientation_xyzw) &&
        first->active == second->active && first->orientation_valid == second->orientation_valid &&
        first->orientation_tracked == second->orientation_tracked && first->views_valid == second->views_valid &&
        first->native_views_valid == second->native_views_valid &&
        first->calibration.epoch == second->calibration.epoch;
}

static bool same_camera(const PcVrHeadLookCamera *first, const PcVrHeadLookCamera *second) {
    return !memcmp(first, second, sizeof *first);
}

// One call of the wrapped builder, with the invariants every call keeps: the original runs exactly once with its CPU
// effects; the render trace forgets the camera once, first; the eye layer is named at most once, before the frame
// record; the frame record is asked at most once, before the original; eye_built follows the original exactly when
// the frame record said yes.
static void call_builder(uint64_t object, uint64_t output, uint64_t caller) {
    Cpu actual = {0}, expected = {0};
    actual.x[0] = expected.x[0] = object;
    actual.x[1] = expected.x[1] = output;
    actual.x[5] = expected.x[5] = 3;
    actual.x[29] = expected.x[29] = mock_frame_pointer;
    actual.x[30] = expected.x[30] = caller;
    actual.sp = expected.sp = 0x1000;
    mock_original(&expected);
    const unsigned prior_originals = original_calls, prior_invalidations = invalidate_calls,
        prior_layers = eye_layer_calls, prior_traces = trace_eye.calls, prior_built = eye_built_calls;
    const uint64_t call_start = mock_order;
    override_function(&actual);
    CHECK(original_calls == prior_originals + 1);
    CHECK(!memcmp(&actual, &expected, sizeof actual));
    CHECK(invalidate_calls == prior_invalidations + 1 && invalidate_camera == object);
    CHECK(invalidate_order > call_start && invalidate_order < original_order);
    const unsigned layer_calls = eye_layer_calls - prior_layers, trace_calls = trace_eye.calls - prior_traces,
        built_calls = eye_built_calls - prior_built;
    CHECK(layer_calls <= 1 && trace_calls <= 1);
    if (layer_calls)
        CHECK(eye_layer_order > invalidate_order && eye_layer_order < original_order &&
              eye_layer_layer == object - LAYER_EYE_CAMERA && eye_layer_frame_pointer == mock_frame_pointer);
    if (trace_calls)
        CHECK(layer_calls && trace_eye.order > eye_layer_order && trace_eye.order < original_order &&
              trace_eye.layer == object - LAYER_EYE_CAMERA && trace_eye.frame_pointer == mock_frame_pointer);
    CHECK(built_calls == (trace_calls && mock_trace_result ? 1u : 0u));
    if (built_calls) CHECK(eye_built_order > original_order && eye_built_layer == object - LAYER_EYE_CAMERA);
}

#ifdef _WIN32
static uint64_t controller, weak, actor, base, other_base, layer, eye, layer2, eye2;
static PcVrHeadLookCamera base_initial, eye_initial, eye2_initial;
static void write_u64(uint64_t at, uint64_t value) { *(uint64_t *)(uintptr_t)at = value; }
static void write_camera(uint64_t at, const PcVrHeadLookCamera *value) {
    memcpy((void *)(uintptr_t)(at + 0x38), value, sizeof *value);
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
// An eye camera builder call (EYE_CALLER), at the frame's root or its sibling.
static void eye_call(uint64_t eye_object, bool root) {
    mock_root = root;
    call_builder(eye_object, eye_object + 8, EYE_CALLER);
}
#endif

int main(void) {
    // Without PC VR, no head look (with it, head look is part of the play configuration: no switch of its own turns it
    // off).
    test_pc_vr_configure((const char *const[]){"vr", "1", NULL});
    mock_pc_vr = false;
    pc_vr_head_look_init();
    CHECK(!pc_vr_head_look_enabled() && !registered && !override_calls && !trace_init_calls);
    mock_pc_vr = true;
    // A game version without PC VR's sites, or a main module of another layout: not armed.
    const uint64_t builder = g_site.pcvr_matrix_builder;
    g_site.pcvr_matrix_builder = 0;
    pc_vr_head_look_init();
    CHECK(!pc_vr_head_look_enabled() && !override_calls);
    g_site.pcvr_matrix_builder = builder;
    loaded_main.size--;
    pc_vr_head_look_init();
    CHECK(!pc_vr_head_look_enabled() && !override_calls);
    loaded_main.size++;
    // Armed: the builder wrapped once, the frame record started.
    pc_vr_head_look_init();
    CHECK(pc_vr_head_look_enabled() && registered && override_calls == 1 && trace_init_calls == 1);
    CHECK(override_address == BUILDER && override_function == head_look_builder);
    CHECK(lookup_calls == 1);

#ifdef _WIN32
    setup_guest();
    mock_frame_pointer = UINT64_C(0x7100001000);
    set_pose(1);
    // No binding yet: each eye builder call still names its layer to the frame record (eye_layer), and nothing else is
    // asked.
    eye_call(eye, true);
    CHECK(eye_layer_calls == 1 && eye_layer_layer == layer);
    eye_call(eye2, false);
    CHECK(eye_layer_calls == 2 && eye_layer_layer == layer2);
    CHECK(!trace_eye.calls && !eye_built_calls && !pose_reads && !root_calls);
    // Not an eye call: another caller, or an output that is not the camera's matrix. No eye layer named.
    call_builder(eye, eye + 8, MAIN_BASE + 0x9999);
    call_builder(eye, eye + 16, EYE_CALLER);
    CHECK(eye_layer_calls == 2 && !trace_eye.calls);
    pc_vr_head_look_observe_controller(controller);
    CHECK(binding_valid && binding.base == base && binding.actor == actor && binding.weak == weak);

    // Root, fresh, but no usable pose yet in this LOCAL definition: the frame record is asked with none.
    mock_pose.active = false;
    eye_call(eye, true);
    CHECK(trace_eye.calls == 1 && trace_eye.fresh && !trace_eye.pose_given && root_frame_pointer == mock_frame_pointer);

    // Root and fresh: the frame record gets the published usable pose, the bound base and the base as read now.
    set_pose(2);
    mock_frame_pointer += 0x100;
    unsigned reads_before = pose_reads;
    eye_call(eye, true);
    CHECK(trace_eye.calls == 2 && trace_eye.layer == layer && trace_eye.base == base && trace_eye.fresh);
    CHECK(trace_eye.pose_given && same_pose(&trace_eye.pose, &mock_pose) && pose_reads == reads_before + 1);
    CHECK(same_camera(&trace_eye.base_camera, &base_initial) && root_frame_pointer == mock_frame_pointer);
    CHECK(eye_built_calls == 2 && eye_built_layer == layer && held_valid);
    // The sibling (not the root): no pose, not even a read of the published one, which has moved on.
    set_pose(3);
    reads_before = pose_reads;
    eye_call(eye2, false);
    CHECK(trace_eye.calls == 3 && trace_eye.layer == layer2 && trace_eye.fresh && !trace_eye.pose_given);
    CHECK(pose_reads == reads_before && eye_built_calls == 3 && eye_built_layer == layer2);
    // Not a fresh copy at the root (the game reused the eye: its view no longer the base's, or a wrong vtable): the
    // frame record is told so, with no pose and no pose read.
    write_camera(eye, &(PcVrHeadLookCamera){.position = {0.03f, 2, 5}, .target = {1.03f, 2, 0}, .up = {0, 1, 0}});
    reads_before = pose_reads;
    eye_call(eye, true);
    CHECK(trace_eye.calls == 4 && !trace_eye.fresh && !trace_eye.pose_given && pose_reads == reads_before);
    restore_eye();
    write_u64(eye, EYE_VTABLE + 8);
    eye_call(eye, true);
    CHECK(trace_eye.calls == 5 && !trace_eye.fresh && !trace_eye.pose_given && pose_reads == reads_before);
    write_u64(eye, EYE_VTABLE);
    // The world-scale rounding of separately translated float positions still passes the fresh-copy test; a changed
    // view does not.
    PcVrHeadLookCamera far_base = {.position = {4000.0f, 2, 4005.0f},
        .target = {4000.0f, 2, 4000.0f}, .up = {0, 1, 0}};
    PcVrHeadLookCamera far_eye = {.position = {4000.03003f, 2, 4005.03003f},
        .target = {4000.03003f, 2, 4000.03003f}, .up = {0, 1, 0}};
    CHECK(freshly_copied_eye(&far_base, &far_eye));
    far_eye.target[0] += 0.1f;
    CHECK(!freshly_copied_eye(&far_base, &far_eye));
    // The frame record says no: rejected, no eye_built; the original still runs once (call_builder).
    mock_trace_result = false;
    eye_call(eye, true);
    CHECK(trace_eye.calls == 6 && trace_eye.fresh && trace_eye.pose_given && eye_built_calls == 5);
    mock_trace_result = true;
    // An unrelated layer (its source camera not the bound base): its layer named, nothing asked.
    write_u64(layer2 + 0x80, other_base);
    const unsigned layers_before = eye_layer_calls;
    eye_call(eye2, false);
    CHECK(eye_layer_calls == layers_before + 1 && eye_layer_layer == layer2);
    CHECK(trace_eye.calls == 6 && eye_built_calls == 5);
    write_u64(layer2 + 0x80, base);
    // The bound chain no longer reads as bound (the actor's vtable changed): rejected without asking the frame record
    // or reading the pose.
    write_u64(actor, ACTOR_VTABLE + 8);
    reads_before = pose_reads;
    eye_call(eye, true);
    CHECK(trace_eye.calls == 6 && eye_built_calls == 5 && pose_reads == reads_before);
    write_u64(actor, ACTOR_VTABLE);

    // D101: a published pose that becomes unusable (each flag in turn, then an unavailable read) leaves the frame
    // record the last usable one while LOCAL keeps its definition.
    set_pose(10);
    eye_call(eye, true);
    const XrPoseSnapshot last_usable = mock_pose;
    CHECK(trace_eye.pose_given && same_pose(&trace_eye.pose, &last_usable));
    for (unsigned mode = 0; mode < 9; ++mode) {
        set_pose(11 + mode);
        if (mode == 0) mock_pose.active = false;
        if (mode == 1) mock_pose.orientation_valid = false;
        if (mode == 2) mock_pose.orientation_tracked = false;
        if (mode == 3) mock_pose.views_valid = false;
        if (mode == 4) mock_pose.native_views_valid = false;
        if (mode == 5) mock_pose.generation = 0;
        if (mode == 6) mock_pose.frame_id = 0;
        if (mode == 7) mock_pose.calibration.epoch = 0;
        if (mode == 8) mock_pose_result = XR_POSE_UNAVAILABLE;
        eye_call(eye, true);
        CHECK(trace_eye.pose_given && same_pose(&trace_eye.pose, &last_usable));
    }
    // LOCAL redefined (a recenter, a new session) while the publication is unusable (here before the XR core's
    // calibration in it): no pose, and again none on the next root.
    ++mock_local_definition;
    set_pose(30);
    mock_pose.calibration.epoch = 0;
    eye_call(eye, true);
    CHECK(!trace_eye.pose_given && !held_valid);
    eye_call(eye, true);
    CHECK(!trace_eye.pose_given);
    // A new usable pose is taken, and becomes the one held.
    set_pose(31);
    const XrPoseSnapshot new_usable = mock_pose;
    eye_call(eye, true);
    CHECK(trace_eye.pose_given && same_pose(&trace_eye.pose, &new_usable));
    set_pose(32);
    mock_pose_result = XR_POSE_UNAVAILABLE;
    eye_call(eye, true);
    CHECK(trace_eye.pose_given && same_pose(&trace_eye.pose, &new_usable));

    // A camera rebinding (the actor's base camera changes: a new section) keeps the held pose.
    write_u64(actor + 0x1088, other_base);
    pc_vr_head_look_observe_controller(controller);
    CHECK(binding_valid && binding.base == other_base && held_valid);
    write_u64(layer + 0x80, other_base);
    eye_call(eye, true);
    CHECK(trace_eye.base == other_base && trace_eye.fresh);
    CHECK(trace_eye.pose_given && same_pose(&trace_eye.pose, &new_usable));
    write_u64(actor + 0x1088, base);
    write_u64(layer + 0x80, base);
    pc_vr_head_look_observe_controller(controller);
    CHECK(binding_valid && binding.base == base && held_valid);
    set_pose(33);

    // Guest reads the guards refuse copy nothing: above the guest range, a failed query or working-set lookup, a failed
    // read or write, a guard page, a page without access, decommitted memory.
    pc_vr_head_look_reset();
    uint64_t scratch = 0;
    CHECK(!labo_guest_copy(GUEST_HI, &scratch, sizeof scratch, false));
    fail_query = fail_working_set = true;
    CHECK(!labo_guest_copy(base, &scratch, sizeof scratch, false));
    fail_query = fail_working_set = false;
    fail_read = true;
    CHECK(!labo_guest_copy(base, &scratch, sizeof scratch, false));
    fail_read = false;
    fail_write = true;
    CHECK(!labo_guest_copy(base, &scratch, sizeof scratch, true));
    fail_write = false;
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

    // After a reset head look is off: an eye call only runs the original (call_builder), names no layer and asks
    // nothing; the binding is gone.
    CHECK(!pc_vr_head_look_enabled() && !binding_valid);
    const unsigned layers_after_reset = eye_layer_calls, traces_after_reset = trace_eye.calls;
    eye_call(eye, true);
    CHECK(eye_layer_calls == layers_after_reset && trace_eye.calls == traces_after_reset);
    CHECK(VirtualFree((void *)(uintptr_t)(MAIN_BASE + UINT64_C(0x22000000)),
                      0, MEM_RELEASE));
#endif

    // Reset and init again: off, then armed without wrapping the builder a second time.
    pc_vr_head_look_reset();
    CHECK(!pc_vr_head_look_enabled() && !binding_valid);
    pc_vr_head_look_init();
    CHECK(pc_vr_head_look_enabled() && override_calls == 1 && lookup_calls == 1);
    printf("pc vr head look: %u checks passed\n", checks);
    return 0;
}

// hle/labo_guest_memory.h: a new access generation per call keeps its page cache cold here.
uint64_t plat_guest_access_generation(void) { static uint64_t generation; return ++generation; }
bool plat_data_watch(const uint64_t addr[4]) { (void)addr; return false; }
