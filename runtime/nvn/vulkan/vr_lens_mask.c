// PC VR renderer: each eye's mask from the headset (D114). What it is for, the spaces and the fallback:
// vr_lens_mask.h.
#include <math.h>
#include <stdlib.h>

#include "vr_lens_mask.h"
#include "../../rt.h"
#include "../../pc_vr_config.h"
#include "../../hle/pc_vr_render_trace.h"
#include "../../xr/xr_runtime.h"

// An eye's mesh in the game's layout, built from one area for one FOV. A new one is built when either changes (a new
// session, a new mask, another FOV); the last few are kept, because a draw recorded with one may still be copied within
// the submission that recorded it, and the executor's copies are keyed by address.
#define VR_LENS_MESHES 4
typedef struct {
    const XrMaskArea *area;
    float fov_tangents[4];
    VrLensVertex *vertices;
    uint16_t *indices;
    uint32_t vertex_count, index_count;
} VrLensMesh;
static VrLensMesh lens_meshes[2][2][VR_LENS_MESHES];  // [hidden][eye]
static unsigned lens_newest[2][2];

// Without a mask: the whole image (two triangles) for the screen quad, one empty triangle for the depth mask.
static const VrLensVertex lens_rectangle_vertices[4] = {
    {{-0.5f, 0.5f}, {0, 0, 0, 1}, {0, 0}}, {{0.5f, 0.5f}, {0, 0, 0, 1}, {1, 0}},
    {{-0.5f, -0.5f}, {0, 0, 0, 1}, {0, 1}}, {{0.5f, -0.5f}, {0, 0, 0, 1}, {1, 1}}};
// (counter-clockwise with y up, as Labo's polygon is: passes that cull back faces drop the other winding; clockwise,
// Link 2026-11-13, the whole eye image came out black)
static const uint16_t lens_rectangle_indices[6] = {0, 2, 1, 1, 2, 3};
static const uint16_t lens_empty_indices[3] = {0, 0, 0};

static uint64_t lens_draws[2], lens_masked[2], lens_fallback[2];  // [hidden] (for the log)

static const VrLensMesh *lens_mesh(unsigned hidden, unsigned eye, const XrMaskArea *area, const float fov_tangents[4]) {
    VrLensMesh *newest = &lens_meshes[hidden][eye][lens_newest[hidden][eye]];
    bool same = newest->area == area;
    for (unsigned i = 0; i < 4 && same; ++i) same = fabsf(newest->fov_tangents[i] - fov_tangents[i]) < 1e-5f;
    if (same) return newest;
    if (area->vertex_count > 65535) return NULL;
    // (the oldest slot: its arrays are reused once the slot comes round again)
    const unsigned slot = (lens_newest[hidden][eye] + 1) % VR_LENS_MESHES;
    VrLensMesh *mesh = &lens_meshes[hidden][eye][slot];
    VrLensVertex *vertices = realloc(mesh->vertices, area->vertex_count * sizeof *vertices);
    if (vertices) mesh->vertices = vertices;
    uint16_t *indices = realloc(mesh->indices, area->index_count * sizeof *indices);
    if (indices) mesh->indices = indices;
    if (!vertices || !indices) return NULL;
    for (uint32_t i = 0; i < area->vertex_count; ++i)
        vertices[i] = vr_lens_vertex(area->tangents_xy[2 * i], area->tangents_xy[2 * i + 1], fov_tangents);
    // Each triangle counter-clockwise with y up, as Labo's polygon is: the game's passes that cull back faces drop the
    // other winding (the runtime's mesh need not be wound that way).
    for (uint32_t i = 0; i + 2 < area->index_count; i += 3) {
        const uint32_t a = area->indices[i], b = area->indices[i + 1], c = area->indices[i + 2];
        const float *pa = &area->tangents_xy[2 * a], *pb = &area->tangents_xy[2 * b], *pc = &area->tangents_xy[2 * c];
        const bool clockwise = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0]) < 0;
        indices[i] = (uint16_t)a;
        indices[i + 1] = (uint16_t)(clockwise ? c : b);
        indices[i + 2] = (uint16_t)(clockwise ? b : c);
    }
    mesh->area = area;
    for (unsigned i = 0; i < 4; ++i) mesh->fov_tangents[i] = fov_tangents[i];
    mesh->vertex_count = area->vertex_count;
    mesh->index_count = area->index_count;
    lens_newest[hidden][eye] = slot;
    // The area's own points, once per eye and kind, so a capture can be compared with where the mesh should fall.
    static const XrMaskArea *points_logged[2][2];
    if (points_logged[hidden][eye] != area) {
        points_logged[hidden][eye] = area;
        char text[4096];
        size_t used = 0;
        for (uint32_t i = 0; i < area->vertex_count && used + 32 < sizeof text; ++i)
            used += (size_t)snprintf(text + used, sizeof text - used, " %.4f,%.4f", area->tangents_xy[2 * i],
                                     area->tangents_xy[2 * i + 1]);
        LOG("[pc-vr-lens] eye=%u %s area tangents:%s\n", eye, hidden ? "hidden" : "visible", text);
        used = 0;
        for (uint32_t i = 0; i < area->index_count && used + 12 < sizeof text; ++i)
            used += (size_t)snprintf(text + used, sizeof text - used, " %u", area->indices[i]);
        LOG("[pc-vr-lens] eye=%u %s area indices:%s\n", eye, hidden ? "hidden" : "visible", text);
        unsigned counter_clockwise = 0, clockwise = 0;  // (with y up)
        for (uint32_t i = 0; i + 2 < area->index_count; i += 3) {
            const float *a = &area->tangents_xy[2 * area->indices[i]], *b = &area->tangents_xy[2 * area->indices[i + 1]];
            const float *c = &area->tangents_xy[2 * area->indices[i + 2]];
            const float cross = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            counter_clockwise += cross > 0;
            clockwise += cross < 0;
        }
        LOG("[pc-vr-lens] eye=%u %s area winding: %u counter-clockwise, %u clockwise (y up)\n", eye,
            hidden ? "hidden" : "visible", counter_clockwise, clockwise);
    }
    static unsigned logged;
    if (logged++ < 16)
        LOG("[pc-vr-lens] eye=%u headset %s area %u vertices, %u triangles, fov_tan_lrud=%.4f,%.4f,%.4f,%.4f\n", eye,
            hidden ? "hidden" : "visible", mesh->vertex_count, mesh->index_count / 3, fov_tangents[0], fov_tangents[1],
            fov_tangents[2], fov_tangents[3]);
    return mesh;
}

