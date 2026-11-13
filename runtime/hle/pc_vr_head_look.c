// PC VR head look (D037; native per-eye views D042): the headset's pose applied to the game's two Labo eye cameras,
// so the player looks around inside the game's third-person view while the game's own camera, and gameplay, stay as
// the game makes them.
//
// The game builds every camera's view matrix with one look-at builder (BUILDER), which reads a camera's position,
// target and up. This file wraps that builder (head_look_builder) and acts on two of its callers (their addresses:
// runtime/game_sites.h GAME_SITES_PC_VR):
//  - BASE_CALLER builds the base camera: the game's DefaultCamera, its third-person camera. After it runs, the base
//    camera and one complete headset pose sample are latched together (base_after_original, latch_camera_locked).
//  - EYE_CALLER builds each Labo eye camera. The game has just copied the base camera into the eye and moved its
//    position and target sideways by the eye's stereo offset. Before the original builder runs, that fresh copy is
//    given the eye's native view and FOV from the latched sample (pc_vr_projection_trace_native_view), or else is
//    rotated by the head's rotation relative to the calibrated heading (pc_vr_head_look_apply_eye).
// Both eyes of a game frame use the same latched sample, so the two eyes always agree. Anything unexpected (an
// unverified camera, a stale or missing pose, an eye that is not a fresh copy) leaves the eye as the game made it.
//
// Which camera is the base: the camera controller the game updates each frame (pc_vr_head_look_observe_controller,
// from pc_vr_camera_update.c) leads through its camera actor to it. Every guest object is checked by address range,
// alignment and vtable before it is used. Head look is part of the PC VR play configuration (runtime/pc_vr_config.h).
// The counters below feed bounded log summaries; the diagnostic SAVAGEWIND_PC_VR_HEAD_LOOK_TIMING=1 adds timing.
#include <math.h>
#include <float.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../gamelib.h"
#include "../game_sites.h"
#include "../hle.h"
#include "../pc_vr.h"
#include "../xr/xr_runtime.h"
static uint64_t memory_timing_begin(void);
static void memory_timing_end(unsigned operation, uint64_t start, bool ok);
static void memory_timing_reject(void);
#define LABO_GUEST_TIMING_BEGIN() memory_timing_begin()
#define LABO_GUEST_TIMING_END(operation, start, ok) memory_timing_end(operation, start, ok)
#define LABO_GUEST_TIMING_REJECT() memory_timing_reject()
#include "labo_guest_memory.h"
#undef LABO_GUEST_TIMING_BEGIN
#undef LABO_GUEST_TIMING_END
#undef LABO_GUEST_TIMING_REJECT
#include "pc_vr_head_look.h"
#include "pc_vr_render_trace.h"
#include "pc_vr_head_look_math.h"
#include "pc_vr_projection_trace.h"

#define BUILDER g_site.pcvr_matrix_builder
#define BASE_CALLER g_site.pcvr_base_caller
#define LAYER_BASE_CALLER g_site.pcvr_layer_base_caller
#define EYE_CALLER g_site.pcvr_eye_caller
#define ACTOR_VTABLE g_site.pcvr_actor_vtable
#define BASE_VTABLE g_site.pcvr_base_vtable
#define EYE_VTABLE g_site.pcvr_eye_vtable
#define POSE_MAX_AGE_NS UINT64_C(250000000)  // a pose sample older than 250 ms is not used
#define SUMMARY_CAP 120                        // log summaries, at most one a second

// Guest layouts (1.9.0) read here.
#define CAMERA_LOOK_AT 0x38        // a camera's look-at: PcVrHeadLookCamera (position, target, up), 0x24 bytes
#define CAMERA_END 0x5c            // CAMERA_LOOK_AT + its size: the camera bytes that must be in guest memory
#define CONTROLLER_ACTOR_REF 0x28  // the controller's weak reference to its camera actor (8-byte pointer)
#define ACTOR_REF_ACTOR 0x8        // the weak reference's pointer to the actor
#define ACTOR_BASE_CAMERA 0x1088   // the camera actor's base camera (DefaultCamera)
#define LAYER_EYE_CAMERA 0x228     // a Labo layer's eye camera, embedded in the layer
#define LAYER_SOURCE_CAMERA 0x80   // the camera the layer's eye copies (the base camera, for this composition)
#define LAYER_SIDE 0x90            // uint16: LAYER_SIDE_SECOND_EYE set for the second eye
#define LAYER_SIDE_SECOND_EYE 0x1000

// The camera chain this composition is bound to (read_binding): the controller, its weak reference to the camera
// actor, the actor, and the actor's base camera.

typedef struct {
    uint64_t controller, weak, actor, base;
} CameraBinding;

