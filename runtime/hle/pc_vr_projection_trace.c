// PC VR native eye views and render certificates (D042, D043, D048; one frame record, D115 step 5c).
//
//  1. The frame record. The game renders its two Labo eye layers in one call chain: a root layer (one eye) and its
//     sibling (the other), told apart by their saved frame pointers and return addresses (PROJ_ROOT_RETURN,
//     PROJ_SIBLING_RETURN). Head look calls pc_vr_projection_trace_eye for each, before the game's look-at builder
//     runs: the root's call starts the record with the base camera and the frame's pose, and builds both eyes' views
//     from them (pc_vr_native_view_build_pair); each call writes its eye's view into the eye camera. The same stack
//     and link tests on the display's calls alone, before head look has bound the game's camera, make the two eye
//     layers known (projection_display_pair), so that a layer's draws are told an eye's or another layer's from the
//     first frame after a load (render_scope_eye_layer). Only until the record knows an eye layer: the display also
//     shows the pause menu's own pair of layers through the same calls.
//  2. The native FOV. On the game's projection builder for an eye of the record, its parameters become the eye's own
//     asymmetric FOV (projection_write_native_fov; D043). The game's original logical and device writers always run,
//     once; the logical matrix is checked to give the eye's frustum (projection_logical_built).
//  3. Render certificates. The render layer's begin and end are wrapped (render_begin_wrapper, render_end_wrapper)
//     to know which eye layer the current thread is rendering. When the command recorder records a draw
//     (pc_vr_render_trace_record), a draw in an eye layer whose view and FOV are as written gets that eye's
//     certificate id: which eye, which record (frame), which pose. The eye-image producer admits eye images by it.
//
// The PC VR play configuration (runtime/pc_vr_config.h) runs all of it, continuously, at its world scale
// (pc_vr_projection_trace_init). An eye whose view or FOV could not be given this frame has no certificate, and the
// next frame starts afresh; why is counted (pc_vr_guards.h). Logs carry metadata only, never game data.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "../gamelib.h"
#include "../game_sites.h"
#include "../hle.h"
#include "../pc_vr.h"
#include "../pc_vr_guards.h"
#include "../xr/xr_runtime.h"
#include "labo_guest_memory.h"
#include "pc_vr_projection_math.h"
#include "pc_vr_projection_trace.h"
#include "pc_vr_render_trace.h"

#define PROJ_BUILDER g_site.pcvr_proj_builder
#define PROJ_DEVICE g_site.pcvr_proj_device
#define PROJ_BUILD_CALLER g_site.pcvr_proj_build_caller
#define PROJ_DEVICE_CALLER g_site.pcvr_proj_device_caller
#define PROJ_VTABLE g_site.pcvr_proj_vtable
#define PROJ_EYE_VTABLE g_site.pcvr_eye_vtable
#define PROJ_BASE_VTABLE g_site.pcvr_base_vtable
#define PROJ_ROOT_RETURN g_site.pcvr_proj_root_return
#define PROJ_SIBLING_RETURN g_site.pcvr_proj_sibling_return
#define RENDER_BEGIN g_site.pcvr_render_begin
// The layer's end: no code calls it directly (vtable +0x68), and it follows begin (vtable +0x50) at +0x58 in 1.8.2 and
// 1.9.0 alike, the same 4 instructions.
#define RENDER_END (g_site.pcvr_render_begin + 0x58)
#define RENDER_BEGIN_CALLER g_site.pcvr_render_begin_caller
#define RENDER_SCOPE_CAP 8          // a thread's open render scopes (RenderScopes)
#define RENDER_CERTIFICATE_SLOTS 64 // the certificate store: a slot is reused after 64 newer certificates

// One eye of the frame record: its layer, and what its certificate carries. Replaced at that eye's next call, so a
// draw of this frame recorded after the next frame's other eye call still finds it.
typedef struct {
    uint64_t layer;           // 0: not an eye layer of the bound composition (yet)
    uint64_t serial;          // the record (frame) it was written in
    pthread_t thread;         // the camera thread; the eye's projection builder runs on it
    unsigned native_index;    // the runtime eye: 0 left, 1 right
    double units_per_meter;   // the world scale its view was built at
    XrPoseSnapshot pose;      // the frame's pose: the eye's FOV, and the certificate's snapshot
    bool view_written, fov_attempted, fov_written, logical_ok;
    // Whether the eye's camera, logical and device projections are as written and checked; a builder call for the
    // eye clears them (pc_vr_render_trace_invalidate_camera, render_invalidate_projection).
    bool view_current, logical_current, device_current;
    uint64_t certificate;
} RecordEye;

// The frame record: the root's call, on its thread and frame, with the base camera it copies, the frame's pose and
// the two eyes' views built from them; open until its sibling's call. Under projection_lock.
typedef struct {
    uint64_t serial;          // 0: none yet; never reused (projection_root_serial)
    bool open;
    pthread_t thread;
    uint64_t frame_pointer, root_layer, sibling, config, base;
    unsigned root_side;
    int root_offset_sign;     // which way the root's eye sits along the base camera's right
    PcVrHeadLookCamera base_camera;
    XrPoseSnapshot pose;
    PcVrNativeViewPair pair;
    RecordEye eyes[2];        // by side (the layer's +0x90 flag 0x1000)
} FrameRecord;

