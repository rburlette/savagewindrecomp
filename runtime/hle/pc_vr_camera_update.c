// PC VR head look's view of the game camera. The eight camera types' update functions (runtime/game_sites.h
// pcvr_cam_update_0..7) are reached through vtables, so their generated entries are overridden directly. Each
// override hands the camera controller in x0 to head look (pc_vr_head_look_observe_controller), which follows it to
// the base camera, and then runs the original update unchanged. Installed only with PC VR and head look on and the
// game version's PC VR sites present.
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../game_sites.h"
#include "../hle.h"
#include "../pc_vr.h"
#include "pc_vr_camera_update.h"
#include "pc_vr_head_look.h"

#define CAMERA_TYPES 8

static uint64_t camera_update_entry(unsigned camera_type) {
    const uint64_t entries[CAMERA_TYPES] = {g_site.pcvr_cam_update_0, g_site.pcvr_cam_update_1,
        g_site.pcvr_cam_update_2, g_site.pcvr_cam_update_3, g_site.pcvr_cam_update_4, g_site.pcvr_cam_update_5,
        g_site.pcvr_cam_update_6, g_site.pcvr_cam_update_7};
    return entries[camera_type];
}

static pthread_mutex_t camera_update_lock = PTHREAD_MUTEX_INITIALIZER;
static GuestFn original_updates[CAMERA_TYPES];
static bool camera_updates_registered;  // overrides last for the process, across reboots

static void camera_update(Cpu *cpu, unsigned camera_type) {
    pc_vr_head_look_observe_controller(cpu->x[0]);
    // dispatch_lookup captured the unmodified generated function before any override was registered. Direct
    // invocation preserves its own Cpu result.
    original_updates[camera_type](cpu);
}

#define CAMERA_UPDATE_WRAPPER(camera_type) \
    static void camera_update_##camera_type(Cpu *cpu) { camera_update(cpu, camera_type); }
CAMERA_UPDATE_WRAPPER(0)
CAMERA_UPDATE_WRAPPER(1)
CAMERA_UPDATE_WRAPPER(2)
CAMERA_UPDATE_WRAPPER(3)
CAMERA_UPDATE_WRAPPER(4)
CAMERA_UPDATE_WRAPPER(5)
CAMERA_UPDATE_WRAPPER(6)
CAMERA_UPDATE_WRAPPER(7)
static const HostFn camera_update_wrappers[CAMERA_TYPES] = {
    camera_update_0, camera_update_1, camera_update_2, camera_update_3,
    camera_update_4, camera_update_5, camera_update_6, camera_update_7
};

void pc_vr_camera_update_init(void) {
    if (!pc_vr_enabled() || !pc_vr_head_look_enabled()) return;
    if (!pc_vr_main_compatible()) return;  // (head look's own start logs this)
    pthread_mutex_lock(&camera_update_lock);
    if (!camera_updates_registered) {
        GuestFn pending[CAMERA_TYPES];
        for (unsigned camera_type = 0; camera_type < CAMERA_TYPES; ++camera_type) {
            pending[camera_type] = dispatch_lookup(camera_update_entry(camera_type));
            if (!pending[camera_type]) {
                // No partial override: without all eight, head look never sees the camera controller.
                LOG("[pc-vr-head-look] camera updates not observed: missing generated update entry type=%u\n",
                    camera_type);
                pthread_mutex_unlock(&camera_update_lock);
                return;
            }
        }
        memcpy(original_updates, pending, sizeof original_updates);
        for (unsigned camera_type = 0; camera_type < CAMERA_TYPES; ++camera_type)
            hle_override(camera_update_entry(camera_type), camera_update_wrappers[camera_type], "PcVrCameraUpdate");
        camera_updates_registered = true;
    }
    pthread_mutex_unlock(&camera_update_lock);
}