// The state, under look_lock.
static pthread_mutex_t look_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool look_enabled;
static bool registered;  // the builder is wrapped (once per process)
static GuestFn original_builder;
static CameraBinding binding;
static bool binding_valid, reference_valid, latch_valid;
static bool latch_root_owned;  // the latch was taken at a new camera traversal's root (eye_before_original)
// The reference: the D053 calibration of the latched pose (from the XR core), its heading (reference_q) and origin;
// reference_generation is its epoch.
static uint64_t reference_generation, latch_generation, latch_published_ns;
static uint64_t latch_frame_id, latch_sequence;
static float reference_q[4], reference_origin[3];
static float latch_relative_q[4];  // the head's rotation relative to the reference heading
// The latch: the base camera and one complete pose sample, used for both eyes of the frame.
static PcVrHeadLookCamera latch_base;
static XrPoseSnapshot latch_pose;
static unsigned latch_eye_count;  // eyes composed from this latch so far
static uint8_t latch_side_mask;   // which sides: 1 the first eye, 2 the second
static PcVrHeadLookCamera prior_base;  // the previous latched base camera (for the log)
static bool prior_base_valid;
static uint64_t summary_start_ns;
static unsigned summaries;
static bool summary_active;
static uint64_t base_calls, layer_base_calls, eye_calls, root_latches, applied, rejected, busy, unavailable;
static bool first_binding_logged, first_base_logged, first_eye_logged;
static _Atomic bool timing_enabled;
static bool timing_active;
static uint64_t timing_window_ns;
static unsigned timing_windows;
static _Atomic uint64_t timing_generic, timing_base_candidates, timing_eye_candidates;
static _Atomic uint64_t timing_bound_base, timing_bound_eye;
static _Atomic uint64_t timing_base_self_ns, timing_eye_self_ns;
static _Atomic uint64_t timing_observe_calls, timing_observe_ns;
static _Atomic uint64_t timing_original_samples, timing_original_sample_ns;
static _Atomic uint64_t timing_memory_calls[LABO_GUEST_TIMING_OPERATIONS];
static _Atomic uint64_t timing_memory_ns[LABO_GUEST_TIMING_OPERATIONS];
static _Atomic uint64_t timing_memory_failed[LABO_GUEST_TIMING_OPERATIONS];
static _Atomic uint64_t timing_guard_rejected;
// Only wrapper/observer work is attributed. In particular, an original builder
// (including the fixture's guarded reads) must never contribute to these totals.
static _Thread_local bool timing_memory_scope;

static uint64_t memory_timing_begin(void) {
    return timing_memory_scope ? plat_now_ns() : 0;
}

static void memory_timing_end(unsigned operation, uint64_t start, bool ok) {
    if (!timing_memory_scope) return;
    const uint64_t end = plat_now_ns();
    atomic_fetch_add(&timing_memory_calls[operation], 1);
    if (end >= start) atomic_fetch_add(&timing_memory_ns[operation], end - start);
    if (!ok) atomic_fetch_add(&timing_memory_failed[operation], 1);
}

static void memory_timing_reject(void) {
    if (timing_memory_scope) atomic_fetch_add(&timing_guard_rejected, 1);
}

static void reset_timing_counts(void) {
    atomic_store(&timing_generic, 0);
    atomic_store(&timing_base_candidates, 0);
    atomic_store(&timing_eye_candidates, 0);
    atomic_store(&timing_bound_base, 0);
    atomic_store(&timing_bound_eye, 0);
    atomic_store(&timing_base_self_ns, 0);
    atomic_store(&timing_eye_self_ns, 0);
    atomic_store(&timing_observe_calls, 0);
    atomic_store(&timing_observe_ns, 0);
    atomic_store(&timing_original_samples, 0);
    atomic_store(&timing_original_sample_ns, 0);
    for (unsigned i = 0; i < LABO_GUEST_TIMING_OPERATIONS; ++i) {
        atomic_store(&timing_memory_calls[i], 0);
        atomic_store(&timing_memory_ns[i], 0);
        atomic_store(&timing_memory_failed[i], 0);
    }
    atomic_store(&timing_guard_rejected, 0);
}

static bool read_u64(uint64_t address, uint64_t *out) {
    return labo_guest_copy(address, out, sizeof *out, false);
}

static bool valid_camera_address(uint64_t address) {
    return address >= GUEST_LO && address <= GUEST_HI - CAMERA_END && !(address & 7);
}

static bool read_camera(uint64_t address, PcVrHeadLookCamera *out) {
    return valid_camera_address(address) &&
        labo_guest_copy(address + CAMERA_LOOK_AT, out, sizeof *out, false);
}