// The game's two lens geometries, by their index buffer and count, as the draws its tagged calls issue show them: [0]
// the lens polygon, [1] the depth mask. Every draw of the same buffer and count is one of them, whichever call issues
// it (Link, 2026-11-13: 21 calls draw the polygon, among them about 370 draws a frame from one); a new session's
// first tagged draws replace them.
static struct { uint64_t indices, count; } lens_geometry[2];

bool vr_lens_draw(uint32_t layer_draw, uint64_t index_address, uint64_t index_count, uint64_t certificate_id,
                  uint32_t vertex_stride, VrLensDraw *out) {
    unsigned hidden;
    if (layer_draw & (PC_VR_LAYER_DRAW_LENS | PC_VR_LAYER_DRAW_LENS_HIDDEN)) {
        hidden = (layer_draw & PC_VR_LAYER_DRAW_LENS_HIDDEN) != 0;
        if (lens_geometry[hidden].indices != index_address || lens_geometry[hidden].count != index_count) {
            static unsigned logged;
            if (logged++ < 8)
                LOG("[pc-vr-lens] the game's %s: indices %#llx, %llu of them\n", hidden ? "depth mask" : "lens polygon",
                    (unsigned long long)index_address, (unsigned long long)index_count);
            lens_geometry[hidden].indices = index_address;
            lens_geometry[hidden].count = index_count;
        }
    } else if (index_address && index_address == lens_geometry[0].indices && index_count == lens_geometry[0].count) {
        hidden = 0;
    } else if (index_address && index_address == lens_geometry[1].indices && index_count == lens_geometry[1].count) {
        hidden = 1;
    } else {
        return false;
    }
    if (vertex_stride != sizeof(VrLensVertex)) {
        static unsigned logged[2];
        if (logged[hidden]++ < 4)
            LOG("[pc-vr-lens] %s draw with vertex stride %u, not %u: left as the game drew it\n",
                hidden ? "depth mask" : "screen quad", vertex_stride, (unsigned)sizeof(VrLensVertex));
        return false;
    }
    ++lens_draws[hidden];
    const VrLensMesh *mesh = NULL;
    PcVrRenderCertificate certificate;
    // (SAVAGEWIND_PC_VR_NO_HEADSET_MASK=1: neither takes the headset's shape, for an A/B run against the mask)
    if (!pc_vr_config()->diagnostics.no_headset_mask && certificate_id &&
        pc_vr_render_trace_resolve(certificate_id, &certificate) && certificate.native_index < 2) {
        const unsigned eye = certificate.native_index;
        const XrMaskArea *area = xr_runtime_mask_area(eye, hidden);
        if (area) {
            const float *angles = certificate.native_snapshot.eyes[eye].fov_left_right_up_down;
            const float fov_tangents[4] = {tanf(angles[0]), tanf(angles[1]), tanf(angles[2]), tanf(angles[3])};
            mesh = lens_mesh(hidden, eye, area, fov_tangents);
        }
    }
    if (mesh) {
        ++lens_masked[hidden];
        *out = (VrLensDraw){mesh->indices, mesh->index_count, (uint64_t)(uintptr_t)mesh->vertices,
                            mesh->vertex_count * sizeof *mesh->vertices};
    } else {
        ++lens_fallback[hidden];
        *out = (VrLensDraw){hidden ? lens_empty_indices : lens_rectangle_indices, hidden ? 3 : 6,
                            (uint64_t)(uintptr_t)lens_rectangle_vertices, sizeof lens_rectangle_vertices};
    }
    const uint64_t draws = lens_draws[hidden];
    if (draws == 1 || (draws >= 4096 && !(draws & (draws - 1))))  // (at 1, 4096, 8192, ...)
        LOG("[pc-vr-lens] %s draws %llu: headset mask %llu, without one %llu\n", hidden ? "depth mask" : "screen quad",
            (unsigned long long)lens_draws[hidden], (unsigned long long)lens_masked[hidden],
            (unsigned long long)lens_fallback[hidden]);
    return true;
}
