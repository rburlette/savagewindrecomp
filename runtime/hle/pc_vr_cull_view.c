// PC VR: the head's view over both eyes for the game's visibility decisions, always in PC VR (a fix, not an option;
// docs/evidence/pc-vr-camera-consumers-2026-11-17.md in the project's records).
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
//
// The grass takes the same view (rt_pc_vr_cull_grass_volume, rt_pc_vr_cull_grass_matrix). The game works out one grass
// region for both eye layers, from the layer record that calculates it, the left eye's: the grass view object's update
// hands the grass that record's culling volume, view matrix and projection matrix, so grass is made inside the left
// eye's frustum only and the right eye's outer edge has none. For the record of a layer whose camera is the eyes' base
// camera, the three patches get the head view's instead: its view matrix, a perspective projection with its field of
// view (a copy of the layer's own, so the class and the near and far planes stay the game's), and a view volume the
// game's own code builds from the two, as the grass slot's builder does for a camera of its own.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include "../game_sites.h"
#include "../hle.h"
#include "../pc_vr.h"
#include "pc_vr_cull_view.h"
#include "pc_vr_cull_view_math.h"
#include "pc_vr_projection_trace.h"

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

// The game structures the grass patches read (1.9.0): the grass view object's renderer, the renderer's layer entries
// by the record's layer id (the record's first u16), the entry's render layer, the layer's camera and projection.
#define GRASS_VIEW_RENDERER 0xf0
#define RENDERER_LAYER_COUNT 0x108  // (u32)
#define RENDERER_LAYERS 0x110
#define RENDERER_LAYER_SIZE 0xe18
#define RENDERER_LAYER_OBJECT 0x3b8
#define LAYER_CAMERA 0x80
#define LAYER_PROJECTION 0x220
// A perspective projection: its two dirty flags (the logical matrix, then the device one), the logical matrix, and
// from fovy on its seven parameters (fovy, its half angle's sine, cosine and tangent, aspect, the centre's offset).
#define PROJECTION_SIZE 0xbc
#define PROJECTION_DIRTY 0x8
#define PROJECTION_MATRIX 0xc
#define PROJECTION_FOVY 0xa0
#define VOLUME_SIZE 0x2c2
// One guest buffer: the view matrix, the projection and the volume. Two of them, used in turn, so that a reader still on
// the last frame's region keeps it whole while the next is built.
#define GRASS_BUFFER_VIEW 0x0
#define GRASS_BUFFER_PROJECTION 0x40
#define GRASS_BUFFER_VOLUME 0x100
#define GRASS_BUFFER_SIZE 0x400

static pthread_mutex_t grass_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t grass_buffers, grass_buffers_boot;
static unsigned grass_next;
static uint64_t grass_built;
// What this thread last gave for a record: the two matrices follow the volume in the same update.
static _Thread_local struct { uint64_t boot, record, view, projection_matrix; } grass_given;

uint64_t rt_pc_vr_cull_grass_volume(Cpu *cpu, uint64_t eye_volume, uint64_t layer_record, uint64_t grass_view) {
    grass_given.record = 0;
    if (!atomic_load_explicit(&cull_ready, memory_order_acquire) || !g_site.pcvr_volume_copy || !eye_volume)
        return eye_volume;
    const uint64_t renderer = LD64(grass_view + GRASS_VIEW_RENDERER);
    const uint32_t layer_id = renderer ? LD16(layer_record) : 0;
    if (!renderer || layer_id >= LD32(renderer + RENDERER_LAYER_COUNT)) return eye_volume;
    const uint64_t layer =
        LD64(LD64(renderer + RENDERER_LAYERS) + (uint64_t)layer_id * RENDERER_LAYER_SIZE + RENDERER_LAYER_OBJECT);
    const uint64_t camera = layer ? LD64(layer + LAYER_CAMERA) : 0;
    const uint64_t layer_projection = layer ? LD64(layer + LAYER_PROJECTION) : 0;
    // (another layer of the base camera may carry another kind of projection: its parameters are not these)
    if (!layer_projection || LD64(layer_projection) != g_site.pcvr_proj_vtable) return eye_volume;
    PcVrCullView view;
    bool given = false;
    pthread_mutex_lock(&cull_lock);
    if (atomic_load_explicit(&cull_ready, memory_order_relaxed) && camera == cull_base_camera) {
        view = cull_view;
        given = true;
    }
    pthread_mutex_unlock(&cull_lock);
    if (!given) return eye_volume;

    const uint64_t boot = atomic_load_explicit(&cull_boot, memory_order_acquire);
    pthread_mutex_lock(&grass_lock);
    if (grass_buffers_boot != boot) {
        grass_buffers = (uint64_t)(uintptr_t)hle_alloc(2 * GRASS_BUFFER_SIZE);
        grass_buffers_boot = boot;
    }
    const uint64_t buffer = grass_buffers + grass_next * GRASS_BUFFER_SIZE;
    grass_next ^= 1;
    const uint64_t view_matrix = buffer + GRASS_BUFFER_VIEW, projection = buffer + GRASS_BUFFER_PROJECTION;
    const uint64_t volume = buffer + GRASS_BUFFER_VOLUME;
    memcpy((void *)(uintptr_t)view_matrix, view.matrix, sizeof view.matrix);
    memcpy((void *)(uintptr_t)projection, (const void *)(uintptr_t)layer_projection, PROJECTION_SIZE);
    const double half_fovy = 0.5 * view.fovy;
    const float parameters[7] = {view.fovy, (float)sin(half_fovy), (float)cos(half_fovy), (float)tan(half_fovy),
                                 view.aspect, 0, 0};
    memcpy((void *)(uintptr_t)(projection + PROJECTION_FOVY), parameters, sizeof parameters);
    // Both matrices built again from them, by the game's own builder when the volume asks for the logical one.
    const uint8_t dirty[2] = {1, 1};
    memcpy((void *)(uintptr_t)(projection + PROJECTION_DIRTY), dirty, sizeof dirty);
    // The volume: the eye's copied whole (every field the game's), then set from the head view's matrix and projection.
    const Cpu saved = *cpu;
    cpu->x[0] = volume, cpu->x[1] = eye_volume;
    guest_call(cpu, g_site.pcvr_volume_copy);
    cpu->x[0] = volume, cpu->x[1] = view_matrix;
    guest_call(cpu, g_site.pcvr_volume_view);
    cpu->x[0] = volume, cpu->x[1] = projection;
    guest_call(cpu, g_site.pcvr_volume_projection);
    *cpu = saved;
    // The first, then one line every 1800 (about a minute at 30 FPS).
    if (++grass_built == 1 || grass_built % 1800 == 0)
        LOG("[pc-vr-cull-view] grass regions=%llu fovy_deg=%.2f aspect=%.4f\n", (unsigned long long)grass_built,
            view.fovy * 57.29578f, view.aspect);
    pthread_mutex_unlock(&grass_lock);
    grass_given.boot = boot, grass_given.record = layer_record;
    grass_given.view = view_matrix, grass_given.projection_matrix = projection + PROJECTION_MATRIX;
    return volume;
}

