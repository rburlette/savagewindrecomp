// PC VR renderer: each eye's mask from the headset (D114). What it is for, the spaces and the fallback:
// vr_lens_mask.h.
#include <math.h>
#include <stdlib.h>

#include "vr_lens_mask.h"
#include "../../rt.h"
#include "../../hle/pc_vr_render_trace.h"
#include "../../xr/xr_runtime.h"

// An eye's mesh in the game's layout, built from one visible area for one FOV. A new one is built when either changes
// (a new session, a new mask, another FOV); the last few are kept, because a draw recorded with one may still be copied
// within the submission that recorded it, and the executor's copies are keyed by address.
#define VR_LENS_MESHES 4
typedef struct {
    const XrVisibleArea *area;
    float fov_tangents[4];
    VrLensVertex *vertices;
    uint16_t *indices;
    uint32_t vertex_count, index_count;
} VrLensMesh;
static VrLensMesh lens_meshes[2][VR_LENS_MESHES];
static unsigned lens_newest[2];

// The whole image, two triangles: for an eye without a mask.
static const VrLensVertex lens_rectangle_vertices[4] = {
    {{-0.5f, 0.5f}, {0, 0, 0, 1}, {0, 0}}, {{0.5f, 0.5f}, {0, 0, 0, 1}, {1, 0}},
    {{-0.5f, -0.5f}, {0, 0, 0, 1}, {0, 1}}, {{0.5f, -0.5f}, {0, 0, 0, 1}, {1, 1}}};
static const uint16_t lens_rectangle_indices[6] = {0, 1, 2, 2, 1, 3};

static uint64_t lens_draws, lens_masked, lens_whole;  // (for the log)

static const VrLensMesh *lens_mesh(unsigned eye, const XrVisibleArea *area, const float fov_tangents[4]) {
    VrLensMesh *newest = &lens_meshes[eye][lens_newest[eye]];
    bool same = newest->area == area;
    for (unsigned i = 0; i < 4 && same; ++i) same = fabsf(newest->fov_tangents[i] - fov_tangents[i]) < 1e-5f;
    if (same) return newest;
    if (area->vertex_count > 65535) return NULL;
    // (the oldest slot: its arrays are reused once the slot comes round again)
    const unsigned slot = (lens_newest[eye] + 1) % VR_LENS_MESHES;
    VrLensMesh *mesh = &lens_meshes[eye][slot];
    VrLensVertex *vertices = realloc(mesh->vertices, area->vertex_count * sizeof *vertices);
    if (vertices) mesh->vertices = vertices;
    uint16_t *indices = realloc(mesh->indices, area->index_count * sizeof *indices);
    if (indices) mesh->indices = indices;
    if (!vertices || !indices) return NULL;
    for (uint32_t i = 0; i < area->vertex_count; ++i)
        vertices[i] = vr_lens_vertex(area->tangents_xy[2 * i], area->tangents_xy[2 * i + 1], fov_tangents);
    for (uint32_t i = 0; i < area->index_count; ++i) indices[i] = (uint16_t)area->indices[i];
    mesh->area = area;
    for (unsigned i = 0; i < 4; ++i) mesh->fov_tangents[i] = fov_tangents[i];
    mesh->vertex_count = area->vertex_count;
    mesh->index_count = area->index_count;
    lens_newest[eye] = slot;
    static unsigned logged;
    if (logged++ < 8)
        LOG("[pc-vr-lens] eye=%u headset visible area %u vertices, %u triangles, fov_tan_lrud=%.4f,%.4f,%.4f,%.4f\n",
            eye, mesh->vertex_count, mesh->index_count / 3, fov_tangents[0], fov_tangents[1], fov_tangents[2],
            fov_tangents[3]);
    return mesh;
}

bool vr_lens_draw(uint32_t layer_draw, uint64_t certificate_id, VrLensDraw *out) {
    if (!(layer_draw & PC_VR_LAYER_DRAW_LENS)) return false;
    ++lens_draws;
    const VrLensMesh *mesh = NULL;
    PcVrRenderCertificate certificate;
    if (certificate_id && pc_vr_render_trace_resolve(certificate_id, &certificate) &&
        certificate.id == certificate_id && certificate.native_index < 2) {
        const unsigned eye = certificate.native_index;
        const XrVisibleArea *area = xr_runtime_visible_area(eye);
        if (area) {
            const float *angles = certificate.native_snapshot.eyes[eye].fov_left_right_up_down;
            const float fov_tangents[4] = {tanf(angles[0]), tanf(angles[1]), tanf(angles[2]), tanf(angles[3])};
            mesh = lens_mesh(eye, area, fov_tangents);
        }
    }
    if (mesh) {
        ++lens_masked;
        *out = (VrLensDraw){mesh->indices, mesh->index_count, (uint64_t)(uintptr_t)mesh->vertices,
                            mesh->vertex_count * sizeof *mesh->vertices};
    } else {
        ++lens_whole;
        *out = (VrLensDraw){lens_rectangle_indices, 6, (uint64_t)(uintptr_t)lens_rectangle_vertices,
                            sizeof lens_rectangle_vertices};
    }
    if (lens_draws % 4000 == 1)
        LOG("[pc-vr-lens] lens draws %llu: headset mask %llu, whole image %llu\n", (unsigned long long)lens_draws,
            (unsigned long long)lens_masked, (unsigned long long)lens_whole);
    return true;
}