// Follows the controller to its base camera, checking each object (range, alignment, vtable). False: not the chain
// this composition expects.
static bool read_binding(uint64_t controller, CameraBinding *out) {
    uint64_t actor_vtable = 0, base_vtable = 0;
    CameraBinding candidate = {.controller = controller};
    if (!out || controller < GUEST_LO || controller > GUEST_HI - (CONTROLLER_ACTOR_REF + 8) || (controller & 7) ||
        !read_u64(controller + CONTROLLER_ACTOR_REF, &candidate.weak) ||
        candidate.weak < GUEST_LO || candidate.weak > GUEST_HI - (ACTOR_REF_ACTOR + 8) || (candidate.weak & 7) ||
        !read_u64(candidate.weak + ACTOR_REF_ACTOR, &candidate.actor) ||
        candidate.actor < GUEST_LO || candidate.actor > GUEST_HI - (ACTOR_BASE_CAMERA + 8) ||
        (candidate.actor & 7) ||
        !read_u64(candidate.actor, &actor_vtable) || actor_vtable != ACTOR_VTABLE ||
        !read_u64(candidate.actor + ACTOR_BASE_CAMERA, &candidate.base) ||
        !valid_camera_address(candidate.base) ||
        !read_u64(candidate.base, &base_vtable) || base_vtable != BASE_VTABLE)
        return false;
    *out = candidate;
    return true;
}

static bool same_binding(const CameraBinding *first, const CameraBinding *second) {
    return first->controller == second->controller && first->weak == second->weak &&
        first->actor == second->actor && first->base == second->base;
}

static void clear_latch(void) {
    latch_valid = false;
    latch_root_owned = false;
    latch_eye_count = 0;
    latch_side_mask = 0;
}

static void clear_reference(void) {
    reference_valid = false;
    reference_generation = 0;
    clear_latch();
}

// in normalized into out. False when not finite or far from unit length (a length from 0.5 to 1.5 is accepted).
static bool normalized_quaternion(const float in[4], float out[4]) {
    double length_squared = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(in[i])) return false;
        length_squared += (double)in[i] * in[i];
    }
    if (length_squared < 0.25 || length_squared > 2.25) return false;
    const double scale = 1.0 / sqrt(length_squared);
    for (unsigned i = 0; i < 4; ++i) out[i] = (float)((double)in[i] * scale);
    return true;
}

// OpenXR LOCAL orientation: inverse(neutral) * current, in xyzw order.
static void relative_quaternion(const float neutral[4], const float current[4], float out[4]) {
    const float inverse[4] = {-neutral[0], -neutral[1], -neutral[2], neutral[3]};
    out[0] = inverse[3] * current[0] + inverse[0] * current[3] + inverse[1] * current[2] - inverse[2] * current[1];
    out[1] = inverse[3] * current[1] - inverse[0] * current[2] + inverse[1] * current[3] + inverse[2] * current[0];
    out[2] = inverse[3] * current[2] + inverse[0] * current[1] - inverse[1] * current[0] + inverse[2] * current[3];
    out[3] = inverse[3] * current[3] - inverse[0] * current[0] - inverse[1] * current[1] - inverse[2] * current[2];
}

static bool pose_fresh(uint64_t published_ns, uint64_t now) {
    return published_ns && now >= published_ns && now - published_ns <= POSE_MAX_AGE_NS;
}

// Whether eye is a fresh copy of base. Where the eye builder is called (the 0x...ddc1b0 site), the game has copied
// the base camera into the eye, then moved both position and target by the same stereo offset, keeping up. So the
// eye's target - position equals the base's, and its up the base's. This guards against composing the head's
// rotation a second time onto an eye object the game reused.
static bool freshly_copied_eye(const PcVrHeadLookCamera *base,
                               const PcVrHeadLookCamera *eye) {
    for (unsigned i = 0; i < 3; ++i) {
        if (!isfinite(base->position[i]) || !isfinite(base->target[i]) ||
            !isfinite(base->up[i]) || !isfinite(eye->position[i]) ||
            !isfinite(eye->target[i]) || !isfinite(eye->up[i])) return false;
        const double source = (double)base->target[i] - base->position[i];
        const double copied = (double)eye->target[i] - eye->position[i];
        const double world_scale = fmax(fmax(fabs(base->position[i]), fabs(base->target[i])),
                                        fmax(fabs(eye->position[i]), fabs(eye->target[i])));
        const double tolerance = 0.0001 * fmax(1.0, fabs(source)) +
                                 4.0 * FLT_EPSILON * world_scale;
        if (fabs(source - copied) > tolerance ||
            fabs((double)base->up[i] - eye->up[i]) > 0.0001) return false;
    }
    return true;
}