// The record, the published certificates and the display's pair change only under it.
static pthread_mutex_t projection_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool projection_enabled;
static bool projection_registered;
static GuestFn projection_original, projection_device_original;
static FrameRecord projection_record;
// The record's eye layers (0: none), kept with it under projection_lock and read without it: the game builds every
// camera and projection through the same two builders, and one that is not an eye's takes no lock
// (pc_vr_render_trace_invalidate_camera, render_invalidate_projection).
static _Atomic uint64_t record_eye_layers[2];
static void record_eye_layers_update_locked(void) {
    for (unsigned side = 0; side < 2; ++side)
        atomic_store_explicit(&record_eye_layers[side], projection_record.eyes[side].layer, memory_order_release);
}
static bool record_eye_layer(uint64_t layer) {
    return layer && (atomic_load_explicit(&record_eye_layers[0], memory_order_acquire) == layer ||
                     atomic_load_explicit(&record_eye_layers[1], memory_order_acquire) == layer);
}
static bool record_eyes_known_locked(void) {
    return projection_record.eyes[0].layer || projection_record.eyes[1].layer;
}
// The record's eye drawn through this layer's projection object (NULL: none).
static RecordEye *record_eye_of_projection_locked(uint64_t object) {
    for (unsigned side = 0; side < 2; ++side)
        if (projection_record.eyes[side].layer && projection_record.eyes[side].layer + 0x288 == object)
            return &projection_record.eyes[side];
    return NULL;
}
// The Labo display's pair of eye layers, known from its own calls for them (pc_vr_projection_trace_eye_layer): the
// root call for one eye layer, then the sibling call for the other, by the same stack and link tests as the record's,
// without head look's camera binding. It tells the eye layers' draws from other layers' while the record knows no eye
// layer yet (render_scope_eye_layer). Under projection_lock; cleared at each arming and at the game reset.
typedef struct {
    // The root call seen and its sibling's not yet: the root's layer, configuration, side, frame pointer and thread.
    bool open;
    uint64_t root_layer, root_config, root_frame_pointer;
    unsigned root_side;
    pthread_t root_thread;
    // The pair recognised last, by side (the layer's +0x90 flag 0x1000); both 0: none known.
    uint64_t layers[2];
} ProjectionDisplayPair;
static ProjectionDisplayPair projection_display_pair;
static uint64_t projection_display_pairs;  // the pairs recognised that differed from the one before (for the log)
// Never reset: reused guest stack addresses and render-layer objects are not frame identities. Zero is no record.
static uint64_t projection_root_serial;
// The counts that bound the records' and the refusals' log lines to the first few.
static uint64_t projection_records, projection_refused;
static double projection_view_scale, projection_launch_scale; // (live: pc_vr_world_scale)
static _Atomic double projection_focus_m; // the camera's target distance at the last built pair (D057)

// Immutable append-only storage outlives observation and queued command lists, reset and re-arm included. A slot is
// reused after 64 newer certificates; its ID field acts as a sequence lock.
static PcVrRenderCertificate render_certificates[RENDER_CERTIFICATE_SLOTS];
static atomic_uint render_certificate_count;
static atomic_bool render_enabled, render_collecting;
static atomic_uint_fast64_t render_epoch;
static GuestFn render_begin_original, render_end_original;
// One render layer being rendered on this thread (between the layer's begin and end): the layer, its render info,
// context and command buffer, its camera and projection.
typedef struct {
    uint64_t layer, rinfo, context, cb, camera, projection;
    uint32_t pass;
    bool readable, identity;
} RenderScope;
// This thread's stack of open render scopes (layers render inside one another). poisoned: the stack overflowed or a
// begin and end did not match, so no draw on this thread gets a certificate until render_epoch changes (the trace is
// armed again, or the game resets), which clears the stack.
typedef struct {
    uint64_t epoch;
    unsigned depth;
    bool poisoned;
    RenderScope scopes[RENDER_SCOPE_CAP];
} RenderScopes;
static _Thread_local RenderScopes render_scopes;
static bool projection_read(uint64_t address, void *out, size_t size) {
    return labo_guest_copy(address, out, size, false);
}

// The layer is an eye layer of the composition bound to this base camera: an aligned guest object whose selected
// projection (+0x220) is its own (+0x288, of the perspective projection class), whose camera (+0x228) is of the eye
// camera class, and whose source camera (+0x80) is the base (of the base camera class). Read once per eye call.
static bool projection_eye_identity(uint64_t layer, uint64_t base) {
    uint64_t vtable = 0, selected = 0, source = 0, eye_vtable = 0, base_vtable = 0;
    return layer >= GUEST_LO && layer <= GUEST_HI - 0x448 && !(layer & 7) &&
        base >= GUEST_LO && base <= GUEST_HI - 0x60 && !(base & 7) &&
        projection_read(layer + 0x288, &vtable, sizeof vtable) && vtable == PROJ_VTABLE &&
        projection_read(layer + 0x220, &selected, sizeof selected) && selected == layer + 0x288 &&
        projection_read(layer + 0x80, &source, sizeof source) && source == base &&
        projection_read(layer + 0x228, &eye_vtable, sizeof eye_vtable) && eye_vtable == PROJ_EYE_VTABLE &&
        projection_read(base, &base_vtable, sizeof base_vtable) && base_vtable == PROJ_BASE_VTABLE;
}

static bool projection_frame_address(uint64_t frame) {
    return frame >= GUEST_LO && frame <= GUEST_HI - 16 && !(frame & 15);
}

// The tests that tell the display's two eye calls apart, shared by the traversal (projection_observe_traversal) and
// the display's pair of eye layers (pc_vr_projection_trace_eye_layer).
// The stack test: the frame record (saved frame pointer, saved link register) at the call's frame pointer, read from a
// frame-aligned guest address, saving the frame pointer of an outer (higher) frame.
static bool projection_saved_frame(uint64_t frame, uint64_t saved[2]) {
    return projection_frame_address(frame) && projection_read(frame, saved, 2 * sizeof saved[0]) &&
        projection_frame_address(saved[0]) && saved[0] > frame;
}

// The sibling's call: it returns into the root's call (PROJ_SIBLING_RETURN), saved the root's frame pointer, and runs
// on the root's thread.
static bool projection_sibling_call(const uint64_t saved[2], uint64_t root_frame_pointer, pthread_t root_thread) {
    return saved[1] == PROJ_SIBLING_RETURN && saved[0] == root_frame_pointer && pthread_equal(root_thread, pthread_self());
}

