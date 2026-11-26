// PC VR head look (D037; native per-eye views D042; one frame record, D115 step 5c): the headset's pose given to the
// game's two Labo eye cameras, so the player looks around inside the game's third-person view while the game's own
// camera, and gameplay, stay as the game makes them.
//
// The game builds every camera's view matrix with one look-at builder (BUILDER), which reads a camera's position,
// target and up. This file wraps that builder (head_look_builder) and acts on one of its callers, EYE_CALLER (its
// address: runtime/game_sites.h GAME_SITES_PC_VR), which builds each Labo eye camera: the game has just copied the base
// camera (its DefaultCamera, the third-person camera) into the eye and moved its position and target sideways by the
// eye's stereo offset. Before the original builder runs, the frame record (pc_vr_projection_trace_eye) gives that
// fresh copy the eye's native view: at the frame's first eye call from the frame's pose, which this file takes
// (frame_pose_locked), at the second from the same record. Anything unexpected leaves the eye as the game made it,
// without a certificate this frame. A pose that goes unavailable is held (D101).
//
// Which camera is the base: the camera controller the game updates each frame (pc_vr_head_look_observe_controller,
// from pc_vr_camera_update.c) leads through its camera actor to it. Every guest object is checked by address range,
// alignment and vtable before it is used. Head look is part of the PC VR play configuration (runtime/pc_vr_config.h).
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
#include "../pc_vr_pace.h"
#include "../xr/xr_runtime.h"
#include "labo_guest_memory.h"
#include "pc_vr_head_look.h"
#include "pc_vr_render_trace.h"
#include "pc_vr_head_look_math.h"
#include "pc_vr_projection_trace.h"

#define BUILDER g_site.pcvr_matrix_builder
#define EYE_CALLER g_site.pcvr_eye_caller
#define ACTOR_VTABLE g_site.pcvr_actor_vtable
#define BASE_VTABLE g_site.pcvr_base_vtable
#define EYE_VTABLE g_site.pcvr_eye_vtable

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
static bool binding_valid;
// D101: the last pose that was usable, taken again while the published one is not (tracking lost, the session not
// visible), until LOCAL is redefined (a new session or a recenter). Never cleared by a camera rebinding.
static XrPoseSnapshot held_pose;
static bool held_valid;
static bool first_binding_logged, first_eye_logged;
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

bool pc_vr_head_look_enabled(void) {
    return atomic_load_explicit(&look_enabled, memory_order_acquire);
}

void pc_vr_head_look_observe_controller(uint64_t controller) {
    if (!pc_vr_head_look_enabled()) return;
    CameraBinding observed;
    if (!read_binding(controller, &observed)) return;
    pthread_mutex_lock(&look_lock);
    if (!binding_valid || !same_binding(&binding, &observed)) {
        binding = observed;
        binding_valid = true;
        if (!first_binding_logged) {
            LOG("[pc-vr-head-look] first verified controller/actor/DefaultCamera binding\n");
            first_binding_logged = true;
        }
    }
    pthread_mutex_unlock(&look_lock);
}

