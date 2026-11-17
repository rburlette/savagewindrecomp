// PC VR: the head's view over both eyes for the game's visibility decisions (SAVAGEWIND_PC_VR_HEAD_CULL=1; off by
// default; docs/evidence/pc-vr-camera-consumers-2026-11-17.md in the project's records).
//
// The game decides what is on screen from its own camera: its visibility test, and its actor level-of-detail manager,
// which stops calculating actors outside its view volumes. Head look turns only the two eye cameras, so those
// decisions follow a view the head does not turn and that is narrower than the headset's: far enemies stand frozen
// in the headset's view, effects are missing at its edges. The game's own Labo mode makes the visibility view the
// goggles' view; this module does the same for the headset. Each frame the frame record publishes the two eye cameras
// and their fields of view; from them one view that holds both eyes' frusta is kept (pc_vr_cull_view_math.h). Where
// the game takes the camera's matrix and its field of view for a visibility view, two patches ask here and get the
// head view's instead, for the eyes' base camera only.
//
// The last published view is held when a frame has none (D101): a load or a menu keeps what the head last saw.
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include "../hle.h"
#include "../pc_vr.h"
#include "../pc_vr_config.h"
#include "pc_vr_cull_view.h"
#include "pc_vr_cull_view_math.h"

// The view and its counts change only under the lock; the patches look at cull_ready first, without it.
static pthread_mutex_t cull_lock = PTHREAD_MUTEX_INITIALIZER;
static PcVrCullView cull_view;
static uint64_t cull_base_camera;
static _Atomic bool cull_ready;
// The game boot the view belongs to: a reset drops the view and every thread's guest copy of its matrix.
static _Atomic uint64_t cull_boot = 1;
static uint64_t cull_published, cull_refused, cull_matrix_given, cull_fov_given;
// This thread's guest copy of the matrix: the game reads the matrix through a guest pointer during its own call.
static _Thread_local struct { uint64_t boot, address; } cull_matrix_copy;

void pc_vr_cull_view_publish(uint64_t base_camera, const PcVrHeadLookCamera eyes[2],
                             const float fov_left_right_up_down[2][4]) {
    if (!pc_vr_config()->diagnostics.head_cull) return;
    PcVrCullView built;
    const bool ok = pc_vr_cull_view_build(eyes, fov_left_right_up_down, &built);
    pthread_mutex_lock(&cull_lock);
    if (ok) {
        cull_view = built;
        cull_base_camera = base_camera;
        ++cull_published;
        atomic_store_explicit(&cull_ready, true, memory_order_release);
    } else ++cull_refused;
    // The first view, then one line every 1800 views (about a minute at 30 FPS) with what the patches took meanwhile.
    if (ok && (cull_published == 1 || cull_published % 1800 == 0)) {
        LOG("[pc-vr-cull-view] views=%llu refused=%llu fovy_deg=%.2f aspect=%.4f matrix_given=%llu fov_given=%llu\n",
            (unsigned long long)cull_published, (unsigned long long)cull_refused, built.fovy * 57.29578f,
            built.aspect, (unsigned long long)cull_matrix_given, (unsigned long long)cull_fov_given);
        cull_matrix_given = cull_fov_given = 0;
    }
    pthread_mutex_unlock(&cull_lock);
}

uint64_t rt_pc_vr_cull_matrix(uint64_t camera) {
    if (!atomic_load_explicit(&cull_ready, memory_order_acquire)) return camera + 8;
    const uint64_t boot = atomic_load_explicit(&cull_boot, memory_order_acquire);
    if (cull_matrix_copy.boot != boot) {
        cull_matrix_copy.address = (uint64_t)(uintptr_t)hle_alloc(sizeof cull_view.matrix);
        cull_matrix_copy.boot = boot;
    }
    bool given = false;
    pthread_mutex_lock(&cull_lock);
    if (atomic_load_explicit(&cull_ready, memory_order_relaxed) && camera == cull_base_camera) {
        memcpy((void *)(uintptr_t)cull_matrix_copy.address, cull_view.matrix, sizeof cull_view.matrix);
        ++cull_matrix_given;
        given = true;
    }
    pthread_mutex_unlock(&cull_lock);
    return given ? cull_matrix_copy.address : camera + 8;
}

void rt_pc_vr_cull_fov(uint64_t camera, uint64_t fovy_address, uint64_t aspect_address) {
    if (!atomic_load_explicit(&cull_ready, memory_order_acquire)) return;
    float fovy = 0, aspect = 0;
    bool given = false;
    pthread_mutex_lock(&cull_lock);
    if (atomic_load_explicit(&cull_ready, memory_order_relaxed) && camera == cull_base_camera) {
        fovy = cull_view.fovy;
        aspect = cull_view.aspect;
        ++cull_fov_given;
        given = true;
    }
    pthread_mutex_unlock(&cull_lock);
    if (!given) return;
    memcpy((void *)(uintptr_t)fovy_address, &fovy, sizeof fovy);
    memcpy((void *)(uintptr_t)aspect_address, &aspect, sizeof aspect);
}

RT_ON_RESET(pc_vr_cull_view_reset) {
    pthread_mutex_lock(&cull_lock);
    atomic_store_explicit(&cull_ready, false, memory_order_release);
    atomic_fetch_add_explicit(&cull_boot, 1, memory_order_release);
    cull_base_camera = 0;
    cull_published = cull_refused = cull_matrix_given = cull_fov_given = 0;
    pthread_mutex_unlock(&cull_lock);
}