// The link test, read live at the sibling's call: the root layer's +0x208 names the sibling layer, its configuration
// (+0x210) is the sibling's, and the root's call is still on the stack (its frame record still returns to
// PROJ_ROOT_RETURN). The sibling may have no link back to the root.
static bool projection_live_link(uint64_t root_layer, uint64_t root_frame_pointer, uint64_t sibling_layer,
                                 uint64_t sibling_config) {
    uint64_t live_link = 0, live_config = 0, root_saved[2] = {0};
    return projection_read(root_layer + 0x208, &live_link, sizeof live_link) && live_link == sibling_layer &&
        projection_read(root_layer + 0x210, &live_config, sizeof live_config) && live_config == sibling_config &&
        projection_read(root_frame_pointer, root_saved, sizeof root_saved) && root_saved[1] == PROJ_ROOT_RETURN;
}

// A layer configuration's address (+0x210), as the root's is checked: set, aligned, in guest memory.
static bool projection_config_address(uint64_t config) {
    return config && !(config & 7) && config >= GUEST_LO && config < GUEST_HI;
}

bool pc_vr_projection_trace_root(uint64_t frame) {
    uint64_t saved[2] = {0};
    return projection_saved_frame(frame, saved) && saved[1] == PROJ_ROOT_RETURN;
}

// A pose with native views: what the eyes are rendered with and xrEndFrame is given. Its flags, not its values (D116:
// OpenXR's poses and FOVs are used as given), nor its age (D101: the frame keeps the pose head look took).
static bool projection_native_pose(const XrPoseSnapshot *pose) {
    return pose->active && pose->orientation_valid && pose->orientation_tracked && pose->views_valid &&
        pose->native_views_valid && pose->frame_id && pose->generation && pose->calibration.epoch;
}

bool pc_vr_render_trace_enabled(void) {
    return atomic_load_explicit(&render_enabled, memory_order_relaxed);
}

// The store's slots are written by the thread that publishes a certificate (under projection_lock) and read by the
// renderer's threads, which do not hold that lock. Publication and lookup share this lock of their own, so a lookup
// never copies a slot while it is being recycled. Lock order: projection_lock, when held, before this one.
static pthread_mutex_t render_store_lock = PTHREAD_MUTEX_INITIALIZER;

bool pc_vr_render_trace_resolve(uint64_t id, PcVrRenderCertificate *out) {
    const unsigned count = atomic_load_explicit(&render_certificate_count, memory_order_acquire);
    if (!out || !id || id > count) return false;
    pthread_mutex_lock(&render_store_lock);
    const PcVrRenderCertificate *slot = &render_certificates[(id - 1) % RENDER_CERTIFICATE_SLOTS];
    const bool found = slot->id == id;  // (a slot recycled since holds a newer certificate's id)
    if (found) *out = *slot;
    pthread_mutex_unlock(&render_store_lock);
    return found;
}

static void render_certificate_store(const PcVrRenderCertificate *certificate) {
    pthread_mutex_lock(&render_store_lock);
    render_certificates[(certificate->id - 1) % RENDER_CERTIFICATE_SLOTS] = *certificate;
    pthread_mutex_unlock(&render_store_lock);
}

static RenderScopes *render_tls(void) {
    const uint64_t epoch = atomic_load_explicit(&render_epoch, memory_order_acquire);
    if (render_scopes.epoch != epoch) {
        memset(&render_scopes, 0, sizeof render_scopes);
        render_scopes.epoch = epoch;
    }
    return &render_scopes;
}

static RenderScope render_scope_read(uint64_t layer, uint64_t rinfo) {
    RenderScope scope = {.layer = layer, .rinfo = rinfo};
    uint64_t words[10] = {0};
    const bool read = projection_read(rinfo, words, sizeof words);
    scope.pass = (uint32_t)words[0]; scope.camera = words[5]; scope.projection = words[6]; scope.context = words[9];
    scope.readable = read && projection_read(scope.context + 0xb8, &scope.cb, sizeof scope.cb);
    scope.identity = scope.readable && words[3] == layer && scope.cb;
    return scope;
}

static bool render_scope_matches(const RenderScope *first, const RenderScope *second) {
    return first->readable && second->identity && first->layer == second->layer && first->rinfo == second->rinfo &&
        first->pass == second->pass && first->camera == second->camera && first->projection == second->projection &&
        first->context == second->context && first->cb == second->cb;
}

static void render_begin_wrapper(Cpu *cpu) {
    if (!atomic_load_explicit(&render_collecting, memory_order_relaxed)) {
        render_begin_original(cpu); return;
    }
    RenderScopes *tls = render_tls();
    if (!tls->poisoned) {
        if (tls->depth == RENDER_SCOPE_CAP) tls->poisoned = true;
        else {
            RenderScope scope = render_scope_read(cpu->x[0], cpu->x[1]);
            scope.identity = scope.identity && cpu->x[30] == RENDER_BEGIN_CALLER;
            // Unrelated/invalid nested scopes suppress an otherwise valid parent.
            tls->scopes[tls->depth++] = scope;
        }
    }
    render_begin_original(cpu); // Preserve all original CPU effects exactly once.
}

static void render_end_wrapper(Cpu *cpu) {
    if (!atomic_load_explicit(&render_collecting, memory_order_relaxed)) {
        render_end_original(cpu); return;
    }
    RenderScopes *tls = render_tls();
    const unsigned depth = tls->depth;
    const uint64_t epoch = tls->epoch;
    // The +68 call is a tail BR. Its LR belongs to ddcc14's caller, not ddcd54.
    const RenderScope end = render_scope_read(cpu->x[0], cpu->x[1]);
    const bool matched = !tls->poisoned && depth &&
        render_scope_matches(&tls->scopes[depth - 1], &end);
    if (!matched) tls->poisoned = true; // Never pop/search for an outer matching entry.
    render_end_original(cpu);
    // A nested callback in the original must have restored exactly this stack.
    if (matched && tls->epoch == epoch && !tls->poisoned) {
        if (tls->depth == depth) --tls->depth;
        else tls->poisoned = true;
    }
}