static void maybe_timing_summary_locked(uint64_t now) {
    if (!timing_active || timing_windows >= SUMMARY_CAP || now < timing_window_ns ||
        now - timing_window_ns < UINT64_C(1000000000)) return;
    LOG("[pc-vr-head-look-timing] window=%u generic=%llu base_caller=%llu eye_caller=%llu bound_base=%llu bound_eye=%llu base_self_ns=%llu eye_self_ns=%llu observe_calls=%llu observe_ns=%llu original_sampled=%llu original_sample_ns=%llu sample_stride=64 query_calls=%llu query_ns=%llu query_failed=%llu read_calls=%llu read_ns=%llu read_failed=%llu write_calls=%llu write_ns=%llu write_failed=%llu guard_rejected=%llu page_query_calls=%llu page_query_ns=%llu page_query_failed=%llu\n",
        timing_windows + 1,
        (unsigned long long)atomic_exchange(&timing_generic, 0),
        (unsigned long long)atomic_exchange(&timing_base_candidates, 0),
        (unsigned long long)atomic_exchange(&timing_eye_candidates, 0),
        (unsigned long long)atomic_exchange(&timing_bound_base, 0),
        (unsigned long long)atomic_exchange(&timing_bound_eye, 0),
        (unsigned long long)atomic_exchange(&timing_base_self_ns, 0),
        (unsigned long long)atomic_exchange(&timing_eye_self_ns, 0),
        (unsigned long long)atomic_exchange(&timing_observe_calls, 0),
        (unsigned long long)atomic_exchange(&timing_observe_ns, 0),
        (unsigned long long)atomic_exchange(&timing_original_samples, 0),
        (unsigned long long)atomic_exchange(&timing_original_sample_ns, 0),
        (unsigned long long)atomic_exchange(&timing_memory_calls[LABO_GUEST_QUERY], 0),
        (unsigned long long)atomic_exchange(&timing_memory_ns[LABO_GUEST_QUERY], 0),
        (unsigned long long)atomic_exchange(&timing_memory_failed[LABO_GUEST_QUERY], 0),
        (unsigned long long)atomic_exchange(&timing_memory_calls[LABO_GUEST_READ], 0),
        (unsigned long long)atomic_exchange(&timing_memory_ns[LABO_GUEST_READ], 0),
        (unsigned long long)atomic_exchange(&timing_memory_failed[LABO_GUEST_READ], 0),
        (unsigned long long)atomic_exchange(&timing_memory_calls[LABO_GUEST_WRITE], 0),
        (unsigned long long)atomic_exchange(&timing_memory_ns[LABO_GUEST_WRITE], 0),
        (unsigned long long)atomic_exchange(&timing_memory_failed[LABO_GUEST_WRITE], 0),
        (unsigned long long)atomic_exchange(&timing_guard_rejected, 0),
        (unsigned long long)atomic_exchange(&timing_memory_calls[LABO_GUEST_WORKING_SET], 0),
        (unsigned long long)atomic_exchange(&timing_memory_ns[LABO_GUEST_WORKING_SET], 0),
        (unsigned long long)atomic_exchange(&timing_memory_failed[LABO_GUEST_WORKING_SET], 0));
    timing_window_ns = now;
    ++timing_windows;
}

static void maybe_summary_locked(uint64_t now) {
    if (atomic_load_explicit(&timing_enabled, memory_order_relaxed))
        maybe_timing_summary_locked(now);
    // Menu and unrelated layer calls can precede the first verified base for
    // a long load. Preserve the finite reporting window for this camera path.
    if (!summary_active) return;
    if (!summary_start_ns) summary_start_ns = now;
    if (summaries >= SUMMARY_CAP || now < summary_start_ns ||
        now - summary_start_ns < UINT64_C(1000000000)) return;
    LOG("[pc-vr-head-look] window=%u base=%llu layer_base=%llu eye=%llu root_latches=%llu applied=%llu rejected=%llu busy=%llu unavailable=%llu last_sequence=%llu last_frame=%llu last_generation=%llu last_eye_count=%u last_side_mask=0x%x\n",
        summaries + 1, (unsigned long long)base_calls, (unsigned long long)layer_base_calls, (unsigned long long)eye_calls,
        (unsigned long long)root_latches, (unsigned long long)applied, (unsigned long long)rejected,
        (unsigned long long)busy, (unsigned long long)unavailable,
        (unsigned long long)latch_sequence, (unsigned long long)latch_frame_id,
        (unsigned long long)latch_generation, latch_eye_count, latch_side_mask);
    base_calls = layer_base_calls = eye_calls = root_latches = applied = rejected = busy = unavailable = 0;
    summary_start_ns = now;
    ++summaries;
}

bool pc_vr_head_look_enabled(void) {
    return atomic_load_explicit(&look_enabled, memory_order_acquire);
}