// Under look_lock, at the frame's first eye call: the frame's pose, the published one when usable, else the last one
// held in the same LOCAL definition (D101). NULL: none yet in this LOCAL definition (or before the XR core's first
// calibration).
static const XrPoseSnapshot *frame_pose_locked(XrPoseSnapshot *published) {
    // D116: OpenXR's poses are used as given; their flags say whether they are there.
    const bool usable = xr_runtime_get_pose(published) == XR_POSE_VALID && published->active &&
        published->orientation_valid && published->orientation_tracked && published->views_valid &&
        published->native_views_valid && published->generation && published->frame_id &&
        published->calibration.epoch;
    if (usable) {
        held_pose = *published;
        held_valid = true;
        return published;
    }
    if (held_valid && published->local_definition != held_pose.local_definition) held_valid = false;
    return held_valid ? &held_pose : NULL;
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

// Whether the layer copies the verified base camera: an eye layer of the composition head look is bound to.
static bool bound_eye_layer(uint64_t layer) {
    uint64_t layer_base = 0;
    pthread_mutex_lock(&look_lock);
    const bool bound = binding_valid && read_u64(layer + LAYER_SOURCE_CAMERA, &layer_base) && layer_base == binding.base;
    pthread_mutex_unlock(&look_lock);
    return bound;
}

// Before the original builder runs for an eye camera: the frame record gives it its native view (see the top of this
// file). False: not this composition's eye (the builder serves other layers too), or not given its view.
static bool eye_before_original(uint64_t eye, uint64_t output, uint64_t frame_pointer) {
    if (output != eye + 8 || !valid_camera_address(eye) || eye < GUEST_LO + LAYER_EYE_CAMERA) return false;
    const uint64_t layer = eye - LAYER_EYE_CAMERA;
    // The display's pair of eye layers, known from its own calls before the camera binding below is asked for (after
    // a load the binding comes seconds later): which layers are the eyes', nothing more.
    pc_vr_projection_trace_eye_layer(layer, frame_pointer);
    // The pose wait (pc_vr_pace.h, an experiment): the first eye call of the bound composition's frame waits, outside
    // the lock, for the display frame whose pose it then takes below.
    if (pc_vr_pace_pose_wait_on() && pc_vr_projection_trace_root(frame_pointer) && bound_eye_layer(layer))
        pc_vr_pace_pose_wait();
    pthread_mutex_lock(&look_lock);
    CameraBinding observed;
    PcVrHeadLookCamera current_base, current_eye;
    uint64_t eye_vtable = 0, layer_base = 0;
    uint16_t layer_side = 0;
    // ddc1b0 also serves unrelated scene layers. Only the layer whose source
    // camera is our verified base belongs to this head-look composition.
    if (!binding_valid || !read_u64(layer + LAYER_SOURCE_CAMERA, &layer_base) ||
        layer_base != binding.base) {
        pthread_mutex_unlock(&look_lock);
        return false;
    }
    const bool readable = read_binding(binding.controller, &observed) && same_binding(&binding, &observed) &&
        read_camera(binding.base, &current_base);
    const bool fresh_copy = readable && eye != binding.base &&
        read_u64(eye, &eye_vtable) && eye_vtable == EYE_VTABLE &&
        labo_guest_copy(layer + LAYER_SIDE, &layer_side, sizeof layer_side, false) &&
        read_camera(eye, &current_eye) && freshly_copied_eye(&current_base, &current_eye);
    // The pose, only at the frame's first eye call: the second uses the record's (one pose for both eyes).
    XrPoseSnapshot published = {0};
    const XrPoseSnapshot *pose = fresh_copy && pc_vr_projection_trace_root(frame_pointer) ?
        frame_pose_locked(&published) : NULL;
    const bool given = readable &&
        pc_vr_projection_trace_eye(layer, binding.base, &current_base, fresh_copy, pose, frame_pointer);
    if (given) {
        audit_watch_eyes(layer, !!(layer_side & LAYER_SIDE_SECOND_EYE));  // [pc-vr-audit]
        if (!first_eye_logged) {
            LOG("[pc-vr-head-look] first eye given its native view (side_bit=%u)\n",
                !!(layer_side & LAYER_SIDE_SECOND_EYE));
            first_eye_logged = true;
        }
    }
    pthread_mutex_unlock(&look_lock);
    return given;
}

// The wrapped look-at builder. x0: the camera object; x1: its output (the matrix, at the camera + 8); x30: the
// return address, which tells the callers apart. The original runs exactly once on every call.
static void head_look_builder(Cpu *cpu) {
    const uint64_t caller = cpu->x[30], object = cpu->x[0], output = cpu->x[1];
    // A recording worker must not retain eligibility while this invocation
    // prepares camera fields or calls the original matrix writer.
    pc_vr_render_trace_invalidate_camera(object);
    const bool given = pc_vr_head_look_enabled() && caller == EYE_CALLER &&
        eye_before_original(object, output, cpu->x[29]);
    original_builder(cpu); // Exactly once, including every rejected/unsupported call.
    if (given) pc_vr_projection_trace_eye_built(object - LAYER_EYE_CAMERA);
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
    atomic_store_explicit(&look_enabled, true, memory_order_release);
    pthread_mutex_unlock(&look_lock);
    LOG("[pc-vr-head-look] armed pinned DefaultCamera/eye composition\n");
    pc_vr_projection_trace_init();
}

RT_ON_RESET(pc_vr_head_look_reset) {
    atomic_store_explicit(&look_enabled, false, memory_order_release);
    pthread_mutex_lock(&look_lock);
    memset(&binding, 0, sizeof binding);
    binding_valid = false;
    pthread_mutex_unlock(&look_lock);
}