static void render_publish_locked(RecordEye *eye) {
    if (!atomic_load_explicit(&render_collecting, memory_order_relaxed) || eye->certificate ||
        !eye->view_current || !eye->logical_current || !eye->device_current) return;
    const unsigned count = atomic_load_explicit(&render_certificate_count, memory_order_relaxed);
    if (count == UINT32_MAX) return;  // (ids never wrap: an old id must not resolve to a new certificate)
    float parameters[9], device[16];
    if (!projection_read(eye->layer + 0x288 + 0x98, parameters, sizeof parameters) ||
        !projection_read(eye->layer + 0x288 + 0x4c, device, sizeof device)) return;
    PcVrRenderCertificate certificate = {.id = count + 1, .traversal = eye->serial, .frame = eye->pose.frame_id,
        .generation = eye->pose.generation, .native_index = eye->native_index,
        .units_per_meter = eye->units_per_meter,
        .depth_terms = {device[10], device[11], device[14], device[15]}, .near_far = {parameters[0], parameters[1]},
        .native_snapshot = eye->pose};
    render_certificate_store(&certificate);
    atomic_store_explicit(&render_certificate_count, count + 1, memory_order_release);
    eye->certificate = certificate.id;
}

// The record's eye drawn by this render scope: the scope is the eye layer's, through its own camera and projection.
static const RecordEye *record_eye_of_scope_locked(const RenderScope *scope) {
    for (unsigned side = 0; side < 2; ++side) {
        const RecordEye *eye = &projection_record.eyes[side];
        if (eye->layer && eye->layer == scope->layer && scope->camera == eye->layer + 0x228 &&
            scope->projection == eye->layer + 0x288) return eye;
    }
    return NULL;
}

// The certificate of a draw being recorded on this thread into this command buffer (0: none), asked for every draw
// the game records: the draw's render scope against the layer's render info now, then the record's eye under
// projection_lock. The eye's camera and projection change only through the game's two builders, and each call for an
// eye takes its certificate away (pc_vr_render_trace_invalidate_camera, render_invalidate_projection), so the
// certificate stands for them as they are. Render callbacks can record on a worker other than the camera thread.
uint64_t pc_vr_render_trace_record(uint64_t command_buffer) {
    if (!atomic_load_explicit(&render_collecting, memory_order_relaxed)) return 0;
    RenderScopes *tls = render_tls();
    if (tls->poisoned || !tls->depth || !tls->scopes[tls->depth - 1].identity) return 0;
    RenderScope *scope = &tls->scopes[tls->depth - 1];
    const RenderScope current_scope = render_scope_read(scope->layer, scope->rinfo);
    if (!command_buffer || command_buffer != scope->cb || command_buffer != current_scope.cb) return 0;
    if (!render_scope_matches(scope, &current_scope)) return 0;
    pthread_mutex_lock(&projection_lock);
    const RecordEye *eye = record_eye_of_scope_locked(scope);
    const uint64_t id = eye && atomic_load_explicit(&projection_enabled, memory_order_relaxed) ? eye->certificate : 0;
    pthread_mutex_unlock(&projection_lock);
    return id;
}

// Whether the layer this thread is rendering (its innermost render scope) is one of the two eye layers, drawn through
// that layer's own camera and projection: an eye layer of the record, or, while the record knows none yet (after the
// first load, until head look binds the game's camera), one of the display's pair of eye layers
// (projection_display_pair, known from the display's own calls). Unlike a certificate it does not depend on the eye's
// view and FOV for this frame. Returns PC_VR_LAYER_DRAW_EYE when it is, 0 when it is another layer (the pause menu's)
// once a pair or an eye layer is known, and PC_VR_LAYER_DRAW_MAYBE_EYE when that cannot be told: neither is known
// yet, or this thread's render scopes are not to be trusted; with PC_VR_LAYER_DRAW_UNBOUND when the layer is drawn
// with the game's own camera.
static uint32_t render_scope_eye_layer(void) {
    RenderScopes *tls = render_tls();
    if (!tls->poisoned && !tls->depth) return 0;  // (not rendering a layer)
    if (tls->poisoned || !tls->scopes[tls->depth - 1].identity) return PC_VR_LAYER_DRAW_MAYBE_EYE;
    const RenderScope *scope = &tls->scopes[tls->depth - 1];
    pthread_mutex_lock(&projection_lock);
    const RecordEye *eye = record_eye_of_scope_locked(scope);
    const bool record_layer = eye != NULL;
    const ProjectionDisplayPair *pair = &projection_display_pair;
    const bool record_known = record_eyes_known_locked();
    // The display's pair speaks only until the record knows an eye layer: while the pause menu is open the display
    // alternates between the world's pair and the menu's own pair of layers through the same calls (Link,
    // 2026-11-12), which only the record's base camera (the play camera) tells apart.
    const bool pair_known = !record_known && pair->layers[0] && pair->layers[1];
    const bool pair_layer = pair_known && (scope->layer == pair->layers[0] || scope->layer == pair->layers[1]) &&
        scope->camera == scope->layer + 0x228 && scope->projection == scope->layer + 0x288;
    const bool none_known = !pair_known && !record_known;
    // Drawn with the game's own camera: the record's eye without its view this frame (its call refused or not the
    // record's; a builder call takes view_current away until the record's own build), or no record eye at all.
    const bool unbound = record_layer ? !eye->view_current : pair_layer || none_known;
    pthread_mutex_unlock(&projection_lock);
    const uint32_t flags = unbound ? PC_VR_LAYER_DRAW_UNBOUND : 0;
    return record_layer || pair_layer ? PC_VR_LAYER_DRAW_EYE | flags : none_known ? PC_VR_LAYER_DRAW_MAYBE_EYE | flags : 0;
}

// The kind of draw this thread is recording (pc_vr_render_trace.h PcVrLayerDraw, with its layer's flag).
static _Thread_local uint32_t render_layer_draw;