void pc_vr_head_look_observe_controller(uint64_t controller) {
    if (!pc_vr_head_look_enabled()) return;
    const bool timing = atomic_load_explicit(&timing_enabled, memory_order_relaxed);
    const uint64_t timing_start = timing ? plat_now_ns() : 0;
    CameraBinding observed;
    const bool previous_scope = timing_memory_scope;
    timing_memory_scope = timing;
    const bool observed_valid = read_binding(controller, &observed);
    timing_memory_scope = previous_scope;
    if (!observed_valid) {
        if (timing) {
            const uint64_t end = plat_now_ns();
            atomic_fetch_add(&timing_observe_calls, 1);
            if (end >= timing_start) atomic_fetch_add(&timing_observe_ns, end - timing_start);
        }
        return;
    }
    pthread_mutex_lock(&look_lock);
    if (!binding_valid || !same_binding(&binding, &observed)) {
        binding = observed;
        binding_valid = true;
        clear_reference();
        prior_base_valid = false;
        if (!first_binding_logged) {
            LOG("[pc-vr-head-look] first verified controller/actor/DefaultCamera binding\n");
            first_binding_logged = true;
        }
    }
    if (timing) {
        const uint64_t end = plat_now_ns();
        if (!timing_active) {
            timing_active = true;
            timing_window_ns = end;
            reset_timing_counts(); // Discard menu/unknown-controller work.
        }
        atomic_fetch_add(&timing_observe_calls, 1);
        if (end >= timing_start) atomic_fetch_add(&timing_observe_ns, end - timing_start);
        maybe_timing_summary_locked(end);
    }
    pthread_mutex_unlock(&look_lock);
}

// Under look_lock. The base builder is conditional in the game; a verified
// root eye copy also reads the current camera after the game's preparation.
static void latch_camera_locked(const PcVrHeadLookCamera *camera,
    const XrPoseSnapshot *pose, XrPoseReadResult status, uint64_t pose_now) {
    clear_latch();
    if (status == XR_POSE_BUSY) {
        ++busy; // Preserve neutral; never reuse a prior eye latch.
    } else if (status != XR_POSE_VALID || !pose->active ||
               !pose->orientation_valid || !pose->orientation_tracked || !pose->generation ||
               !pose->frame_id || !pose_fresh(pose->published_monotonic_ns, pose_now)) {
        ++unavailable;
        clear_reference();
    } else if (!pose->calibration.epoch) {
        ++unavailable; // No head look before the XR core's first calibration.
        clear_reference();
    } else {
        float current[4], heading[4];
        if (!normalized_quaternion(pose->orientation_xyzw, current) ||
            !normalized_quaternion(pose->calibration.heading_xyzw, heading)) {
            ++rejected;
            clear_reference();
        } else {
            // D053: the reference is the XR core's calibration for this LOCAL
            // space, never re-taken here after tracking loss or a rebinding.
            memcpy(reference_q, pose->calibration.heading_xyzw, sizeof reference_q);
            memcpy(reference_origin, pose->calibration.origin_xyz, sizeof reference_origin);
            reference_generation = pose->calibration.epoch;
            reference_valid = true;
            relative_quaternion(heading, current, latch_relative_q);
            latch_base = *camera;
            latch_pose = *pose;
            latch_generation = pose->generation;
            latch_published_ns = pose->published_monotonic_ns;
            latch_frame_id = pose->frame_id;
            ++latch_sequence;
            latch_valid = true;
            latch_eye_count = 0;
            latch_side_mask = 0;
            if (!first_base_logged || latch_sequence <= 8) {
                double distance2 = 0;
                if (prior_base_valid)
                    for (unsigned i = 0; i < 3; ++i) {
                        double delta = (double)camera->position[i] - prior_base.position[i];
                        distance2 += delta * delta;
                    }
                double angle = 2 * acos(fmin(1.0, fabs((double)latch_relative_q[3]))) *
                    (180.0 / 3.14159265358979323846);
                LOG("[pc-vr-head-look] base generation=%llu frame=%llu sequence=%llu base_position_delta=%.5f relative_head_deg=%.3f\n",
                    (unsigned long long)latch_generation, (unsigned long long)latch_frame_id,
                    (unsigned long long)latch_sequence,
                    prior_base_valid ? sqrt(distance2) : 0.0, angle);
                first_base_logged = true;
            }
            prior_base = *camera;
            prior_base_valid = true;
        }
    }
}

static bool base_after_original(uint64_t object, uint64_t output) {
    if (output != object + 8 || !valid_camera_address(object)) return false;
    pthread_mutex_lock(&look_lock);
    const bool is_bound_base = binding_valid && binding.base == object;
    pthread_mutex_unlock(&look_lock);
    if (!is_bound_base) return false;
    PcVrHeadLookCamera camera;
    CameraBinding observed;
    if (!read_camera(object, &camera)) return true;
    XrPoseSnapshot pose = {0};
    const XrPoseReadResult status = xr_runtime_get_pose(&pose);
    const uint64_t pose_now = xr_runtime_pose_clock_ns();
    const uint64_t log_now = plat_now_ns();
    pthread_mutex_lock(&look_lock);
    if (!binding_valid || binding.base != object ||
        !read_binding(binding.controller, &observed) || !same_binding(&binding, &observed)) {
        ++rejected;
        clear_reference();
    } else {
        summary_active = true;
        ++base_calls;
        // An intervening base rebuild cannot replace an open root's sample.
        // A changed base invalidates the pair instead of mixing its eyes.
        if (latch_root_owned && latch_eye_count == 1) {
            if (status == XR_POSE_BUSY) {
                ++busy;
                clear_latch();
            } else if (status != XR_POSE_VALID || !pose.active ||
                !pose.orientation_valid || !pose.orientation_tracked ||
                !pose.frame_id || pose.generation != latch_generation ||
                pose.calibration.epoch != reference_generation ||
                !pose_fresh(pose.published_monotonic_ns, pose_now)) {
                ++unavailable;
                clear_reference();
            } else if (memcmp(&camera, &latch_base, sizeof camera)) clear_latch();
        } else latch_camera_locked(&camera, &pose, status, pose_now);
    }
    maybe_summary_locked(log_now);
    pthread_mutex_unlock(&look_lock);
    return true;
}