uint64_t rt_pc_vr_cull_grass_matrix(uint64_t layer_record, unsigned projection, uint64_t record_matrix) {
    if (!grass_given.record || grass_given.record != layer_record ||
        grass_given.boot != atomic_load_explicit(&cull_boot, memory_order_acquire))
        return record_matrix;
    return projection ? grass_given.projection_matrix : grass_given.view;
}

// A model's level of detail by its distance (tools/patches.py _model_detail_depth). The game picks a model's level
// from the depth of its bounding sphere along the view's axis: the finest until the depth passes the model's own
// change-over distance, coarser beyond. The depth is the distance times the cosine of the angle from the view's
// centre, so a thing has its greatest depth where the view points straight at it: in the headset a distant hill just
// past its change-over distance is the coarse model when looked at and the detailed one a few degrees to the side
// (the user's captures of 2026-11-18). For an eye's volume the depth gives way to the distance from the point
// between the two rendered eyes. The game's volume builder copies the camera's 3x4 view to the start of the volume.
//
// The distance is taken times the play setting MODEL_DISTANCE (runtime/pc_vr_config.h). The distance itself is never
// less than the depth, so on its own it lands every such model on the coarse side: on Link a third of the models the
// game's measure showed detailed went a level coarser, and the hill was the coarse model at every turn (the user,
// 2026-11-18: "always the lower quality model"). The game's change-over distances belong to its own measure, which
// over a view is shorter than the distance; the setting's factor stands in for that. At 0 the game's depth is kept.
//
// Apply this only to the original off-centre perspective volumes and only when their view matches a rendered eye.
#define VOLUME_OFFSET_X 0x13c
#define VOLUME_PERSPECTIVE 0x2c0  // (u8)

float rt_pc_vr_model_depth(uint64_t volume, uint64_t sphere, float depth) {
    const PcVrConfig *config = pc_vr_config();
    const float factor = (float)config->play.model_distance;
    if (!config->enabled || factor == 0 || !LD8(volume + VOLUME_PERSPECTIVE)) return depth;
    float offset_x, midpoint[3], centre[3];
    memcpy(&offset_x, (const void *)(uintptr_t)(volume + VOLUME_OFFSET_X), sizeof offset_x);
    if (offset_x == 0 || !pc_vr_projection_view_midpoint((const float *)(uintptr_t)volume, midpoint)) return depth;
    memcpy(centre, (const void *)(uintptr_t)sphere, sizeof centre);
    float distance_squared = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const float delta = centre[axis] - midpoint[axis];
        distance_squared += delta * delta;
    }
    return distance_squared > 0 ? -sqrtf(distance_squared) * factor : depth;
}

RT_ON_RESET(pc_vr_cull_view_reset) {
    pthread_mutex_lock(&cull_lock);
    atomic_store_explicit(&cull_ready, false, memory_order_release);
    atomic_fetch_add_explicit(&cull_boot, 1, memory_order_release);
    cull_base_camera = 0;
    cull_published = cull_refused = cull_matrix_given = cull_fov_given = 0;
    pthread_mutex_unlock(&cull_lock);
    pthread_mutex_lock(&grass_lock);
    grass_buffers = grass_buffers_boot = 0;
    grass_next = 0;
    grass_built = 0;
    pthread_mutex_unlock(&grass_lock);
}