uint32_t rt_layer_draw_begin(uint32_t kind) {
    const uint32_t previous = render_layer_draw;
    // The kind needs nothing from the render trace (the display's draws come before the trace is armed, at the title
    // screen); whose layer it is does. While the trace is not collecting, that cannot be told.
    if (pc_vr_enabled())
        render_layer_draw = (kind & PC_VR_LAYER_DRAW_KIND) |
            (atomic_load_explicit(&render_collecting, memory_order_relaxed) ? render_scope_eye_layer()
                                                                             : PC_VR_LAYER_DRAW_MAYBE_EYE);
    return previous;
}

uint32_t rt_lens_draw_begin(uint32_t hidden) {
    const uint32_t previous = render_layer_draw;
    if (pc_vr_enabled())
        render_layer_draw = previous | (hidden ? PC_VR_LAYER_DRAW_LENS_HIDDEN : PC_VR_LAYER_DRAW_LENS) |
            (atomic_load_explicit(&render_collecting, memory_order_relaxed) ? render_scope_eye_layer()
                                                                             : PC_VR_LAYER_DRAW_MAYBE_EYE);
    return previous;
}

uint32_t rt_rune_test_draw_begin(void) {
    const uint32_t previous = render_layer_draw;
    if (pc_vr_enabled()) render_layer_draw = previous | PC_VR_LAYER_DRAW_RUNE_TEST;
    return previous;
}

void rt_layer_draw_end(uint32_t previous) { render_layer_draw = previous; }

uint32_t pc_vr_layer_draw_recording(void) { return render_layer_draw; }

void pc_vr_render_trace_invalidate_camera(uint64_t camera) {
    if (!atomic_load_explicit(&render_collecting, memory_order_relaxed) || !record_eye_layer(camera - 0x228))
        return;
    pthread_mutex_lock(&projection_lock);
    for (unsigned side = 0; side < 2; ++side) {
        RecordEye *eye = &projection_record.eyes[side];
        if (eye->layer && camera == eye->layer + 0x228) { eye->view_current = false; eye->certificate = 0; }
    }
    pthread_mutex_unlock(&projection_lock);
}

static void render_invalidate_projection(uint64_t object, bool device) {
    if (!atomic_load_explicit(&render_collecting, memory_order_relaxed) || !record_eye_layer(object - 0x288))
        return;
    pthread_mutex_lock(&projection_lock);
    RecordEye *eye = record_eye_of_projection_locked(object);
    if (eye) {
        eye->device_current = false; eye->certificate = 0;
        if (!device) eye->logical_current = false;
    }
    pthread_mutex_unlock(&projection_lock);
}

// A refusal: counted by reason, and the first few logged.
static void projection_refuse_locked(PcVrGuard reason) {
    pc_vr_guard_count(reason);
    if (++projection_refused <= 8)
        LOG("[pc-vr-native-view] refused=%llu reason=%s\n", (unsigned long long)projection_refused,
            pc_vr_guard_name(reason));
}