// [pc-vr-audit] SAVAGEWIND_AUDIT_WATCH_EYES=1 (Windows): once both eye layers are known, CPU data breakpoints on each
// eye's view matrix (its third row: layer + 0x230 + 0x20) and logical projection (row 0, columns 1 and 2: layer +
// 0x288 + 0x10), to find the game code that reads the head-rotated eye cameras (rune targeting). =2: side 0 only (the
// game's main-camera lookup returns that layer's camera), its view matrix row, its camera's position (x, y) and
// target (y, z) fields (layer + 0x260 and + 0x270) and its projection.
static void audit_watch_eyes(uint64_t layer, unsigned side) {
#ifdef _WIN32
    static int wanted = -1;
    static uint64_t layers[2];
    static bool armed;
    if (wanted < 0) {
        const char *setting = getenv("SAVAGEWIND_AUDIT_WATCH_EYES");
        wanted = setting ? atoi(setting) : 0;
    }
    if (wanted < 1 || wanted > 2 || armed) return;
    layers[side & 1] = layer;
    if (!layers[0] || !layers[1]) return;
    extern bool plat_data_watch(const uint64_t addr[4]);
    const uint64_t addresses[4] = {layers[0] + 0x250, wanted == 2 ? layers[0] + 0x260 : layers[1] + 0x250,
        wanted == 2 ? layers[0] + 0x270 : layers[0] + 0x298, wanted == 2 ? layers[0] + 0x298 : layers[1] + 0x298};
    armed = plat_data_watch(addresses);
    LOG("[pc-vr-audit] watch eyes: layer side0 %llx side1 %llx armed=%d\n", (unsigned long long)layers[0],
        (unsigned long long)layers[1], armed);
#else
    (void)layer, (void)side;
#endif
}

