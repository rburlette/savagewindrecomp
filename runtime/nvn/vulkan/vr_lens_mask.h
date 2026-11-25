// PC VR renderer: each eye's mask from the headset (D114), runtime/nvn/vulkan/vr_lens_mask.c.
//
// What it is for: Labo's VR mode keeps the scene off the parts of each eye image its cardboard lenses do not show.
// While its mask flag is set, two draws do it: a depth mask before the eye's scene covers the area outside the lens at
// the nearest depth, so the scene's geometry skips it, and the eye scenes' screen-quad draw covers only the
// lens-shaped polygon (48 indices into 18 vertices) instead of the full-screen triangle, so the full-screen passes
// skip it too. Both draws are known by the code that issues them (tools/game_sites.py pcvr_lens_depth_draw and
// pcvr_lens_draw; their draws carry PC_VR_LAYER_DRAW_LENS_HIDDEN and PC_VR_LAYER_DRAW_LENS), and so is every other
// draw of the same geometry (its index buffer and count; 21 calls draw the polygon, from occlusion tests in 50x56
// targets to the eye's placement in the final image). The renderer draws the headset's own areas there instead: the
// OpenXR runtime's XR_KHR_visibility_mask meshes for that eye (xr_runtime_mask_area), the visible area for every
// polygon draw and the hidden area for the depth mask, fetched each session, so another headset gets its own shape.
// Every pass reads the mesh the same way up (measured: the hidden area's skipped geometry against an unmasked capture
// of the same frame, Link 2026-11-13), and each triangle is wound counter-clockwise as Labo's polygon is (passes that
// cull back faces drop the other winding). The game's shaders and states are kept; only the indices and the vertices
// change, and only for a draw whose vertex stride is the layout's 32 bytes (another is left as the game drew it, and
// logged).
//
// Units and spaces: the runtime's mesh is in tangents of the eye's view angles (x right, y up). The game's polygon is in
// a square of side 1 centred on the eye image (x right, y up), with texture coordinates over the image (u right, v
// down). A tangent becomes a place in the image through the FOV the eye was rendered with: its certificate's (the
// render trace's snapshot that built both eyes), so the mask matches that frame's projection exactly.
//
// Without a mask for an eye (a runtime without the extension, no certificate for the draw), the screen quad covers the
// whole image and the depth mask nothing (one empty triangle): the same as no mask.
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

// The same point for a program that lays the polygon out about the view axis: the light-shaft slices take the
// polygon's position times the slice's half size (its depth over the projection's scale: depth x tan x aspect, a
// rectangle centred on the axis) as a place in front of the camera, and only then project it. Through an eye's
// off-centre projection the image's own fractions land displaced by the off-centre terms (12% of the width and 10% of
// the height on the Quest 3: the light shafts, and the fog that is weighted by them, stopped at a copy of the mask
// that far from each eye's outer side). Laid out as the tangent over the view's whole width and height the polygon
// lands where the mask is, whatever the projection's centre. The texture coordinates stay the image's.
static inline VrLensVertex vr_lens_vertex_about_axis(float tangent_x, float tangent_y, const float fov_tangents[4]) {
    VrLensVertex vertex = vr_lens_vertex(tangent_x, tangent_y, fov_tangents);
    vertex.position[0] = tangent_x / (fov_tangents[1] - fov_tangents[0]);
    vertex.position[1] = tangent_y / (fov_tangents[2] - fov_tangents[3]);
    return vertex;
}

// What the renderer draws in place of the game's lens polygon: 16-bit indices and their vertices, in memory the module
// keeps for the process (the draw copies them when it is recorded).
typedef struct {
    const uint16_t *indices;
    uint32_t index_count;
    uint64_t vertex_address, vertex_size;
} VrLensDraw;

// For an indexed draw with this tag (nvn_layer_draw), index buffer and count, certificate and stream 0 vertex stride:
// false when it draws neither lens geometry (or its stride is not the layout's); otherwise true, with the headset's
// area for the certificate's eye, or the fallback without one. A tagged draw names the game's lens geometry (its index
// buffer and count); later draws of the same geometry are recognised by it, whichever call issues them. about_axis: the
// draw's program lays the polygon out about the view axis (vr_lens_vertex_about_axis; vr_lens_program.h names those
// programs), so it gets the visible area in that layout, and without a mask the whole view's rectangle in it. Runs
// inside the executor's draw, serialised by gpu_lock as all draw recording is.
bool vr_lens_draw(uint32_t layer_draw, uint64_t index_address, uint64_t index_count, uint64_t certificate_id,
                  uint32_t vertex_stride, bool about_axis, VrLensDraw *out);