bool pc_vr_projection_trace_eye(uint64_t layer, uint64_t base, const PcVrHeadLookCamera *base_camera, bool fresh,
    const XrPoseSnapshot *pose, uint64_t guest_frame_pointer) {
    if (!atomic_load_explicit(&projection_enabled, memory_order_relaxed)) return false;
    // The call's own guest state, read before the lock: its frame record, the layer's objects, side, configuration
    // and sibling link, the base camera's stored right row and the eye camera the game just copied from it.
    uint64_t saved[2] = {0}, config = 0, sibling = 0;
    uint16_t flags = 0;
    float right[3];
    PcVrHeadLookCamera eye_camera;
    const bool framed = projection_saved_frame(guest_frame_pointer, saved);
    const bool root = framed && saved[1] == PROJ_ROOT_RETURN;
    const bool readable = projection_eye_identity(layer, base) &&
        projection_read(layer + 0x90, &flags, sizeof flags) && projection_read(layer + 0x210, &config, sizeof config) &&
        projection_read(layer + 0x208, &sibling, sizeof sibling) && projection_read(base + 8, right, sizeof right) &&
        projection_read(layer + 0x260, &eye_camera, sizeof eye_camera);
    const unsigned side = !!(flags & 0x1000);
    pthread_mutex_lock(&projection_lock);
    if (!atomic_load_explicit(&projection_enabled, memory_order_relaxed)) {
        pthread_mutex_unlock(&projection_lock); return false;
    }
    FrameRecord *record = &projection_record;
    PcVrGuard refused = PC_VR_GUARD_COUNT;
    bool counted = true;  // (a sibling of a root already refused is not counted again)
    if (!framed || (!root && saved[1] != PROJ_SIBLING_RETURN)) {
        record->open = false;
        refused = PC_VR_GUARD_TRAVERSAL_FRAME_UNREADABLE;
    } else if (root) {
        // A new frame: the record starts afresh (an open earlier root is abandoned).
        record->open = false;
        record->serial = ++projection_root_serial;
        if (!readable || !projection_config_address(config) || sibling == layer || (sibling & 7) ||
            sibling < GUEST_LO || sibling > GUEST_HI - 0x448) refused = PC_VR_GUARD_TRAVERSAL_ROOT_IDENTITY;
        else if (!fresh) refused = PC_VR_GUARD_VIEW_NOT_FRESH;
        else if (!pose || !projection_native_pose(pose)) refused = PC_VR_GUARD_VIEW_NO_POSE;
        else {
            // (the D053 calibration of this LOCAL definition: the heading and origin the views are relative to)
            PcVrNativeViewReference reference = {.generation = pose->calibration.epoch};
            memcpy(reference.orientation_xyzw, pose->calibration.heading_xyzw, sizeof reference.orientation_xyzw);
            memcpy(reference.origin_xyz, pose->calibration.origin_xyz, sizeof reference.origin_xyz);
            if (!pc_vr_native_view_build_pair(base_camera, &reference, pose, projection_view_scale, &record->pair))
                refused = PC_VR_GUARD_VIEW_PAIR;
            else {
                record->open = true;
                record->thread = pthread_self();
                record->frame_pointer = guest_frame_pointer;
                record->root_layer = layer;
                record->sibling = sibling;
                record->config = config;
                record->base = base;
                record->root_side = side;
                record->base_camera = *base_camera;
                record->pose = *pose;
                double focus = 0;
                for (unsigned i = 0; i < 3; ++i) focus += pow((double)base_camera->target[i] - base_camera->position[i], 2);
                focus = sqrt(focus) / projection_view_scale;
                if (isfinite(focus)) atomic_store_explicit(&projection_focus_m, focus, memory_order_relaxed);
                if (++projection_records <= 4)
                    LOG("[pc-vr-traversal] record=%llu serial=%llu (the first few: the frames have started)\n",
                        (unsigned long long)projection_records, (unsigned long long)record->serial);
            }
        }
    } else {
        // The sibling's call (one use: a duplicate or delayed sibling cannot take the record again): the root's
        // sibling layer on the other side, called from the root's call on its thread, with the same configuration and
        // the base camera as it was at the root.
        counted = record->open;
        const bool linked = record->open && readable && layer == record->sibling &&
            side != record->root_side && config == record->config && base == record->base &&
            projection_sibling_call(saved, record->frame_pointer, record->thread) &&
            !memcmp(base_camera, &record->base_camera, sizeof *base_camera) &&
            projection_live_link(record->root_layer, record->frame_pointer, layer, config);
        if (!linked) refused = PC_VR_GUARD_TRAVERSAL_NOT_SIBLING;
        else if (!fresh) refused = PC_VR_GUARD_VIEW_NOT_FRESH;
        record->open = false;
    }
    // Which runtime eye this layer's camera is: the game's copy sits to one side of the base camera along its right,
    // the root's to the other; the runtime's view 0 is the left eye (PRIMARY_STEREO, used as given: D116).
    PcVrProjectionEyeBasis basis = {0};
    if (refused == PC_VR_GUARD_COUNT &&
        (!pc_vr_projection_eye_basis(&record->base_camera, &eye_camera, right, &basis) || !basis.right_matches ||
         !basis.parallel_eye || !basis.lateral_eye || !basis.offset_sign ||
         (!root && basis.offset_sign != -record->root_offset_sign))) refused = PC_VR_GUARD_VIEW_PLACEMENT;
    const unsigned native_index = basis.offset_sign < 0 ? 0 : 1;
    // One write (D115 step 5c), not read back: labo_guest_copy refuses a page that is not writable before it copies,
    // and the eye layer is the game's heap object. A refused write leaves the eye without a certificate this frame.
    if (refused == PC_VR_GUARD_COUNT &&
        !labo_guest_copy(layer + 0x260, &record->pair.eyes[native_index], sizeof(PcVrHeadLookCamera), true))
        refused = PC_VR_GUARD_VIEW_WRITE_FAILED;
    if (refused == PC_VR_GUARD_COUNT && root) record->root_offset_sign = basis.offset_sign;
    if (refused != PC_VR_GUARD_COUNT) {
        if (root) record->open = false;
        if (counted) projection_refuse_locked(refused);
    }
    // This side's eye, replaced by its own call: an eye layer of the composition, written or not. A call that cannot
    // say its side (its objects unreadable), or a refused second call on a side this record already wrote (the root's),
    // leaves the eyes as they are.
    RecordEye *eye = &record->eyes[side];
    const bool replaces = readable &&
        (root || refused == PC_VR_GUARD_COUNT || !eye->view_written || eye->serial != record->serial);
    if (replaces) *eye = (RecordEye){.layer = layer};
    if (replaces && refused == PC_VR_GUARD_COUNT) {
        eye->serial = record->serial;
        eye->thread = pthread_self();
        eye->native_index = native_index;
        eye->units_per_meter = record->pair.game_units_per_meter;
        eye->pose = record->pose;
        eye->view_written = true;
    }
    record_eye_layers_update_locked();
    pthread_mutex_unlock(&projection_lock);
    return refused == PC_VR_GUARD_COUNT;
}

void pc_vr_projection_trace_eye_built(uint64_t layer) {
    pthread_mutex_lock(&projection_lock);
    for (unsigned side = 0; side < 2; ++side) {
        RecordEye *eye = &projection_record.eyes[side];
        if (eye->layer == layer && eye->view_written && pthread_equal(eye->thread, pthread_self()))
            eye->view_current = true;
    }
    pthread_mutex_unlock(&projection_lock);
}

// On the game's projection builder for an eye of the record whose view is written and built: its parameters become
// the eye's FOV, keeping the near and far planes (pc_vr_projection_from_fov). Once per eye per frame. True: written,
// the logical matrix to be checked after the original builder.
static bool projection_write_native_fov(uint64_t object) {
    if (!atomic_load_explicit(&projection_enabled, memory_order_relaxed) || !record_eye_layer(object - 0x288))
        return false;
    pthread_mutex_lock(&projection_lock);
    RecordEye *eye = record_eye_of_projection_locked(object);
    if (!eye || !eye->view_written || eye->fov_attempted) {
        pthread_mutex_unlock(&projection_lock); return false;
    }
    eye->fov_attempted = true;
    float original[9], mapped[9];
    PcVrGuard refused = PC_VR_GUARD_COUNT;
    if (!eye->view_current || !pthread_equal(eye->thread, pthread_self()) ||
        !projection_read(object + 0x98, original, sizeof original) ||
        !pc_vr_projection_from_fov(eye->pose.eyes[eye->native_index].fov_left_right_up_down, original, mapped))
        refused = PC_VR_GUARD_FOV_PRECONDITION;
    // One write, not read back (as the camera's in pc_vr_projection_trace_eye).
    else if (!labo_guest_copy(object + 0xa0, mapped + 2, 7 * sizeof(float), true)) refused = PC_VR_GUARD_FOV_WRITE_FAILED;
    if (refused != PC_VR_GUARD_COUNT) projection_refuse_locked(refused);
    else eye->fov_written = true;
    pthread_mutex_unlock(&projection_lock);
    return refused == PC_VR_GUARD_COUNT;
}

