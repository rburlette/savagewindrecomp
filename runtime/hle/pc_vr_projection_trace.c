// PC VR native eye views and render certificates (D042, D043, D048). Started from a bounded, read-only investigation
// of how the game builds its Labo eye cameras' perspective; three jobs now share it:
//
//  1. Traversals. The game renders its two Labo eye layers in one call chain: a root layer (one eye) and its sibling
//     (the other). The two are recognised as one traversal by their saved frame pointers and return addresses
//     (PROJ_ROOT_RETURN, PROJ_SIBLING_RETURN), and the root latches one complete pose snapshot that both eyes use
//     (projection_observe_traversal). What is seen of each eye layer (its camera, projection object, side) is kept
//     in a ProjectionNote. The same stack and link tests on the display's calls alone, before head look has bound the
//     game's camera, make the two eye layers known (projection_display_pair), so that a layer's draws are told an
//     eye's or another layer's from the first frame after a load (render_scope_eye_layer). Only until an eye note
//     exists: the display also shows the pause menu's own pair of layers through the same calls.
//  2. Native view and FOV. The eye camera's view becomes the headset eye's pose relative to the D053 calibration, at
//     a world scale of game units per metre (pc_vr_projection_trace_native_view, called from head look; D042), and
//     the eye projection's parameters become the eye's own asymmetric FOV (projection_prepare_native_fov, on the
//     game's projection builder; D043). The game's original logical and device projection writers always run, once;
//     the matrices they write are checked against what the new parameters must give (projection_matrix_contract).
//  3. Render certificates. The render layer's begin and end are wrapped (render_begin_wrapper, render_end_wrapper)
//     to know which eye layer the current thread is rendering. When the command recorder records a draw
//     (pc_vr_render_trace_record), a draw in a layer whose native view and FOV were verified gets that eye's
//     certificate id: which eye, which traversal, which pose snapshot. The eye-image producer uses it to admit eye images.
//
// The PC VR play configuration (runtime/pc_vr_config.h) runs all of it, continuously, at its world scale
// (pc_vr_projection_trace_init): no traversal, time or certificate budget. A failed or partial traversal yields no
// certificate pair and the next traversal starts afresh; a failed guest write (restored) stops the writes until the
// trace is armed again. Logs carry metadata only, never game data.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "../gamelib.h"
#include "../game_sites.h"
#include "../hle.h"
#include "../pc_vr.h"
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
#define PROJ_POSE_MAX_AGE_NS UINT64_C(250000000)
#define RENDER_BEGIN g_site.pcvr_render_begin
// The layer's end: no code calls it directly (vtable +0x68), and it follows begin (vtable +0x50) at +0x58 in 1.8.2 and
// 1.9.0 alike, the same 4 instructions.
#define RENDER_END (g_site.pcvr_render_begin + 0x58)
#define RENDER_BEGIN_CALLER g_site.pcvr_render_begin_caller
#define RENDER_SCOPE_CAP 8          // a thread's open render scopes (RenderScopes)
#define RENDER_CERTIFICATE_SLOTS 64 // the certificate store: a slot is reused after 64 newer certificates

// What was seen of one eye layer in the current traversal, and what was done to it.
typedef struct {
    // The guest objects: the eye layer, the base camera it copies, its projection object, the layer's configuration
    // and its sibling layer (the other eye).
    uint64_t layer, base, projection, config, sibling;
    // The head-look latch it was built with (sequence, frame, generation), this note's number, when, a hash of the
    // base camera, and the traversal it belongs to.
    uint64_t sequence, frame, generation, note, time_ns, base_hash, traversal_serial;
    unsigned side;
    int offset_sign;
    bool latch_valid;
    PcVrProjectionEyeBasis basis;
    bool basis_read, native_fov_ready, logical_seen;
    // What happened: head look acknowledged and applied; the native FOV attempted, applied, and its logical and
    // device matrices verified; the native view attempted, written and verified; and whether the camera, logical
    // projection and device projection still match what the certificate describes.
    bool head_acknowledged, head_applied, fov_attempted, fov_applied, fov_logical_ok, fov_device_ok;
    bool native_view_attempted, native_fields_applied, native_view_verified;
    bool render_view_current, render_logical_current, render_device_current;
    uint64_t render_certificate, render_camera_hash, render_view_hash;
    // The native FOV: which runtime eye, the game's projection parameters before and after, the eye's angles
    // (left, right, up, down) and the near and far planes.
    int fov_native_index;
    float fov_original[9], fov_mapped[9], fov_angles[4], fov_depth[2];
} ProjectionNote;

// The current traversal: its root eye, where and on which thread it runs, the one pose snapshot both eyes use, and
// the progress of the native FOV and view (masks: a bit per eye).
typedef struct {
    ProjectionNote root;
    uint64_t frame_pointer, serial;
    pthread_t thread;
    XrPoseSnapshot native_snapshot;
    bool open;            // the root was seen and its sibling not yet
    bool native_latched;  // native_snapshot holds the root's pose sample
    bool native_pair;     // the sibling came while that sample was still current
    bool fov_admitted, fov_failed, fov_finished;
    unsigned fov_written_mask, fov_logical_mask, fov_device_mask;
    PcVrNativeViewPair view_pair;
    bool view_skipped, view_admitted, view_failed, view_finished;
    unsigned view_written_mask, view_verified_mask;
} ProjectionTraversal;

static pthread_mutex_t projection_lock = PTHREAD_MUTEX_INITIALIZER;
// What a draw's certificate depends on, beyond its own render scope, changes only under projection_lock: the eye
// notes, the traversal, the published certificates and whether the trace is armed. Everything that takes the lock to
// change any of it takes it with projection_lock_for_change, which moves this version on; a render scope keeps the
// answer it got for a command buffer for as long as the version stands (pc_vr_render_trace_record). The two that take
// the lock only to read (render_record_evaluate, render_scope_eye_layer) leave it alone. The display's pair of eye
// layers (pc_vr_projection_trace_eye_layer) moves it on only when the pair changes (D095: kept answers refresh).
static atomic_uint_fast64_t render_state_version;
static void projection_lock_for_change(void) {
    pthread_mutex_lock(&projection_lock);
    atomic_fetch_add_explicit(&render_state_version, 1, memory_order_release);
}
static atomic_bool projection_enabled, projection_started;
static bool projection_registered;
static GuestFn projection_original, projection_device_original;
static ProjectionNote projection_notes[2];
// The note numbers, and the counts that bound or space out the log lines below.
static uint64_t projection_note_count, projection_rejected, projection_writes, projection_device_writes;
static uint64_t projection_bad_matrix, projection_pairs, projection_last_pair;
static ProjectionTraversal projection_traversal;
// The Labo display's pair of eye layers, known from its own calls for them (pc_vr_projection_trace_eye_layer): the
// root call for one eye layer, then the sibling call for the other, by the same stack and link tests as a traversal's,
// without head look's camera binding. It tells the eye layers' draws from other layers' while there is no eye note yet
// (render_scope_eye_layer). Under projection_lock; cleared at each arming and at the game reset.
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
// Never reset this identity at re-arm/reset: reused guest stack addresses and
// render-layer objects are not traversal identities. Zero is always invalid.
static uint64_t projection_root_serial;
static uint64_t projection_roots, projection_traversal_pairs, projection_stack_rejected;
static uint64_t projection_fov_attempts, projection_fov_rejected, projection_view_rejected;
// The native FOV and view writes, on while the trace is armed; a failed write (restored) turns them off.
static atomic_bool projection_fov_experiment, projection_view_experiment;
static double projection_view_scale, projection_launch_scale; // (live: pc_vr_world_scale)
static _Atomic double projection_focus_m; // the camera's target distance at the last built pair (D057)
static bool projection_matrix_contract(const float params[9], const float matrix[16]);
static bool projection_near(double actual, double expected);

