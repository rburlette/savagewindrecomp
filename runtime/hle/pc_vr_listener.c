// PC VR: the game's sound listener turned and moved with the head, as the eye cameras are.
//
// The game pans and attenuates every positional sound itself, in game code. Its audio library keeps a listener with a
// view matrix and takes each sound's position into that view (1.9.0 0x7000f13988, 0x7000f13b44; the distance from
// the listener's position, 0x7000f13a14). Once a frame the library's listener update (0x7000f1380c) asks the
// listener's poser for two views: the game's snd::ListenerPoser, whose calc (the site pcvr_listener_poser, reached
// through its vtable) builds them from the camera it follows, the base camera head look builds the eyes from. x1: the
// listener's view (the camera's, 1.8 units further forward: where distances are measured from); x2: the camera's own
// view (the one panning uses). The update then takes the listener's position, its inverse view and its velocity from
// them. Head look turns only the eye cameras, so without this the sounds stay where the game camera faces.
//
// Here the poser's calc runs unchanged, and both views then take the head's offset from the camera: its rotation and
// translation as the frame record built the eyes (pc_vr_listener_publish, at the record's root call; the math in
// pc_vr_listener_math.h). A sound ahead of the camera moves to the side as the head turns, as the picture does; the
// poser's own state (its smoothing of camera cuts) stays the game's. Only while the poser follows the eyes' base
// camera; the last offset is held while a frame has none (D101), and a reboot drops it.
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "../game_sites.h"
#include "../hle.h"
#include "labo_guest_memory.h"
#include "pc_vr_listener.h"
#include "pc_vr_listener_math.h"

#define POSER_CALC g_site.pcvr_listener_poser
#define POSER_CAMERA 0x58  // (1.9.0) the camera the poser follows (8-byte pointer); 0: its calc writes nothing

static pthread_mutex_t listener_lock = PTHREAD_MUTEX_INITIALIZER;
static bool registered;  // the poser's calc is overridden (once per process)
static GuestFn original_calc;
// The head's offset from the base camera of the last frame record that built its eyes (under listener_lock).
static PcVrListenerOffset head_offset;
static uint64_t offset_base_camera;  // 0: none since the boot
static bool first_turn_logged, other_camera_logged;

void pc_vr_listener_publish(uint64_t base_camera, const PcVrHeadLookCamera *base, const PcVrHeadLookCamera eyes[2]) {
    PcVrListenerOffset offset;
    if (!pc_vr_listener_offset_build(base, eyes, &offset)) return;  // (the last one held)
    pthread_mutex_lock(&listener_lock);
    head_offset = offset;
    offset_base_camera = base_camera;
    pthread_mutex_unlock(&listener_lock);
}

// One of the poser's output views, as the calc just wrote it, turned with the head in place.
static void turn_view(const PcVrListenerOffset *offset, uint64_t address) {
    float view[12];
    if (!address || !labo_guest_copy(address, view, sizeof view, false)) return;
    pc_vr_listener_offset_apply(offset, view, view);
    labo_guest_copy(address, view, sizeof view, true);
}

// The overridden calc. x0: the poser; x1, x2: where its two views go (either may be 0). The original runs exactly once.
static void listener_poser_calc(Cpu *cpu) {
    const uint64_t poser = cpu->x[0], listener_view = cpu->x[1], camera_view = cpu->x[2];
    uint64_t camera = 0;
    const bool follows = labo_guest_copy(poser + POSER_CAMERA, &camera, sizeof camera, false) && camera;
    original_calc(cpu);
    if (!follows) return;
    PcVrListenerOffset offset;
    pthread_mutex_lock(&listener_lock);
    const bool turned = offset_base_camera && camera == offset_base_camera;
    if (turned) offset = head_offset;
    if (turned && !first_turn_logged) {
        LOG("[pc-vr-listener] first listener turned with the head\n");
        first_turn_logged = true;
    }
    if (!turned && offset_base_camera && !other_camera_logged) {
        // (also for a moment after a load, until the eyes' next record has the new camera)
        LOG("[pc-vr-listener] the poser's camera is not the eyes' base camera: the game's listener kept meanwhile\n");
        other_camera_logged = true;
    }
    pthread_mutex_unlock(&listener_lock);
    if (!turned) return;
    turn_view(&offset, listener_view);
    turn_view(&offset, camera_view);
}

void pc_vr_listener_init(void) {
    if (!POSER_CALC) {
        LOG("[pc-vr-listener] off: no listener poser site for this game version\n");
        return;
    }
    pthread_mutex_lock(&listener_lock);
    if (!registered) {
        GuestFn pending = dispatch_lookup(POSER_CALC);
        if (!pending) {
            LOG("[pc-vr-listener] off: missing generated listener poser calc\n");
            pthread_mutex_unlock(&listener_lock);
            return;
        }
        original_calc = pending;
        hle_override(POSER_CALC, listener_poser_calc, "PcVrListenerPoser");
        registered = true;
    }
    pthread_mutex_unlock(&listener_lock);
    LOG("[pc-vr-listener] armed: the sound listener follows the head\n");
}

RT_ON_RESET(pc_vr_listener_reset) {
    pthread_mutex_lock(&listener_lock);
    offset_base_camera = 0;
    pthread_mutex_unlock(&listener_lock);
}