// Before the original builder runs for an eye camera: compose the latched pose onto it (see the top of this file).
// False: not this composition's eye (the builder serves other layers too); true: handled, applied or rejected.
static bool eye_before_original(uint64_t eye, uint64_t output, uint64_t frame_pointer,
    PcVrNativeViewToken *native_token) {
    if (output != eye + 8 || !valid_camera_address(eye) || eye < GUEST_LO + LAYER_EYE_CAMERA) return false;
    const uint64_t layer = eye - LAYER_EYE_CAMERA;
    // The display's pair of eye layers, known from its own calls before the camera binding below is asked for (after
    // a load the binding comes seconds later): which layers are the eyes', nothing more.
    pc_vr_projection_trace_eye_layer(layer, frame_pointer);
    uint64_t pose_now = xr_runtime_pose_clock_ns();
    const uint64_t log_now = plat_now_ns();
    pthread_mutex_lock(&look_lock);
    CameraBinding observed;
    PcVrHeadLookCamera current_base, current_eye, rotated;
    uint64_t eye_vtable = 0, layer_base = 0;
    uint16_t layer_side = 0;
    // ddc1b0 also serves unrelated scene layers. Only the layer whose source
    // camera is our verified base belongs to this head-look composition.
    if (!binding_valid || !read_u64(layer + LAYER_SOURCE_CAMERA, &layer_base) ||
        layer_base != binding.base) {
        maybe_summary_locked(log_now);
        pthread_mutex_unlock(&look_lock);
        return false;
    }
    ++eye_calls;
    const bool fresh_copy = eye != binding.base &&
        read_binding(binding.controller, &observed) && same_binding(&binding, &observed) &&
        read_u64(eye, &eye_vtable) && eye_vtable == EYE_VTABLE &&
        labo_guest_copy(layer + LAYER_SIDE, &layer_side, sizeof layer_side, false) &&
        read_camera(binding.base, &current_base) && read_camera(eye, &current_eye) &&
        freshly_copied_eye(&current_base, &current_eye);
    if (pc_vr_projection_trace_root(frame_pointer)) {
        // The game may cache its base camera or prepare it at ddbb14 instead
        // of fd8788. Sample only after validating this new root's original
        // copy; never refresh from a sibling or compose onto a modified eye.
        summary_active = true;
        if (fresh_copy) {
            XrPoseSnapshot pose = {0};
            const XrPoseReadResult status = xr_runtime_get_pose(&pose);
            pose_now = xr_runtime_pose_clock_ns();
            latch_camera_locked(&current_base, &pose, status, pose_now);
            latch_root_owned = latch_valid;
            ++root_latches;
        } else clear_latch();
    }
    pc_vr_projection_trace_eye(layer, binding.base, latch_sequence,
        latch_frame_id, latch_generation, latch_valid, frame_pointer, latch_valid ? &latch_pose : NULL);
    if (!fresh_copy || !latch_valid ||
        !pose_fresh(latch_published_ns, pose_now) ||
        memcmp(&current_base, &latch_base, sizeof current_base)) {
        pc_vr_projection_trace_head_result(layer, latch_sequence, false);
        ++rejected;
        if (!pose_fresh(latch_published_ns, pose_now)) clear_reference();
        else clear_latch(); // Prevent further writes; an earlier eye cannot be rolled back.
        maybe_summary_locked(log_now);
        pthread_mutex_unlock(&look_lock);
        return true;
    }
    PcVrNativeViewReference native_reference = {.generation = reference_generation};
    memcpy(native_reference.orientation_xyzw, reference_q, sizeof reference_q);
    memcpy(native_reference.origin_xyz, reference_origin, sizeof reference_origin);
    const PcVrNativeViewResult native_result = pc_vr_projection_trace_native_view(layer, latch_sequence,
        &latch_base, &current_eye, reference_valid ? &native_reference : NULL, native_token);
    // A legitimate identity refresh can be byte-identical to its prior output;
    // the source-backed equal-forward/equal-up invariant above is the guard.
    if (native_result == PC_VR_NATIVE_VIEW_REJECTED ||
        (native_result != PC_VR_NATIVE_VIEW_APPLIED &&
        (!pc_vr_head_look_apply_eye(&latch_base, &current_eye,
                                    latch_relative_q, &rotated) ||
        !labo_guest_copy(eye + CAMERA_LOOK_AT, &rotated, sizeof rotated, true)))) {
        pc_vr_projection_trace_head_result(layer, latch_sequence, false);
        ++rejected;
        clear_latch();
        maybe_summary_locked(log_now);
        pthread_mutex_unlock(&look_lock);
        return true;
    }
    ++latch_eye_count;
    latch_side_mask |= (layer_side & LAYER_SIDE_SECOND_EYE) ? 2 : 1;
    ++applied;
    audit_watch_eyes(layer, !!(layer_side & LAYER_SIDE_SECOND_EYE));  // [pc-vr-audit]
    if (native_result != PC_VR_NATIVE_VIEW_APPLIED)
        pc_vr_projection_trace_head_result(layer, latch_sequence, true);
    if (!first_eye_logged || latch_sequence <= 8) {
        LOG("[pc-vr-head-look] eye generation=%llu frame=%llu sequence=%llu eye_in_latch=%u side_bit=%u\n",
            (unsigned long long)latch_generation, (unsigned long long)latch_frame_id,
            (unsigned long long)latch_sequence, latch_eye_count,
            !!(layer_side & LAYER_SIDE_SECOND_EYE));
        first_eye_logged = true;
    }
    maybe_summary_locked(log_now);
    pthread_mutex_unlock(&look_lock);
    return true;
}

static void eye_after_original(const PcVrNativeViewToken *token) {
    if (!token->note) return;
    pthread_mutex_lock(&look_lock);
    const bool current = binding_valid && latch_valid && reference_valid &&
        token->sequence == latch_sequence && token->reference.generation == reference_generation &&
        !memcmp(token->reference.orientation_xyzw, reference_q, sizeof reference_q) &&
        !memcmp(token->reference.origin_xyz, reference_origin, sizeof reference_origin);
    if (!pc_vr_projection_trace_native_view_built(token, current)) {
        ++rejected;
        clear_latch();
    }
    pthread_mutex_unlock(&look_lock);
}