// The one post-condition (D115 step 5c): the logical matrix the game's writer built from the parameters written gives
// the eye's own frustum (pc_vr_projection_native_rays).
static void projection_logical_built(uint64_t object) {
    pthread_mutex_lock(&projection_lock);
    RecordEye *eye = record_eye_of_projection_locked(object);
    float logical[16];
    if (eye && eye->fov_written && !eye->logical_ok) {
        if (projection_read(object + 0xc, logical, sizeof logical) &&
            pc_vr_projection_native_rays(eye->pose.eyes[eye->native_index].fov_left_right_up_down, logical))
            eye->logical_ok = eye->logical_current = true;
        else projection_refuse_locked(PC_VR_GUARD_FOV_CONVERSION_MISMATCH);
    }
    pthread_mutex_unlock(&projection_lock);
}

// The device stage: the game's fixed conversion of the logical matrix. The certificate reads its depth terms. A device
// stage built again (its certificate taken away by render_invalidate_projection) certifies the eye again while its
// view and logical matrix are still as written.
static void projection_device_built(uint64_t object) {
    if (!record_eye_layer(object - 0x288)) return;
    pthread_mutex_lock(&projection_lock);
    RecordEye *eye = record_eye_of_projection_locked(object);
    if (eye && eye->logical_ok) {
        eye->device_current = true;
        render_publish_locked(eye);
    }
    pthread_mutex_unlock(&projection_lock);
}

// A layer that is an eye layer by its own objects, without the base camera an eye of the record also checks
// (projection_eye_identity): an aligned guest object whose selected projection (+0x220) is its own (+0x288, of the
// perspective projection class) and whose camera (+0x228) is of the eye camera class.
static bool projection_eye_layer_objects(uint64_t layer) {
    uint64_t vtable = 0, selected = 0, eye_vtable = 0;
    return layer >= GUEST_LO && layer <= GUEST_HI - 0x448 && !(layer & 7) &&
        projection_read(layer + 0x288, &vtable, sizeof vtable) && vtable == PROJ_VTABLE &&
        projection_read(layer + 0x220, &selected, sizeof selected) && selected == layer + 0x288 &&
        projection_read(layer + 0x228, &eye_vtable, sizeof eye_vtable) && eye_vtable == PROJ_EYE_VTABLE;
}

void pc_vr_projection_trace_eye_layer(uint64_t layer, uint64_t guest_frame_pointer) {
    if (!atomic_load_explicit(&projection_enabled, memory_order_relaxed)) return;
    // The call's own guest state, read before the lock: its frame record, then the layer's objects, side and
    // configuration. The builder serves other layers too: a call that is neither a root's nor a sibling's by its frame
    // record is not the display's, and leaves an open root as it was.
    uint64_t saved[2] = {0}, config = 0;
    uint16_t flags = 0;
    if (!projection_saved_frame(guest_frame_pointer, saved) ||
        (saved[1] != PROJ_ROOT_RETURN && saved[1] != PROJ_SIBLING_RETURN))
        return;
    const bool readable = projection_eye_layer_objects(layer) &&
        projection_read(layer + 0x90, &flags, sizeof flags) && projection_read(layer + 0x210, &config, sizeof config);
    const unsigned side = !!(flags & 0x1000);
    pthread_mutex_lock(&projection_lock);
    ProjectionDisplayPair *pair = &projection_display_pair;
    // Once the record knows an eye layer it decides (render_scope_eye_layer): the pair is no longer followed, so the
    // pause menu's own pair, shown through the same calls, does not replace it.
    if (!atomic_load_explicit(&projection_enabled, memory_order_relaxed) || record_eyes_known_locked()) {
        pair->open = false;
        pthread_mutex_unlock(&projection_lock);
        return;
    }
    if (saved[1] == PROJ_ROOT_RETURN) {  // (an open earlier root is abandoned)
        pair->open = readable && projection_config_address(config);
        pair->root_layer = layer;
        pair->root_config = config;
        pair->root_side = side;
        pair->root_frame_pointer = guest_frame_pointer;
        pair->root_thread = pthread_self();
        pthread_mutex_unlock(&projection_lock);
        return;
    }
    // The sibling's call (one use: a duplicate or delayed sibling cannot consume the root again).
    const bool linked = readable && pair->open &&
        projection_sibling_call(saved, pair->root_frame_pointer, pair->root_thread) && side != pair->root_side &&
        config == pair->root_config &&
        projection_live_link(pair->root_layer, pair->root_frame_pointer, layer, config);
    pair->open = false;
    uint64_t layers[2] = {0};
    layers[pair->root_side] = pair->root_layer;
    layers[side] = layer;
    if (linked && (layers[0] != pair->layers[0] || layers[1] != pair->layers[1])) {
        pair->layers[0] = layers[0];
        pair->layers[1] = layers[1];
        ++projection_display_pairs;
        if (projection_display_pairs <= 8)
            LOG("[pc-vr-traversal] eye layers known from the display's pair (pair %llu of at most 8 logged)\n",
                (unsigned long long)projection_display_pairs);
    }
    pthread_mutex_unlock(&projection_lock);
}

// (diagnostic) SAVAGEWIND_PC_VR_PROJECTION_LIST=1: each perspective projection the game builds, by object, with its
// fovy, aspect and centre, when first seen and when those change (at most 64 lines); =2: only the world camera's and
// its copies (16:9, far plane 20000 or more).
static void projection_list(uint64_t object, uint64_t caller) {
    static uint64_t objects[32];
    static float seen[32][3];
    static unsigned count, lines;
    const int on = pc_vr_config()->diagnostics.projection_list;
    float params[9];
    if (!on || lines >= 64 || !projection_read(object + 0x98, params, sizeof params)) return;
    if (on == 2 && !(params[1] >= 20000 && fabsf(params[6] - 16.0f / 9.0f) < 0.01f)) return;
    pthread_mutex_lock(&projection_lock);  // (for the list's own state: nothing a certificate depends on changes)
    unsigned i = 0;
    while (i < count && objects[i] != object) ++i;
    const bool fresh = i == count && count < 32;
    if (fresh) objects[count++] = object;
    if (lines < 64 && i < count && (fresh || seen[i][0] != params[2] || seen[i][1] != params[6] || seen[i][2] != params[7])) {
        seen[i][0] = params[2], seen[i][1] = params[6], seen[i][2] = params[7];
        ++lines;
        LOG("[pc-vr-projection-list] object=%llx caller=%llx fovy_deg=%.4f aspect=%.5f center=%.5f,%.5f near=%.4g far=%.6g\n",
            (unsigned long long)object, (unsigned long long)caller, params[2] * 57.29578f,
            params[6], params[7], params[8], params[0], params[1]);
    }
    pthread_mutex_unlock(&projection_lock);
}

