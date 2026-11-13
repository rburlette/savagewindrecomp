// PC VR renderer: each eye's mask from the headset (D114), runtime/nvn/vulkan/vr_lens_mask.c.
//
// What it is for: Labo's VR mode keeps the scene passes off the parts of each eye image its cardboard lenses do not
// show. While its mask flag is set, the eye scenes' screen-quad draw covers only a lens-shaped polygon (48 indices into
// 18 vertices) instead of the full-screen triangle, so those passes skip the pixels outside it. The game's draw is
// known by the code that issues it (tools/game_sites.py pcvr_lens_draw; its draws carry PC_VR_LAYER_DRAW_LENS), and
// the renderer draws the headset's own visible area there instead: the OpenXR runtime's XR_KHR_visibility_mask mesh for
// that eye (xr_runtime_visible_area), fetched each session, so another headset gets its own shape. The game's shaders
// and states are kept; only the indices and the vertices change.
//
// Units and spaces: the runtime's mesh is in tangents of the eye's view angles (x right, y up). The game's polygon is in
// a square of side 1 centred on the eye image (x right, y up), with texture coordinates over the image (u right, v
// down). A tangent becomes a place in the image through the FOV the eye was rendered with: its certificate's (the
// render trace's snapshot that built both eyes), so the mask matches that frame's projection exactly.
//
// Without a mask for an eye (a runtime without the extension, no certificate for the draw), the draw covers the whole
// image: the same as no mask.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// One vertex of the game's lens polygon as its screen-quad draw reads it (32 bytes, the game's layout, read from its
// vertex buffer): the position in the square, four constants (0, 0, 0, 1) and the texture coordinates
// (u = x + 0.5, v = 0.5 - y).
typedef struct {
    float position[2], constants[4], texture[2];
} VrLensVertex;

// The vertex at a point of the eye's view given as tangents (x right, y up), for the FOV it is drawn with: the
// tangents of its left, right, up and down edges (left and down negative).
static inline VrLensVertex vr_lens_vertex(float tangent_x, float tangent_y, const float fov_tangents[4]) {
    const float u = (tangent_x - fov_tangents[0]) / (fov_tangents[1] - fov_tangents[0]);
    const float v = (fov_tangents[2] - tangent_y) / (fov_tangents[2] - fov_tangents[3]);
    return (VrLensVertex){{u - 0.5f, 0.5f - v}, {0, 0, 0, 1}, {u, v}};
}

// What the renderer draws in place of the game's lens polygon: 16-bit indices and their vertices, in memory the module
// keeps for the process (the draw copies them when it is recorded).
typedef struct {
    const uint16_t *indices;
    uint32_t index_count;
    uint64_t vertex_address, vertex_size;
} VrLensDraw;

// For a draw with this tag (nvn_layer_draw) and certificate: false when it is not the lens draw; otherwise true, with
// the headset's visible area for the certificate's eye, or the whole image without one. Runs inside the executor's
// draw, serialised by gpu_lock as all draw recording is.
bool vr_lens_draw(uint32_t layer_draw, uint64_t certificate_id, VrLensDraw *out);