// Immutable append-only storage outlives observation and queued command lists, reset and re-arm included. A slot is
// reused after 64 newer certificates; its ID field acts as a sequence lock.
static PcVrRenderCertificate render_certificates[RENDER_CERTIFICATE_SLOTS];
static atomic_uint render_certificate_count;
static atomic_bool render_enabled, render_collecting;
static atomic_uint_fast64_t render_epoch;
static GuestFn render_begin_original, render_end_original;
// One render layer being rendered on this thread (between the layer's begin and end): the layer, its render info,
// context and command buffer, its camera and projection, and the certificate last checked for it.
typedef struct {
    uint64_t layer, rinfo, context, cb, camera, projection, checked_certificate;
    uint32_t pass;
    bool readable, identity;
    // The certificate last worked out for a draw in this scope (0: none), the command buffer it was asked for and the
    // state version it was worked out at (pc_vr_render_trace_record).
    bool answered;
    uint64_t answer, answer_command_buffer, answer_version;
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
static void render_publish_locked(ProjectionNote *note);

// A traversal's end: its admitted native FOV and view pairs are finished, complete or not, so no later writer of this
// traversal adds to them. A rendered first eye cannot be rolled back; a partial pair gets no certificate pair, and the
// next traversal starts afresh.
static void projection_fov_finish_locked(void) {
    ProjectionTraversal *traversal = &projection_traversal;
    if (traversal->fov_admitted) traversal->fov_finished = true;
    if (traversal->view_admitted) traversal->view_finished = true;
}

static uint64_t projection_hash(const void *data, size_t size) {
    const unsigned char *bytes = data;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool projection_read(uint64_t address, void *out, size_t size) {
    return labo_guest_copy(address, out, size, false);
}

static bool projection_identity(const ProjectionNote *note) {
    uint64_t vtable = 0, selected = 0, source = 0, eye_vtable = 0, base_vtable = 0;
    uint16_t flags = 0;
    return note->layer >= GUEST_LO && note->layer <= GUEST_HI - 0x448 && !(note->layer & 7) &&
        note->base >= GUEST_LO && note->base <= GUEST_HI - 0x60 && !(note->base & 7) &&
        note->projection == note->layer + 0x288 &&
        projection_read(note->projection, &vtable, sizeof vtable) && vtable == PROJ_VTABLE &&
        projection_read(note->layer + 0x220, &selected, sizeof selected) && selected == note->projection &&
        projection_read(note->layer + 0x80, &source, sizeof source) && source == note->base &&
        projection_read(note->layer + 0x228, &eye_vtable, sizeof eye_vtable) && eye_vtable == PROJ_EYE_VTABLE &&
        projection_read(note->base, &base_vtable, sizeof base_vtable) && base_vtable == PROJ_BASE_VTABLE &&
        projection_read(note->layer + 0x90, &flags, sizeof flags) && !!(flags & 0x1000) == note->side;
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

static bool projection_native_quaternion(const float rotation[4]) {
    double norm = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(rotation[i])) return false;
        norm += (double)rotation[i] * rotation[i];
    }
    return norm > 0.99 && norm < 1.01;
}

static bool projection_native_pose(const XrPoseSnapshot *pose, uint64_t now) {
    if (!pose->active || !pose->orientation_valid || !pose->orientation_tracked ||
        !pose->views_valid || !pose->native_views_valid || !pose->frame_id || !pose->generation ||
        !pose->published_monotonic_ns || now < pose->published_monotonic_ns ||
        now - pose->published_monotonic_ns > PROJ_POSE_MAX_AGE_NS ||
        !projection_native_quaternion(pose->orientation_xyzw)) return false;
    for (unsigned side = 0; side < 2; ++side) {
        const XrEyeSnapshot *eye = &pose->eyes[side];
        if (!projection_native_quaternion(eye->orientation_xyzw)) return false;
        for (unsigned i = 0; i < 3; ++i) {
            if (!isfinite(eye->position_xyz[i])) return false;
            if (pose->angular_velocity_valid && !isfinite(pose->angular_velocity_xyz[i])) return false;
        }
        const float *fov = eye->fov_left_right_up_down;
        for (unsigned i = 0; i < 4; ++i) if (!isfinite(fov[i])) return false;
        if (!(fov[0] < fov[1] && fov[3] < fov[2] && fov[0] > -1.5707963f &&
              fov[1] < 1.5707963f && fov[3] > -1.5707963f && fov[2] < 1.5707963f)) return false;
    }
    return true;
}

// A publication can occur after the caller's earlier clock read. Validate
// both samples against time read AFTER copying current tracking, otherwise a
// valid newer publication is falsely classified as future. Keep the rendered
// sample immutable; current tracking only gates its continued eligibility.
static bool projection_pose_current(const XrPoseSnapshot *retained) {
    XrPoseSnapshot current = {0};
    if (xr_runtime_get_pose(&current) != XR_POSE_VALID) return false;
    const uint64_t checked_now = xr_runtime_pose_clock_ns();
    return projection_native_pose(retained, checked_now) &&
        projection_native_pose(&current, checked_now) && current.generation == retained->generation;
}

static ProjectionNote *projection_find_note(uint64_t object) {
    ProjectionNote *note = NULL;
    for (unsigned i = 0; i < 2; ++i)
        if (projection_notes[i].note && projection_notes[i].projection == object &&
            (!note || projection_notes[i].note > note->note)) note = &projection_notes[i];
    return note;
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

static void render_publish_locked(ProjectionNote *note) {
    if (!atomic_load_explicit(&render_collecting, memory_order_relaxed) || note->render_certificate ||
        !note->native_view_verified || !note->fov_logical_ok || !note->fov_device_ok ||
        !note->render_view_current || !note->render_logical_current || !note->render_device_current) return;
    const unsigned count = atomic_load_explicit(&render_certificate_count, memory_order_relaxed);
    if (count == UINT32_MAX) return;  // (ids never wrap: an old id must not resolve to a new certificate)
    float parameters[9], logical[16], device[16];
    if (!projection_read(note->projection + 0x98, parameters, sizeof parameters) ||
        !projection_read(note->projection + 0xc, logical, sizeof logical) ||
        !projection_read(note->projection + 0x4c, device, sizeof device)) return;
    const ProjectionTraversal *traversal = &projection_traversal;
    PcVrRenderCertificate certificate = {.id = count + 1, .traversal = traversal->serial,
        .note = note->note, .sequence = note->sequence, .frame = note->frame,
        .generation = note->generation, .native_index = (uint32_t)note->fov_native_index,
        .side_bit = note->side, .units_per_meter = projection_view_scale,
        .reference_hash = projection_hash(&traversal->view_pair.reference, sizeof traversal->view_pair.reference),
        .camera_hash = note->render_camera_hash, .view_hash = note->render_view_hash,
        .parameters_hash = projection_hash(parameters, sizeof parameters),
        .logical_hash = projection_hash(logical, sizeof logical),
        .device_hash = projection_hash(device, sizeof device),
        .depth_terms = {device[10], device[11], device[14], device[15]}, .near_far = {parameters[0], parameters[1]},
        .native_snapshot = traversal->native_snapshot};
    render_certificate_store(&certificate);
    atomic_store_explicit(&render_certificate_count, count + 1, memory_order_release);
    note->render_certificate = certificate.id;
}

static bool render_fingerprint(const ProjectionNote *note, const PcVrRenderCertificate *cert) {
    PcVrHeadLookCamera camera;
    float view[12], parameters[9], logical[16], device[16];
    uint16_t dirty = UINT16_MAX;
    return projection_read(note->layer + 0x260, &camera, sizeof camera) &&
        projection_read(note->layer + 0x230, view, sizeof view) &&
        projection_read(note->projection + 8, &dirty, sizeof dirty) && !dirty &&
        projection_read(note->projection + 0x98, parameters, sizeof parameters) &&
        projection_read(note->projection + 0xc, logical, sizeof logical) &&
        projection_read(note->projection + 0x4c, device, sizeof device) &&
        projection_hash(&camera, sizeof camera) == cert->camera_hash &&
        projection_hash(view, sizeof view) == cert->view_hash &&
        projection_hash(parameters, sizeof parameters) == cert->parameters_hash &&
        projection_hash(logical, sizeof logical) == cert->logical_hash &&
        projection_hash(device, sizeof device) == cert->device_hash;
}

// Works a draw's certificate out in full: the draw's render scope against the layer's guest state now, then the eye
// note, the traversal and the certificate under projection_lock.
static uint64_t render_record_evaluate(uint64_t command_buffer) {
    RenderScopes *tls = render_tls();
    if (tls->poisoned || !tls->depth || !tls->scopes[tls->depth - 1].identity) return 0;
    RenderScope *scope = &tls->scopes[tls->depth - 1];
    const RenderScope current_scope = render_scope_read(scope->layer, scope->rinfo);
    if (!command_buffer || command_buffer != scope->cb || command_buffer != current_scope.cb) return 0;
    if (!render_scope_matches(scope, &current_scope)) return 0;
    pthread_mutex_lock(&projection_lock);
    const ProjectionNote *note = projection_find_note(scope->projection);
    const ProjectionTraversal *traversal = &projection_traversal;
    uint64_t id = 0;
    // Render callbacks can record on a worker different from the verified
    // camera-construction thread. Scope/object/generation/readback evidence is
    // required here; root/sibling construction still requires the same thread.
    // In order: the scope is the eye layer's own camera and projection; the trace is armed and the note's traversal
    // the current one, admitted, not failed and not skipped for warmup; the eye's view and FOV are verified and still
    // current, with a certificate.
    const bool ready = note && note->layer == scope->layer && scope->camera == note->layer + 0x228 &&
        atomic_load_explicit(&projection_enabled, memory_order_relaxed) &&
        traversal->serial && note->traversal_serial == traversal->serial &&
        traversal->view_admitted && !traversal->view_failed && !traversal->fov_failed && !traversal->view_skipped &&
        note->native_view_verified && note->fov_applied && note->fov_logical_ok && note->fov_device_ok &&
        note->render_view_current && note->render_logical_current && note->render_device_current &&
        note->render_certificate;
    // The certificate is this note's, and the layer's guest state still matches it (checked once per certificate in a
    // render scope).
    PcVrRenderCertificate cert;
    if (ready && pc_vr_render_trace_resolve(note->render_certificate, &cert) && cert.traversal == traversal->serial &&
        cert.note == note->note && cert.sequence == note->sequence && cert.frame == note->frame &&
        cert.generation == note->generation && cert.native_index == (uint32_t)note->fov_native_index &&
        cert.side_bit == note->side &&
        (scope->checked_certificate == cert.id || (projection_identity(note) && render_fingerprint(note, &cert)))) {
        scope->checked_certificate = cert.id;
        id = cert.id;
    }
    pthread_mutex_unlock(&projection_lock);
    return id;
}

// The certificate of a draw being recorded on this thread into this command buffer (0: none). Asked for every draw
// the game records. A render scope keeps the answer worked out for its command buffer and gives it again until
// render_state_version moves: nothing a certificate depends on has changed since, so working it out again (guest
// reads, the lock) would give the same. What the kept answer does not see is the game rewriting the layer's own render
// info in guest memory in the middle of the layer's rendering; the scope's end still checks that it matches its begin.
uint64_t pc_vr_render_trace_record(uint64_t command_buffer) {
    if (!atomic_load_explicit(&render_collecting, memory_order_relaxed)) return 0;
    RenderScopes *tls = render_tls();
    RenderScope *scope = !tls->poisoned && tls->depth && tls->scopes[tls->depth - 1].identity ?
        &tls->scopes[tls->depth - 1] : NULL;
    const uint64_t version = atomic_load_explicit(&render_state_version, memory_order_acquire);
    if (scope && scope->answered && scope->answer_command_buffer == command_buffer && scope->answer_version == version)
        return scope->answer;
    const uint64_t id = render_record_evaluate(command_buffer);
    if (!scope) return id;
    // (version was read before the evaluation: a change during it leaves the answer already out of date)
    scope->answered = true;
    scope->answer = id;
    scope->answer_command_buffer = command_buffer;
    scope->answer_version = version;
    return id;
}

// Whether the layer this thread is rendering (its innermost render scope) is one of the two eye layers, drawn through
// that layer's own camera and projection: an eye note's layer, or, while there is no eye note yet (after the first
// load, until head look binds the game's camera), one of the display's pair of eye layers (projection_display_pair,
// known from the display's own calls). Unlike a certificate it does not depend on
// the eye's view and FOV having been verified for this frame. Returns PC_VR_LAYER_DRAW_EYE when it is, 0 when it is
// another layer (the pause menu's) once a pair or a note is known, and PC_VR_LAYER_DRAW_MAYBE_EYE when that cannot be
// told: neither a pair nor a note is known yet, or this thread's render scopes are not to be trusted.
static uint32_t render_scope_eye_layer(void) {
    RenderScopes *tls = render_tls();
    if (!tls->poisoned && !tls->depth) return 0;  // (not rendering a layer)
    if (tls->poisoned || !tls->scopes[tls->depth - 1].identity) return PC_VR_LAYER_DRAW_MAYBE_EYE;
    const RenderScope *scope = &tls->scopes[tls->depth - 1];
    pthread_mutex_lock(&projection_lock);
    const ProjectionNote *note = projection_find_note(scope->projection);
    const bool note_layer = note && note->layer == scope->layer && scope->camera == note->layer + 0x228;
    const ProjectionDisplayPair *pair = &projection_display_pair;
    const bool notes_known = projection_notes[0].note || projection_notes[1].note;
    // The display's pair speaks only until an eye note exists: while the pause menu is open the display alternates
    // between the world's pair and the menu's own pair of layers through the same calls (Link, 2026-11-12), which only
    // an eye note's base camera (the play camera) tells apart. (As an eye note's layer: the scope's camera and
    // projection are the layer's own.)
    const bool pair_known = !notes_known && pair->layers[0] && pair->layers[1];
    const bool pair_layer = pair_known && (scope->layer == pair->layers[0] || scope->layer == pair->layers[1]) &&
        scope->camera == scope->layer + 0x228 && scope->projection == scope->layer + 0x288;
    const bool none_known = !pair_known && !notes_known;
    pthread_mutex_unlock(&projection_lock);
    return note_layer || pair_layer ? PC_VR_LAYER_DRAW_EYE : none_known ? PC_VR_LAYER_DRAW_MAYBE_EYE : 0;
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

void rt_layer_draw_end(uint32_t previous) { render_layer_draw = previous; }

uint32_t pc_vr_layer_draw_recording(void) { return render_layer_draw; }

void pc_vr_render_trace_invalidate_camera(uint64_t camera) {
    if (!atomic_load_explicit(&render_collecting, memory_order_relaxed)) return;
    projection_lock_for_change();
    for (unsigned i = 0; i < 2; ++i) {
        ProjectionNote *note = &projection_notes[i];
        if (note->note && camera == note->layer + 0x228) {
            note->render_view_current = false; note->render_certificate = 0;
        }
    }
    pthread_mutex_unlock(&projection_lock);
}

static void render_invalidate_projection(uint64_t object, bool device) {
    if (!atomic_load_explicit(&render_collecting, memory_order_relaxed)) return;
    projection_lock_for_change();
    ProjectionNote *note = projection_find_note(object);
    if (note) {
        note->render_device_current = false; note->render_certificate = 0;
        if (!device) note->render_logical_current = false;
    }
    pthread_mutex_unlock(&projection_lock);
}

static void projection_view_reject_locked(unsigned reason) {
    ProjectionTraversal *traversal = &projection_traversal;
    if (traversal->view_failed) return;
    traversal->view_failed = traversal->fov_failed = true;
    ++projection_view_rejected;
    if (projection_view_rejected <= 4)
        LOG("[pc-vr-native-view] rejected=%llu reason=%u native_provenance=0 image_admission=0\n",
            (unsigned long long)projection_view_rejected, reason);
    if (traversal->view_written_mask || traversal->fov_written_mask) projection_fov_finish_locked();
}

void pc_vr_projection_trace_head_result(uint64_t layer, uint64_t sequence, bool applied) {
    if (!atomic_load_explicit(&projection_fov_experiment, memory_order_relaxed)) return;
    projection_lock_for_change();
    ProjectionNote *note = projection_find_note(layer + 0x288);
    if (note && note->layer == layer && note->sequence == sequence && !note->head_acknowledged) {
        note->head_acknowledged = true;
        note->head_applied = applied;
        if (!applied && note->traversal_serial == projection_traversal.serial &&
            !projection_traversal.view_skipped) projection_view_reject_locked(1);
    }
    pthread_mutex_unlock(&projection_lock);
}

static void projection_fov_reject_locked(unsigned reason) {
    ++projection_fov_rejected;
    projection_traversal.fov_failed = true;
    if (projection_fov_rejected <= 4)
        LOG("[pc-vr-fov-experiment] rejected=%llu reason=%u native_provenance=0 image_admission=0\n",
            (unsigned long long)projection_fov_rejected, reason);
    if (projection_traversal.fov_written_mask || projection_traversal.view_written_mask)
        projection_fov_finish_locked();
}

PcVrNativeViewResult pc_vr_projection_trace_native_view(uint64_t layer, uint64_t sequence,
    const PcVrHeadLookCamera *center, const PcVrHeadLookCamera *original_eye,
    const PcVrNativeViewReference *reference, PcVrNativeViewToken *token) {
    if (!atomic_load_explicit(&projection_view_experiment, memory_order_relaxed))
        return PC_VR_NATIVE_VIEW_LEGACY;
    projection_lock_for_change();
    ProjectionTraversal *traversal = &projection_traversal;
    ProjectionNote *note = projection_find_note(layer + 0x288);
    if (!atomic_load_explicit(&projection_view_experiment, memory_order_relaxed) ||
        !atomic_load_explicit(&projection_enabled, memory_order_relaxed)) {
        pthread_mutex_unlock(&projection_lock); return PC_VR_NATIVE_VIEW_LEGACY;
    }
    const bool identity = note && note->layer == layer && note->sequence == sequence &&
        note->traversal_serial && note->traversal_serial == traversal->serial &&
        pthread_equal(traversal->thread, pthread_self());
    const bool root = identity && note->note == traversal->root.note;
    // The root decides warmup for both eyes. A legacy root can establish q0,
    // but its sibling must not start half a native pair using that new q0.
    if (identity && (traversal->view_skipped || (root && !reference && !traversal->view_admitted))) {
        traversal->view_skipped = true;
        pthread_mutex_unlock(&projection_lock); return PC_VR_NATIVE_VIEW_SKIPPED;
    }
    const uint64_t now = xr_runtime_pose_clock_ns();
    ProjectionNote *root_note = &projection_notes[traversal->root.side];
    bool valid = identity && center && original_eye && reference && token && !note->native_view_attempted &&
        !note->head_acknowledged && !traversal->view_failed && !traversal->fov_failed && traversal->native_latched &&
        note->native_fov_ready && note->latch_valid && note->frame == traversal->native_snapshot.frame_id &&
        note->generation == traversal->native_snapshot.generation &&
        projection_hash(center, sizeof *center) == note->base_hash && now >= note->time_ns &&
        now - note->time_ns <= PROJ_POSE_MAX_AGE_NS &&
        projection_native_pose(&traversal->native_snapshot, now) && projection_identity(note) &&
        projection_pose_current(&traversal->native_snapshot);
    if (identity) note->native_view_attempted = true;
    valid = valid && (root ? (traversal->open && !traversal->view_admitted) :
        (traversal->view_admitted && !traversal->view_finished && traversal->native_pair && !traversal->open &&
         root_note->note == traversal->root.note && root_note->native_view_verified &&
         root_note->sequence == sequence && root_note->basis.offset_sign == -note->basis.offset_sign &&
         !memcmp(reference, &traversal->view_pair.reference, sizeof *reference) &&
         traversal->view_written_mask == (1u << traversal->root.side)));
    // Reject already-known FOV incompatibilities before changing a camera.
    // The original logical writer repeats these guards at its actual call.
    float perspective[9], logical[16], depth[2];
    uint16_t dirty = 0;
    uint32_t posture = UINT32_MAX;
    if (valid) valid = projection_read(note->projection + 0x98, perspective, sizeof perspective) &&
        pc_vr_projection_logical(perspective, logical) && projection_matrix_contract(perspective, logical) &&
        projection_read(note->projection + 8, &dirty, sizeof dirty) && (dirty & 0xff) == 1 && (dirty >> 8) <= 1 &&
        projection_read(note->projection + 0x8c, &posture, sizeof posture) && posture == 0 &&
        projection_read(note->projection + 0x90, depth, sizeof depth) && isfinite(depth[0]) && isfinite(depth[1]);
    if (valid && root) valid = pc_vr_native_view_build_pair(center, reference,
        &traversal->native_snapshot, projection_view_scale, &traversal->view_pair);
    if (valid && root) {
        double focus = 0;
        for (unsigned i = 0; i < 3; ++i) focus += pow((double)center->target[i] - center->position[i], 2);
        focus = sqrt(focus) / projection_view_scale;
        if (isfinite(focus)) atomic_store_explicit(&projection_focus_m, focus, memory_order_relaxed);
    }
    PcVrHeadLookCamera original, readback;
    const uint64_t camera_fields = layer + 0x260;
    if (valid) valid = projection_read(camera_fields, &original, sizeof original) &&
        !memcmp(&original, original_eye, sizeof original);
    if (!valid) {
        projection_view_reject_locked(2);
        pthread_mutex_unlock(&projection_lock); return PC_VR_NATIVE_VIEW_REJECTED;
    }
    if (root) traversal->view_admitted = true;
    const int native_index = note->basis.offset_sign < 0 ? 0 : 1;
    PcVrHeadLookCamera *mapped = &traversal->view_pair.eyes[native_index];
    const bool written = labo_guest_copy(camera_fields, mapped, sizeof *mapped, true);
    const bool verified = written && projection_read(camera_fields, &readback, sizeof readback) &&
        !memcmp(&readback, mapped, sizeof readback);
    if (!verified) {
        // A failed host copy may touch a prefix. Restore the original fresh
        // camera before its original builder runs; never repair its matrix.
        const bool restored = labo_guest_copy(camera_fields, &original, sizeof original, true) &&
            projection_read(camera_fields, &readback, sizeof readback) &&
            !memcmp(&readback, &original, sizeof readback);
        atomic_store_explicit(&projection_view_experiment, false, memory_order_release);
        atomic_store_explicit(&projection_fov_experiment, false, memory_order_release);
        projection_view_reject_locked(3);
        pthread_mutex_unlock(&projection_lock);
        if (!restored) FATAL("native view experiment could not restore owned camera after a failed write\n");
        return PC_VR_NATIVE_VIEW_REJECTED;
    }
    note->native_fields_applied = true;
    traversal->view_written_mask |= 1u << note->side;
    *token = (PcVrNativeViewToken){.note = note->note, .traversal_serial = traversal->serial,
        .layer = layer, .sequence = sequence, .reference = *reference};
    pthread_mutex_unlock(&projection_lock);
    return PC_VR_NATIVE_VIEW_APPLIED;
}

bool pc_vr_projection_trace_native_view_built(const PcVrNativeViewToken *token,
    bool calibration_current) {
    if (!token || !token->note) return false;
    projection_lock_for_change();
    ProjectionTraversal *traversal = &projection_traversal;
    ProjectionNote *note = projection_find_note(token->layer + 0x288);
    const uint64_t now = xr_runtime_pose_clock_ns();
    bool valid = calibration_current && note && note->note == token->note && note->layer == token->layer &&
        note->sequence == token->sequence && note->traversal_serial == token->traversal_serial &&
        token->traversal_serial == traversal->serial && traversal->view_admitted && !traversal->view_failed &&
        !traversal->view_finished && note->native_fields_applied && !note->native_view_verified &&
        pthread_equal(traversal->thread, pthread_self()) &&
        !memcmp(&token->reference, &traversal->view_pair.reference, sizeof token->reference) &&
        projection_identity(note) && projection_native_pose(&traversal->native_snapshot, now) &&
        projection_pose_current(&traversal->native_snapshot);
    PcVrHeadLookCamera readback;
    float matrix[12];
    double origin_residual = 0;
    if (valid) {
        const unsigned native_index = note->basis.offset_sign < 0 ? 0 : 1;
        double baseline[3];
        for (unsigned i = 0; i < 3; ++i)
            baseline[i] = (double)traversal->view_pair.eyes[1].position[i] - traversal->view_pair.eyes[0].position[i];
        valid = projection_read(token->layer + 0x260, &readback, sizeof readback) &&
            !memcmp(&readback, &traversal->view_pair.eyes[native_index], sizeof readback) &&
            projection_read(token->layer + 0x230, matrix, sizeof matrix) &&
            pc_vr_native_view_matrix_matches(&readback, matrix, pc_vr_native_view_length(baseline), &origin_residual);
    }
    if (!valid) {
        // Field writes have already happened and the original builder has run.
        // No second builder invocation or guessed matrix repair is permitted.
        // This traversal is left without a certificate.
        projection_view_reject_locked(4);
    } else {
        note->native_view_verified = true;
        note->render_view_current = true;
        note->render_camera_hash = projection_hash(&readback, sizeof readback);
        note->render_view_hash = projection_hash(matrix, sizeof matrix);
        traversal->view_verified_mask |= 1u << note->side;
    }
    pthread_mutex_unlock(&projection_lock);
    return valid;
}

// Returns a note token only after one verified 28-byte parameter mutation.
// The caller invokes the original writer exactly once, regardless of outcome.
static uint64_t projection_prepare_native_fov(uint64_t object) {
    if (!atomic_load_explicit(&projection_fov_experiment, memory_order_relaxed)) return 0;
    projection_lock_for_change();
    ProjectionNote *note = projection_find_note(object);
    if (!atomic_load_explicit(&projection_enabled, memory_order_relaxed) ||
        !atomic_load_explicit(&projection_fov_experiment, memory_order_relaxed) || !note || note->fov_attempted) {
        pthread_mutex_unlock(&projection_lock); return 0;
    }
    ProjectionTraversal *traversal = &projection_traversal;
    if (traversal->view_skipped && note->traversal_serial == traversal->serial) {
        pthread_mutex_unlock(&projection_lock); return 0;
    }
    note->fov_attempted = true;
    const bool root = traversal->root.note == note->note;
    const uint64_t now = xr_runtime_pose_clock_ns();
    const unsigned side_bit = 1u << note->side;
    const int native_index = note->basis.offset_sign < 0 ? 0 : 1;
    ProjectionNote *root_note = &projection_notes[traversal->root.side];
    ProjectionNote *sibling_note = &projection_notes[1 - traversal->root.side];
    // (the FOV follows the native view: each eye's camera must be its verified native view)
    const bool current_camera = traversal->view_admitted && !traversal->view_failed && note->native_view_verified;
    const bool closed_pair_ready = traversal->native_pair && sibling_note->note == traversal->root.note + 1 &&
        sibling_note->traversal_serial == traversal->serial && sibling_note->native_view_verified &&
        sibling_note->native_fov_ready && sibling_note->latch_valid &&
        sibling_note->sequence == note->sequence && sibling_note->frame == note->frame &&
        sibling_note->generation == note->generation &&
        sibling_note->basis.offset_sign == -note->basis.offset_sign && projection_identity(sibling_note);
    bool valid = !traversal->fov_failed && note->traversal_serial && note->traversal_serial == traversal->serial &&
        traversal->native_latched && pthread_equal(traversal->thread, pthread_self()) && note->native_fov_ready &&
        current_camera && note->latch_valid && note->frame == traversal->native_snapshot.frame_id &&
        note->generation == traversal->native_snapshot.generation && now >= note->time_ns &&
        now - note->time_ns <= PROJ_POSE_MAX_AGE_NS &&
        projection_native_pose(&traversal->native_snapshot, now) && projection_identity(note) &&
        projection_pose_current(&traversal->native_snapshot);
    // Camera setup may finish both siblings before either projection writer.
    // `open` tracks observation, not rendering; a verified closed native pair
    // is equally eligible for its root's first writer.
    valid = valid && (root ? ((traversal->open || closed_pair_ready) && !traversal->fov_admitted) :
        (traversal->fov_admitted && traversal->native_pair && !traversal->open && !traversal->fov_finished &&
         root_note->note == traversal->root.note && root_note->fov_device_ok &&
         root_note->sequence == note->sequence && root_note->fov_native_index != native_index &&
         traversal->fov_device_mask == (1u << traversal->root.side) && !(traversal->fov_written_mask & side_bit)));
    float original[9], mapped[9], original_logical[16], depth[2];
    uint16_t dirty = 0;
    uint32_t posture = UINT32_MAX;
    if (valid) valid = projection_read(object + 0x98, original, sizeof original) &&
        projection_read(object + 8, &dirty, sizeof dirty) && (dirty & 0xff) == 1 && (dirty >> 8) <= 1 &&
        projection_read(object + 0x8c, &posture, sizeof posture) && posture == 0 &&
        projection_read(object + 0x90, depth, sizeof depth) && isfinite(depth[0]) && isfinite(depth[1]);
    // The only initial device posture admitted is the source/live-verified
    // unrotated posture 0. All conversion fields remain owned by the game.
    if (valid)
        valid = pc_vr_projection_logical(original, original_logical) &&
            pc_vr_projection_from_fov(traversal->native_snapshot.eyes[native_index].fov_left_right_up_down, original,
                                      mapped);
    if (!valid) {
        projection_fov_reject_locked(1);
        pthread_mutex_unlock(&projection_lock); return 0;
    }
    if (root) traversal->fov_admitted = true;
    memcpy(note->fov_original, original, sizeof original);
    memcpy(note->fov_mapped, mapped, sizeof mapped);
    memcpy(note->fov_angles, traversal->native_snapshot.eyes[native_index].fov_left_right_up_down,
           sizeof note->fov_angles);
    memcpy(note->fov_depth, depth, sizeof depth);
    note->fov_native_index = native_index;
    float readback[9];
    uint16_t dirty_after = 0;
    const bool written = labo_guest_copy(object + 0xa0, mapped + 2, 7 * sizeof(float), true);
    const bool verified = written && projection_read(object + 0x98, readback, sizeof readback) &&
        !memcmp(readback, mapped, sizeof mapped) && projection_read(object + 8, &dirty_after, sizeof dirty_after) &&
        dirty_after == dirty;
    if (!verified) {
        // A failed OS copy may have changed a prefix. Restore and verify the
        // exact original fields before letting the original writer continue.
        const bool restored = labo_guest_copy(object + 0xa0, original + 2, 7 * sizeof(float), true) &&
            projection_read(object + 0x98, readback, sizeof readback) && !memcmp(readback, original, sizeof original) &&
            projection_read(object + 8, &dirty_after, sizeof dirty_after) && dirty_after == dirty;
        atomic_store_explicit(&projection_fov_experiment, false, memory_order_release);
        projection_fov_reject_locked(2);
        pthread_mutex_unlock(&projection_lock);
        if (!restored) FATAL("native FOV experiment could not restore owned parameters after a failed write\n");
        return 0;
    }
    note->fov_applied = true;
    traversal->fov_written_mask |= side_bit;
    const uint64_t token = note->note;
    pthread_mutex_unlock(&projection_lock);
    return token;
}

static void projection_verify_native_fov(uint64_t object, bool device, uint64_t token) {
    if (!atomic_load_explicit(&projection_enabled, memory_order_relaxed)) return;
    projection_lock_for_change();
    ProjectionNote *note = projection_find_note(object);
    if (!note || !note->fov_applied || (token && token != note->note) ||
        (device ? note->fov_device_ok : note->fov_logical_ok)) {
        pthread_mutex_unlock(&projection_lock); return;
    }
    ProjectionTraversal *traversal = &projection_traversal;
    float parameters[9], logical[16], converted[16], depth[2];
    uint32_t posture = UINT32_MAX;
    bool valid = note->traversal_serial == traversal->serial && !traversal->fov_failed && projection_identity(note) &&
        projection_read(object + 0x98, parameters, sizeof parameters) &&
        !memcmp(parameters, note->fov_mapped, sizeof parameters) &&
        projection_read(object + 0xc, logical, sizeof logical) &&
        projection_matrix_contract(parameters, logical) && pc_vr_projection_native_rays(note->fov_angles, logical);
    if (device && valid) {
        valid = note->fov_logical_ok && projection_read(object + 0x4c, converted, sizeof converted) &&
            projection_read(object + 0x8c, &posture, sizeof posture) && posture == 0 &&
            projection_read(object + 0x90, depth, sizeof depth) && !memcmp(depth, note->fov_depth, sizeof depth);
        if (valid) for (unsigned i = 0; i < 16; ++i) valid &= isfinite(converted[i]);
        if (valid) for (unsigned i = 0; i < 8; ++i) valid &= projection_near(converted[i], logical[i]);
        if (valid) for (unsigned i = 12; i < 16; ++i) valid &= projection_near(converted[i], logical[i]);
        valid = valid && projection_near(converted[8], depth[0] * (double)logical[8]) &&
            projection_near(converted[9], depth[0] * (double)logical[9]) &&
            projection_near(converted[10], depth[0] * ((double)logical[10] + depth[1] * logical[14])) &&
            projection_near(converted[11], logical[11] * (double)depth[0] + depth[1] * logical[15]);
    }
    if (!valid) {
        LOG("[pc-vr-fov-experiment] verified_stage=%s eye=%llu native_rays=0 original_conversion=0 native_provenance=0 image_admission=0\n",
            device ? "device" : "logical", (unsigned long long)note->note);
        projection_fov_reject_locked(3);
    } else if (device) {
        note->fov_device_ok = true; traversal->fov_device_mask |= 1u << note->side;
        note->render_device_current = true;
        render_publish_locked(note);
        if (traversal->fov_device_mask == 3) projection_fov_finish_locked();
    } else {
        note->fov_logical_ok = true; traversal->fov_logical_mask |= 1u << note->side;
        note->render_logical_current = true;
    }
    pthread_mutex_unlock(&projection_lock);
}

static void projection_traversal_reject(void) {
    ++projection_stack_rejected;
    memset(&projection_traversal, 0, sizeof projection_traversal);
    if (projection_stack_rejected <= 3)
        LOG("[pc-vr-traversal] rejected=%llu pending_cleared=1 native_provenance=0\n",
            (unsigned long long)projection_stack_rejected);
}

// The pinned ddb820 frame saves FP/LR at SP+0x40/+0x48 and sets FP=SP+0x40.
// At the fresh-eye virtual call, root LR is dde6f0; direct sibling recursion
// saves LR ddbdd8 and the root FP. No guest address or raw snapshot is logged.
static void projection_observe_traversal(ProjectionNote *note, uint64_t frame, const XrPoseSnapshot *head_pose) {
    uint64_t saved[2] = {0};
    if (!projection_saved_frame(frame, saved)) {
        projection_traversal_reject(); return;
    }
    if (saved[1] == PROJ_ROOT_RETURN) {
        memset(&projection_traversal, 0, sizeof projection_traversal);  // (an open earlier root is abandoned)
        // Fail closed on identity exhaustion; wrapping would permit old IDs.
        if (projection_root_serial == UINT64_MAX) {
            projection_traversal_reject();
            return;
        }
        note->traversal_serial = ++projection_root_serial;
        ++projection_roots;
        if (!projection_config_address(note->config) || note->sibling == note->layer || (note->sibling & 7) ||
            note->sibling < GUEST_LO || note->sibling > GUEST_HI - 0x448) {
            projection_traversal_reject(); return;
        }
        ProjectionTraversal *traversal = &projection_traversal;
        traversal->root = *note;
        traversal->frame_pointer = frame;
        traversal->serial = note->traversal_serial;
        traversal->thread = pthread_self();
        traversal->open = true;
        // Retain the base camera's complete sample. A display-rate publication
        // between the base and this root must not substitute another pose/FOV.
        // Still check current tracking/generation so loss or recenter fails closed.
        const bool head_matches = head_pose && note->latch_valid &&
            head_pose->frame_id == note->frame && head_pose->generation == note->generation;
        if (head_matches && projection_pose_current(head_pose)) {
            traversal->native_snapshot = *head_pose; // Exactly once for this traversal.
            traversal->native_latched = true;
        }
        if (projection_roots <= 4 || !(projection_roots % 16))
            LOG("[pc-vr-traversal] root=%llu serial=%llu root_return=1 native_latched=%d native_matches_head=%d native_provenance=0\n",
                (unsigned long long)projection_roots, (unsigned long long)traversal->serial,
                traversal->native_latched, traversal->native_latched && note->latch_valid &&
                head_matches);
        return;
    }
    ProjectionTraversal *traversal = &projection_traversal;
    const ProjectionNote *root = &traversal->root;
    const bool stack_link = traversal->open &&
        projection_sibling_call(saved, traversal->frame_pointer, traversal->thread);
    const bool layer_link = stack_link && root->sibling == note->layer && root->side != note->side &&
        root->note + 1 == note->note && root->config == note->config && root->base == note->base &&
        root->base_hash == note->base_hash;
    if (!layer_link) {
        projection_traversal_reject();
        return;
    }
    // Revalidate the root's still-live guest relation. The original sibling
    // may have no reverse link; require the source-backed root->sibling link.
    if (!projection_identity(root) ||
        !projection_live_link(root->layer, traversal->frame_pointer, note->layer, note->config)) {
        projection_traversal_reject(); return;
    }
    note->traversal_serial = traversal->serial;
    traversal->open = false; // A duplicate/delayed sibling cannot consume it again.
    ++projection_traversal_pairs;
    // A new publication in the same generation must not replace the root
    // snapshot. Lost tracking, recenter/reset, BUSY or age reject the pair.
    if (traversal->native_latched) traversal->native_pair = projection_pose_current(&traversal->native_snapshot);
    if (projection_traversal_pairs <= 4 || !(projection_traversal_pairs % 16))
        LOG("[pc-vr-traversal] pair=%llu serial=%llu saved_frame_link=1 exact_layer_config_base=1 opposite_sides=1 same_base_pose=1 same_head_latch=%d native_snapshot_retained=%d native_pair_valid=%d same_simulation_proven=0 native_provenance=0\n",
            (unsigned long long)projection_traversal_pairs, (unsigned long long)traversal->serial,
            root->sequence && root->sequence == note->sequence && root->frame == note->frame &&
            root->generation == note->generation && root->latch_valid && note->latch_valid,
            traversal->native_latched, traversal->native_pair);
}

// A layer that is an eye layer by its own objects, without the base camera an eye note also checks
// (projection_identity): an aligned guest object whose selected projection (+0x220) is its own (+0x288, of the
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
    // Once an eye note exists the notes decide (render_scope_eye_layer): the pair is no longer followed, so the pause
    // menu's own pair, shown through the same calls, does not replace it.
    if (!atomic_load_explicit(&projection_enabled, memory_order_relaxed) || projection_notes[0].note ||
        projection_notes[1].note) {
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
        atomic_fetch_add_explicit(&render_state_version, 1, memory_order_release);
        pair->layers[0] = layers[0];
        pair->layers[1] = layers[1];
        ++projection_display_pairs;
        if (projection_display_pairs <= 8)
            LOG("[pc-vr-traversal] eye layers known from the display's pair (pair %llu of at most 8 logged; eye notes "
                "known=%d)\n", (unsigned long long)projection_display_pairs,
                projection_notes[0].note || projection_notes[1].note);
    }
    pthread_mutex_unlock(&projection_lock);
}

static void projection_observe_native_fov(ProjectionNote *note) {
    ++projection_fov_attempts;
    const ProjectionTraversal *traversal = &projection_traversal;
    const bool latched = note->traversal_serial && note->traversal_serial == traversal->serial &&
        traversal->native_latched && ((traversal->open && traversal->root.note == note->note) || traversal->native_pair);
    const XrPoseSnapshot *pose = &traversal->native_snapshot;
    const bool native_order = latched && pc_vr_projection_native_order(pose->orientation_xyzw,
        pose->eyes[0].position_xyz, pose->eyes[1].position_xyz);
    const bool basis = note->basis_read && note->basis.right_matches && note->basis.parallel_eye &&
        note->basis.lateral_eye && note->basis.offset_sign;
    const int native_index = basis && native_order ? (note->basis.offset_sign < 0 ? 0 : 1) : -1;
    float original[9], mapped[9], logical[16];
    uint16_t dirty = 0;
    const bool original_read = projection_read(note->projection + 0x98, original, sizeof original) &&
        projection_read(note->projection + 8, &dirty, sizeof dirty);
    const bool mapped_ok = original_read && native_index >= 0 &&
        pc_vr_projection_from_fov(pose->eyes[native_index].fov_left_right_up_down, original, mapped);
    const bool preserved_depth = mapped_ok && !memcmp(original, mapped, 2 * sizeof(float));
    const bool logical_contract = mapped_ok && pc_vr_projection_logical(mapped, logical) &&
        projection_matrix_contract(mapped, logical);
    note->native_fov_ready = preserved_depth && logical_contract && (dirty & 0xff) != 0;
    if (projection_fov_attempts <= 8 || !(projection_fov_attempts % 32))
        LOG("[pc-vr-native-fov] eye=%llu side_bit=%u right_basis=%d parallel_eye=%d lateral_eye=%d basis_offset_sign=%d native_stereo_order=%d candidate_native_index=%d logical_dirty=%d near_far_preserved=%d dry_run_contract=%d guest_writes=0 resource_mapping_proven=0 native_provenance=0\n",
            (unsigned long long)note->note, note->side, note->basis_read && note->basis.right_matches,
            note->basis_read && note->basis.parallel_eye, note->basis_read && note->basis.lateral_eye,
            note->basis_read ? note->basis.offset_sign : 0, native_order, native_index,
            original_read && (dirty & 0xff) != 0, preserved_depth, logical_contract);
}

void pc_vr_projection_trace_eye(uint64_t layer, uint64_t bound_base,
    uint64_t sequence, uint64_t frame, uint64_t generation, bool latch_valid, uint64_t guest_frame_pointer,
    const XrPoseSnapshot *head_pose) {
    if (!atomic_load_explicit(&projection_enabled, memory_order_relaxed)) return;
    projection_lock_for_change();
    if (!atomic_load_explicit(&projection_enabled, memory_order_relaxed)) {
        pthread_mutex_unlock(&projection_lock); return;
    }
    const uint64_t now = xr_runtime_pose_clock_ns();
    // (the projection writers are observed from the first eye on: projection_observe_matrix)
    if (!atomic_load_explicit(&projection_started, memory_order_relaxed))
        atomic_store_explicit(&projection_started, true, memory_order_release);
    ProjectionNote note = {.layer = layer, .base = bound_base, .projection = layer + 0x288,
        .sequence = sequence, .frame = frame, .generation = generation, .latch_valid = latch_valid,
        .time_ns = now};
    uint16_t flags = 0;
    float base_pose[9], eye_position[3], right[3];
    bool valid = layer >= GUEST_LO && layer <= GUEST_HI - 0x448 &&
        projection_read(layer + 0x90, &flags, sizeof flags);
    note.side = !!(flags & 0x1000);
    valid = valid && projection_identity(&note) &&
        projection_read(layer + 0x210, &note.config, sizeof note.config) &&
        projection_read(layer + 0x208, &note.sibling, sizeof note.sibling) &&
        projection_read(bound_base + 0x38, base_pose, sizeof base_pose) &&
        projection_read(bound_base + 8, right, sizeof right) &&
        projection_read(layer + 0x260, eye_position, sizeof eye_position);
    double signed_offset = 0;
    if (valid) for (unsigned i = 0; i < 3; ++i) {
        valid &= isfinite(base_pose[i]) && isfinite(right[i]) && isfinite(eye_position[i]);
        signed_offset += ((double)eye_position[i] - base_pose[i]) * right[i];
    }
    if (valid) for (unsigned i = 0; i < 9; ++i) valid &= isfinite(base_pose[i]);
    if (!valid) {
        projection_traversal_reject();
        ++projection_rejected;
        if (projection_rejected <= 3) LOG("[pc-vr-projection] bound-eye rejected=%llu native_provenance=0\n",
            (unsigned long long)projection_rejected);
        pthread_mutex_unlock(&projection_lock);
        return;
    }
    note.offset_sign = signed_offset > 1e-5 ? 1 : signed_offset < -1e-5 ? -1 : 0;
    note.base_hash = projection_hash(base_pose, sizeof base_pose);
    note.note = ++projection_note_count;
    if (note.note <= 4) {
        uint64_t vtable = 0, begin = 0, end = 0;
        const uint64_t main_end = g_site.main_end;
        const bool callbacks = projection_read(layer, &vtable, sizeof vtable) &&
            vtable >= MAIN_BASE && vtable <= main_end - 0x70 && !(vtable & 7) &&
            projection_read(vtable + 0x50, &begin, sizeof begin) &&
            projection_read(vtable + 0x68, &end, sizeof end) &&
            begin >= MAIN_BASE && begin < main_end && !(begin & 3) &&
            end >= MAIN_BASE && end < main_end && !(end & 3);
        LOG("[pc-vr-render-scope] eye=%llu side_bit=%u callbacks_in_pinned_main=%d begin_offset=%llx end_offset=%llx live_rinfo_join=0 native_provenance=0\n",
            (unsigned long long)note.note, note.side, callbacks,
            (unsigned long long)(callbacks ? begin - MAIN_BASE : 0),
            (unsigned long long)(callbacks ? end - MAIN_BASE : 0));
    }
    PcVrHeadLookCamera original_base, original_eye;
    memcpy(&original_base, base_pose, sizeof original_base);
    note.basis_read = projection_read(layer + 0x260, &original_eye, sizeof original_eye) &&
        pc_vr_projection_eye_basis(&original_base, &original_eye, right, &note.basis);
    projection_observe_traversal(&note, guest_frame_pointer, head_pose);
    projection_observe_native_fov(&note);
    projection_notes[note.side] = note;
    if (note.note <= 8 || !(note.note % 32)) {
        uint16_t dirty = 0;
        float params[9], matrix[16];
        const bool inspected = projection_read(note.projection + 8, &dirty, sizeof dirty) &&
            projection_read(note.projection + 0x98, params, sizeof params) &&
            projection_read(note.projection + 0xc, matrix, sizeof matrix);
        const bool existing_contract = inspected && projection_matrix_contract(params, matrix);
        LOG("[pc-vr-projection] eye=%llu side_bit=%u offset_sign=%d selected_owned_projection=1 latch_valid=%d sequence=%llu tracking_frame=%llu generation=%llu dirty_bits=%u existing_matrix_contract=%d native_provenance=0\n",
            (unsigned long long)note.note, note.side, note.offset_sign, note.latch_valid,
            (unsigned long long)sequence, (unsigned long long)frame, (unsigned long long)generation,
            inspected ? dirty : UINT16_MAX, existing_contract);
    }
    const ProjectionNote *other = &projection_notes[1 - note.side];
    if (other->note && other->note + 1 == note.note && other->note > projection_last_pair) {
        projection_last_pair = note.note;
        ++projection_pairs;
        if (projection_pairs <= 4 || !(projection_pairs % 16))
            LOG("[pc-vr-projection] pair=%llu opposite_sides=1 same_config=%d sibling_link=%d same_base_pose=%d same_latch=%d same_tracking=%d same_simulation_proven=0\n",
                (unsigned long long)projection_pairs, note.config && note.config == other->config,
                note.sibling == other->layer || other->sibling == note.layer,
                note.base == other->base && note.base_hash == other->base_hash,
                note.sequence && note.sequence == other->sequence,
                note.latch_valid && other->latch_valid && note.frame == other->frame && note.generation == other->generation);
    }
    pthread_mutex_unlock(&projection_lock);
}

static bool projection_near(double actual, double expected) {
    return isfinite(actual) && isfinite(expected) && fabs(actual - expected) <= 0.00002 * fmax(1.0, fabs(expected));
}

// Class fields n,f,fovy,sinHalf,cosHalf,tanHalf,aspect,centerX,centerY.
// Validate their relationships without logging any raw field/matrix payload.
static bool projection_matrix_contract(const float params[9], const float matrix[16]) {
    for (unsigned i = 0; i < 9; ++i) if (!isfinite(params[i])) return false;
    if (!(params[0] > 0 && params[1] > params[0] && params[2] > 0 && params[2] < 3.14159265 &&
          params[5] > 0 && params[6] > 0)) return false;
    if (!projection_near(params[3], sin(params[2] * 0.5)) ||
        !projection_near(params[4], cos(params[2] * 0.5)) ||
        !projection_near(params[5], tan(params[2] * 0.5))) return false;
    const double near_plane = params[0], far_plane = params[1];
    const double expected[16] = {1.0 / (params[5] * (double)params[6]), 0, 2.0 * params[7], 0,
        0, 1.0 / params[5], 2.0 * params[8], 0, 0, 0, -(far_plane + near_plane) / (far_plane - near_plane),
        -2.0 * far_plane * near_plane / (far_plane - near_plane), 0, 0, -1, 0};
    for (unsigned i = 0; i < 16; ++i) if (!projection_near(matrix[i], expected[i])) return false;
    return true;
}

static void projection_observe_matrix(uint64_t object, bool device) {
    if (!atomic_load_explicit(&projection_enabled, memory_order_relaxed) ||
        !atomic_load_explicit(&projection_started, memory_order_acquire)) return;
    projection_lock_for_change();
    if (!atomic_load_explicit(&projection_enabled, memory_order_relaxed)) {
        pthread_mutex_unlock(&projection_lock); return;
    }
    const uint64_t now = xr_runtime_pose_clock_ns();
    uint64_t *counter = device ? &projection_device_writes : &projection_writes;
    ProjectionNote *note = NULL;
    for (unsigned i = 0; i < 2; ++i)
        if (projection_notes[i].note && projection_notes[i].projection == object) note = &projection_notes[i];
    if (!note || now < note->time_ns || now - note->time_ns > UINT64_C(250000000) || !projection_identity(note)) {
        pthread_mutex_unlock(&projection_lock);
        return;
    }
    float params[9], matrix[16], device_matrix[16], depth[2];
    uint32_t posture = 0;
    bool valid = projection_read(object + 0x98, params, sizeof params) &&
        projection_read(object + 0xc, matrix, sizeof matrix);
    bool contract = valid && projection_matrix_contract(params, matrix);
    bool device_finite = false, depth_contract = false;
    if (device) {
        valid &= projection_read(object + 0x4c, device_matrix, sizeof device_matrix) &&
            projection_read(object + 0x90, depth, sizeof depth) &&
            projection_read(object + 0x8c, &posture, sizeof posture);
        device_finite = valid;
        if (valid) for (unsigned i = 0; i < 16; ++i) device_finite &= isfinite(device_matrix[i]);
        // da1034 applies device depth conversion after its posture-dependent
        // XY row transform. Report the independent depth formula for all modes.
        depth_contract = device_finite &&
            projection_near(device_matrix[8], depth[0] * (double)matrix[8]) &&
            projection_near(device_matrix[9], depth[0] * (double)matrix[9]) &&
            projection_near(device_matrix[10], depth[0] * ((double)matrix[10] + depth[1] * matrix[14])) &&
            projection_near(device_matrix[11], matrix[11] * (double)depth[0] + depth[1] * matrix[15]);
    } else if (!contract) ++projection_bad_matrix;
    if (note->native_fov_ready && (*counter < 8 || !((*counter + 1) % 32)))
        LOG("[pc-vr-native-fov] original_stage=%s eye=%llu dry_run_ready=1 logical_before_device=%d original_contract=%d experiment_applied=%d native_provenance=0\n",
            device ? "device" : "logical", (unsigned long long)note->note,
            device && note->logical_seen, contract && (!device || depth_contract), note->fov_applied);
    if (!device) note->logical_seen = contract;
    ++*counter;
    if (*counter <= 8 || !(*counter % 32) || (!contract && projection_bad_matrix <= 3))
        LOG("[pc-vr-projection] stage=%s sample=%llu side_bit=%u note=%llu read_ok=%d perspective_contract=%d device_finite=%d device_depth_contract=%d posture=%u parameter_hash=%016llx matrix_hash=%016llx native_provenance=0\n",
            device ? "device" : "perspective", (unsigned long long)*counter, note->side,
            (unsigned long long)note->note, valid, contract, device_finite, depth_contract, posture,
            (unsigned long long)(valid ? projection_hash(params, sizeof params) : 0),
            (unsigned long long)(valid ? projection_hash(device ? device_matrix : matrix, sizeof matrix) : 0));
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
    projection_lock_for_change();
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
    const uint64_t fov_token = exact ? projection_prepare_native_fov(object) : 0;
    projection_original(cpu);
    if (fov_token) projection_verify_native_fov(object, false, fov_token);
    if (exact) projection_observe_matrix(object, false);
    projection_list(object, caller);
}

static void projection_device_wrapper(Cpu *cpu) {
    const uint64_t object = cpu->x[0];
    render_invalidate_projection(object, true);
    const bool exact = cpu->x[30] == PROJ_DEVICE_CALLER && cpu->x[1] == object + 0x4c && cpu->x[2] == object + 0xc;
    projection_device_original(cpu);
    if (exact) projection_verify_native_fov(object, true, 0);
    if (exact) projection_observe_matrix(object, true);
}

double pc_vr_camera_focus_distance(void) {
    return atomic_load_explicit(&projection_focus_m, memory_order_relaxed);
}

bool pc_vr_world_scale_active(void) {
    return atomic_load_explicit(&projection_view_experiment, memory_order_acquire);
}

// Under projection_lock: the root eye builds its pair with one value, and its sibling uses that pair.
double pc_vr_world_scale(double factor) {
    projection_lock_for_change();
    double scale = 0;
    if (atomic_load_explicit(&projection_view_experiment, memory_order_relaxed) && projection_view_scale > 0) {
        projection_view_scale = factor > 0 ? fmin(100.0, fmax(0.1, projection_view_scale * factor)) : projection_launch_scale;
        scale = projection_view_scale;
    }
    pthread_mutex_unlock(&projection_lock);
    if (scale) LOG("[pc-vr-native-view] world scale %.4g game units per metre (world at 1:%.3g of life size)\n", scale, scale);
    return scale;
}

// PC VR on is the play configuration: all of it, continuously, at the configured world scale. Armed at each head look
// init, afresh: the eye notes, the traversal and the counts start over; published certificates stay resolvable. None of
// it is armed when a part cannot run (no PC VR sites for this game version, an original projection writer or render
// scope callback missing, a world scale that is not finite and above 0): head look then composes the eyes itself
// (PC_VR_NATIVE_VIEW_LEGACY) and no draw gets a certificate.
void pc_vr_projection_trace_init(void) {
    atomic_store_explicit(&render_collecting, false, memory_order_release);
    if (!pc_vr_enabled()) return;
    if (!pc_vr_main_compatible()) {
        LOG("[pc-vr-projection] disabled: no PC VR sites for this game version\n");
        return;
    }
    const double units_per_meter = pc_vr_config()->play.native_view_units_per_meter;
    if (!isfinite(units_per_meter) || units_per_meter <= 0) {
        LOG("[pc-vr-native-view] disabled: the world scale must be finite and above 0 game units per metre\n");
        return;
    }
    projection_lock_for_change();
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
    memset(projection_notes, 0, sizeof projection_notes);
    memset(&projection_traversal, 0, sizeof projection_traversal);
    memset(&projection_display_pair, 0, sizeof projection_display_pair);
    projection_display_pairs = 0;
    projection_note_count = projection_rejected = projection_writes = projection_device_writes = 0;
    projection_bad_matrix = projection_pairs = projection_last_pair = 0;
    projection_roots = projection_traversal_pairs = projection_stack_rejected = 0;
    projection_fov_attempts = projection_fov_rejected = projection_view_rejected = 0;
    projection_view_scale = projection_launch_scale = units_per_meter;
    atomic_fetch_add_explicit(&render_epoch, 1, memory_order_release);
    atomic_store_explicit(&render_enabled, true, memory_order_release);
    atomic_store_explicit(&render_collecting, true, memory_order_release);
    atomic_store_explicit(&projection_view_experiment, true, memory_order_release);
    atomic_store_explicit(&projection_fov_experiment, true, memory_order_release);
    atomic_store_explicit(&projection_started, false, memory_order_release);
    atomic_store_explicit(&projection_enabled, true, memory_order_release);
    pthread_mutex_unlock(&projection_lock);
    LOG("[pc-vr-eyes] armed continuous native view/FOV/certificates game_units_per_meter=%.9g live_scale_keys=minus,equals,0 certificate_slots=%u scope_cap=%u original_builder_retained=1 provenance=dev-unproven\n",
        units_per_meter, RENDER_CERTIFICATE_SLOTS, RENDER_SCOPE_CAP);
}

RT_ON_RESET(pc_vr_projection_trace_reset) {
    atomic_store_explicit(&render_collecting, false, memory_order_release);
    atomic_fetch_add_explicit(&render_epoch, 1, memory_order_release);
    atomic_store_explicit(&projection_enabled, false, memory_order_release);
    atomic_store_explicit(&projection_started, false, memory_order_release);
    projection_lock_for_change();
    atomic_store_explicit(&projection_fov_experiment, false, memory_order_release);
    atomic_store_explicit(&projection_view_experiment, false, memory_order_release);
    memset(projection_notes, 0, sizeof projection_notes);
    memset(&projection_traversal, 0, sizeof projection_traversal);
    memset(&projection_display_pair, 0, sizeof projection_display_pair);
    pthread_mutex_unlock(&projection_lock);
}