static void projection_builder_wrapper(Cpu *cpu) {
    const uint64_t object = cpu->x[0], caller = cpu->x[30];
    render_invalidate_projection(object, false);
    const bool exact = cpu->x[30] == PROJ_BUILD_CALLER && cpu->x[1] == object + 0xc;
    const bool written = exact && projection_write_native_fov(object);
    projection_original(cpu);
    if (written) projection_logical_built(object);
    projection_list(object, caller);
}

static void projection_device_wrapper(Cpu *cpu) {
    const uint64_t object = cpu->x[0];
    render_invalidate_projection(object, true);
    const bool exact = cpu->x[30] == PROJ_DEVICE_CALLER && cpu->x[1] == object + 0x4c && cpu->x[2] == object + 0xc;
    projection_device_original(cpu);
    if (exact) projection_device_built(object);
}

double pc_vr_camera_focus_distance(void) {
    return atomic_load_explicit(&projection_focus_m, memory_order_relaxed);
}

bool pc_vr_world_scale_active(void) {
    return atomic_load_explicit(&projection_enabled, memory_order_acquire);
}

// Under projection_lock: the root eye builds its pair with one value, and its sibling uses that pair.
double pc_vr_world_scale(double factor) {
    pthread_mutex_lock(&projection_lock);
    double scale = 0;
    if (atomic_load_explicit(&projection_enabled, memory_order_relaxed) && projection_view_scale > 0) {
        projection_view_scale = factor > 0 ? fmin(100.0, fmax(0.1, projection_view_scale * factor)) : projection_launch_scale;
        scale = projection_view_scale;
    }
    pthread_mutex_unlock(&projection_lock);
    if (scale) LOG("[pc-vr-native-view] world scale %.4g game units per metre (world at 1:%.3g of life size)\n", scale, scale);
    return scale;
}

// PC VR on is the play configuration: all of it, continuously, at the configured world scale. Armed at each head look
// init, afresh: the frame record and the counts start over; published certificates stay resolvable. None of it is
// armed when a part cannot run (no PC VR sites for this game version, an original projection writer or render scope
// callback missing): the eyes are then left as the game makes them, and no draw gets a certificate.
void pc_vr_projection_trace_init(void) {
    atomic_store_explicit(&render_collecting, false, memory_order_release);
    if (!pc_vr_enabled()) return;
    if (!pc_vr_main_compatible()) {
        LOG("[pc-vr-projection] disabled: no PC VR sites for this game version\n");
        return;
    }
    const double units_per_meter = pc_vr_config()->play.native_view_units_per_meter;  // (finite, above 0: its parse)
    pthread_mutex_lock(&projection_lock);
    if (!projection_registered) {
        const GuestFn builder = dispatch_lookup(PROJ_BUILDER), device = dispatch_lookup(PROJ_DEVICE);
        const GuestFn begin = dispatch_lookup(RENDER_BEGIN), end = dispatch_lookup(RENDER_END);
        if (!builder || !device || !begin || !end) {
            if (!builder || !device) LOG("[pc-vr-projection] disabled: missing original perspective/device writer\n");
            else LOG("[pc-vr-render-trace] disabled: missing pinned render callbacks\n");
            pthread_mutex_unlock(&projection_lock);
            return;
        }
        projection_original = builder;
        projection_device_original = device;
        render_begin_original = begin;
        render_end_original = end;
        hle_override(PROJ_BUILDER, projection_builder_wrapper, "PcVrProjectionObserver");
        hle_override(PROJ_DEVICE, projection_device_wrapper, "PcVrProjectionDeviceObserver");
        hle_override(RENDER_BEGIN, render_begin_wrapper, "PcVrRenderScopeBegin");
        hle_override(RENDER_END, render_end_wrapper, "PcVrRenderScopeEnd");
        projection_registered = true;
    }
    memset(&projection_record, 0, sizeof projection_record);
    record_eye_layers_update_locked();
    memset(&projection_display_pair, 0, sizeof projection_display_pair);
    projection_display_pairs = projection_records = projection_refused = 0;
    projection_view_scale = projection_launch_scale = units_per_meter;
    atomic_fetch_add_explicit(&render_epoch, 1, memory_order_release);
    atomic_store_explicit(&render_enabled, true, memory_order_release);
    atomic_store_explicit(&render_collecting, true, memory_order_release);
    atomic_store_explicit(&projection_enabled, true, memory_order_release);
    pthread_mutex_unlock(&projection_lock);
    LOG("[pc-vr-eyes] armed continuous native view/FOV/certificates game_units_per_meter=%.9g live_scale_keys=minus,equals,0 certificate_slots=%u scope_cap=%u\n",
        units_per_meter, RENDER_CERTIFICATE_SLOTS, RENDER_SCOPE_CAP);
}

RT_ON_RESET(pc_vr_projection_trace_reset) {
    atomic_store_explicit(&render_collecting, false, memory_order_release);
    atomic_fetch_add_explicit(&render_epoch, 1, memory_order_release);
    atomic_store_explicit(&projection_enabled, false, memory_order_release);
    pthread_mutex_lock(&projection_lock);
    memset(&projection_record, 0, sizeof projection_record);
    record_eye_layers_update_locked();
    memset(&projection_display_pair, 0, sizeof projection_display_pair);
    pthread_mutex_unlock(&projection_lock);
}