// The wrapped look-at builder. x0: the camera object; x1: its output (the matrix, at the camera + 8); x30: the
// return address, which tells the callers apart. The original runs exactly once on every call.
static void head_look_builder(Cpu *cpu) {
    const uint64_t caller = cpu->x[30], object = cpu->x[0], output = cpu->x[1];
    // A recording worker must not retain eligibility while this invocation
    // prepares camera fields or calls the original matrix writer.
    pc_vr_render_trace_invalidate_camera(object);
    const bool enabled = pc_vr_head_look_enabled();
    const bool timing = enabled && atomic_load_explicit(&timing_enabled, memory_order_relaxed);
    const uint64_t call_index = timing ? atomic_fetch_add(&timing_generic, 1) : 0;
    if (timing && caller == EYE_CALLER) atomic_fetch_add(&timing_eye_candidates, 1);
    if (timing && caller == BASE_CALLER) atomic_fetch_add(&timing_base_candidates, 1);
    PcVrNativeViewToken native_token = {0};
    if (enabled && caller == EYE_CALLER) {
        const uint64_t start = timing ? plat_now_ns() : 0;
        const bool previous_scope = timing_memory_scope;
        timing_memory_scope = timing;
        const bool bound = eye_before_original(object, output, cpu->x[29], &native_token);
        timing_memory_scope = previous_scope;
        if (timing) {
            const uint64_t end = plat_now_ns();
            if (end >= start) atomic_fetch_add(&timing_eye_self_ns, end - start);
            if (bound) atomic_fetch_add(&timing_bound_eye, 1);
        }
    }
    const bool sample_original = timing && !(call_index & 63);
    const uint64_t original_start = sample_original ? plat_now_ns() : 0;
    const bool previous_scope = timing_memory_scope;
    timing_memory_scope = false;
    original_builder(cpu); // Exactly once, including every rejected/unsupported call.
    timing_memory_scope = previous_scope;
    if (sample_original) {
        const uint64_t end = plat_now_ns();
        atomic_fetch_add(&timing_original_samples, 1);
        if (end >= original_start)
            atomic_fetch_add(&timing_original_sample_ns, end - original_start);
    }
    if (native_token.note) {
        const uint64_t start = timing ? plat_now_ns() : 0;
        eye_after_original(&native_token);
        if (timing) {
            const uint64_t end = plat_now_ns();
            if (end >= start) atomic_fetch_add(&timing_eye_self_ns, end - start);
        }
    }
    if (enabled && caller == BASE_CALLER) {
        const uint64_t start = timing ? plat_now_ns() : 0;
        const bool previous_base_scope = timing_memory_scope;
        timing_memory_scope = timing;
        const bool bound = base_after_original(object, output);
        timing_memory_scope = previous_base_scope;
        if (timing) {
            const uint64_t end = plat_now_ns();
            if (end >= start) atomic_fetch_add(&timing_base_self_ns, end - start);
            if (bound) atomic_fetch_add(&timing_bound_base, 1);
        }
    } else if (enabled && caller == LAYER_BASE_CALLER && output == object + 8) {
        // Attribute the pinned alternate preparation without granting a pose
        // latch here. Only the validated root copy refreshes that path.
        pthread_mutex_lock(&look_lock);
        if (binding_valid && binding.base == object) ++layer_base_calls;
        pthread_mutex_unlock(&look_lock);
    }
}

void pc_vr_head_look_init(void) {
    if (!pc_vr_enabled()) return;
    if (!pc_vr_main_compatible()) {
        LOG("[pc-vr-head-look] disabled: no PC VR sites for this game version\n");
        return;
    }
    pthread_mutex_lock(&look_lock);
    if (!registered) {
        GuestFn pending = dispatch_lookup(BUILDER);
        if (!pending) {
            LOG("[pc-vr-head-look] disabled: missing generated look-at builder\n");
            pthread_mutex_unlock(&look_lock);
            return;
        }
        original_builder = pending;
        hle_override(BUILDER, head_look_builder, "PcVrHeadLookBuilder");
        registered = true;
    }
    memset(&binding, 0, sizeof binding);
    binding_valid = false;
    clear_reference();
    prior_base_valid = false;
    summary_start_ns = 0;
    summaries = 0;
    summary_active = false;
    base_calls = layer_base_calls = eye_calls = root_latches = applied = rejected = busy = unavailable = 0;
    timing_active = false;
    timing_window_ns = 0;
    timing_windows = 0;
    reset_timing_counts();
    atomic_store_explicit(&timing_enabled, pc_vr_config()->diagnostics.head_look_timing, memory_order_release);
    atomic_store_explicit(&look_enabled, true, memory_order_release);
    pthread_mutex_unlock(&look_lock);
    LOG("[pc-vr-head-look] armed pinned DefaultCamera/eye composition\n");
    pc_vr_projection_trace_init();
    if (atomic_load_explicit(&timing_enabled, memory_order_relaxed))
        LOG("[pc-vr-head-look-timing] armed bounded 1Hz attribution; original builder sampled 1/64 calls\n");
}

RT_ON_RESET(pc_vr_head_look_reset) {
    atomic_store_explicit(&look_enabled, false, memory_order_release);
    atomic_store_explicit(&timing_enabled, false, memory_order_release);
    pthread_mutex_lock(&look_lock);
    memset(&binding, 0, sizeof binding);
    binding_valid = false;
    clear_reference();
    prior_base_valid = false;
    summary_start_ns = 0;
    summaries = 0;
    summary_active = false;
    base_calls = layer_base_calls = eye_calls = root_latches = applied = rejected = busy = unavailable = 0;
    timing_active = false;
    timing_window_ns = 0;
    timing_windows = 0;
    reset_timing_counts();
    pthread_mutex_unlock(&look_lock);
}
