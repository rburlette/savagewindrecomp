// Asset-free check of the eye masks from the headset (runtime/nvn/vulkan/vr_lens_mask.c, compiled here as it is): a
// tangent becomes the game's lens-polygon vertex through the eye's FOV (edges to the square's edges, y up to v down,
// texture = position + 0.5 with v flipped); only the two lens geometries are replaced (named by the tagged draws,
// then recognised by their index buffer and count whatever the call), and only at the layout's vertex stride; a
// certified eye with a mask gets its visible area for the screen quad (each triangle counter-clockwise) and its hidden
// area for the depth mask, at its own FOV, and a new FOV builds a new mesh while the old one stays; without a certificate or a mask
// the screen quad covers the whole image and the depth mask nothing. A draw whose program lays the polygon out about the
// view axis (the light-shaft slices) gets the visible area, or without a mask the whole view, in that layout: the
// tangent over the view's width and height, which an off-centre projection puts at the image's own fraction.
//
// The stubs below stand for what the module calls (the render trace's certificates, the XR owner's mask areas). No
// GPU, OpenXR runtime or game is involved.
#include "../runtime/nvn/vulkan/vr_lens_mask.c"
#include "../runtime/nvn/vulkan/vk.h"

#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; if (failures < 10) printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

// ------------------------------------------------------------------------------------------------- what the module calls
static PcVrRenderCertificate certificate;  // (resolved for its id when certificate_known)
static bool certificate_known;
static const XrMaskArea *areas[2][2];  // [hidden][eye]

bool pc_vr_render_trace_resolve(uint64_t id, PcVrRenderCertificate *out) {
    if (!certificate_known || id != certificate.id) return false;
    *out = certificate;
    return true;
}
const XrMaskArea *xr_runtime_mask_area(unsigned eye, bool hidden) { return eye < 2 ? areas[hidden][eye] : NULL; }
static PcVrConfig configuration;
const PcVrConfig *pc_vr_config(void) { return &configuration; }

static bool near(float value, float expected) { return fabsf(value - expected) < 1e-5f; }
static const uint32_t stride = sizeof(VrLensVertex);
static const uint64_t LENS = 0x72d3abdb88, DEPTH = 0x72d3abdc48;  // (the game's two index buffers)

// ------------------------------------------------------------------------------------------------------------- checks
static uint8_t staged_indices[32];
static uint64_t staged_size;

VkSpan vk_transient(uint64_t size, uint64_t align) {
    CHECK(align == 4 && size <= sizeof staged_indices);
    staged_size = size;
    memset(staged_indices, 0xcc, sizeof staged_indices);
    return (VkSpan){.ptr = staged_indices};
}

static void test_index_upload(void) {
    // The extra indices belong to another draw and must never become this draw's padding.
    const uint16_t indices[] = {0, 2, 1, 3, 2};
    const uint8_t zero_padding[4] = {0};
    const size_t triangle_bytes = 3 * sizeof *indices;
    VkSpan upload = vk_stage_indices(indices, triangle_bytes);
    CHECK(staged_size == triangle_bytes + sizeof zero_padding);
    CHECK(memcmp(upload.ptr, indices, triangle_bytes) == 0);
    CHECK(memcmp(staged_indices + triangle_bytes, zero_padding, sizeof zero_padding) == 0);
    CHECK(staged_indices[triangle_bytes + sizeof zero_padding] == 0xcc);

    upload = vk_stage_indices(NULL, 0);
    CHECK(staged_size == sizeof zero_padding && memcmp(upload.ptr, zero_padding, sizeof zero_padding) == 0);
}

static void test_vertex(void) {
    const float fov[4] = {-1.0f, 0.5f, 0.75f, -1.25f};  // left, right, up, down
    VrLensVertex corner = vr_lens_vertex(-1.0f, 0.75f, fov);  // the top left corner
    CHECK(near(corner.position[0], -0.5f) && near(corner.position[1], 0.5f));
    CHECK(near(corner.texture[0], 0) && near(corner.texture[1], 0));
    corner = vr_lens_vertex(0.5f, -1.25f, fov);  // the bottom right corner
    CHECK(near(corner.position[0], 0.5f) && near(corner.position[1], -0.5f));
    CHECK(near(corner.texture[0], 1) && near(corner.texture[1], 1));
    const VrLensVertex straight_ahead = vr_lens_vertex(0, 0, fov);  // nearer the right and top edges
    CHECK(near(straight_ahead.texture[0], 1.0f / 1.5f) && near(straight_ahead.texture[1], 0.75f / 2.0f));
    CHECK(near(straight_ahead.position[0], straight_ahead.texture[0] - 0.5f));
    CHECK(near(straight_ahead.position[1], 0.5f - straight_ahead.texture[1]));
    CHECK(straight_ahead.constants[0] == 0 && straight_ahead.constants[1] == 0 && straight_ahead.constants[2] == 0 &&
          straight_ahead.constants[3] == 1);
    CHECK(sizeof(VrLensVertex) == 32);  // (the game's vertex stride)
    // About the view axis: the axis is the origin, the texture coordinates stay the image's, and the eye's projection
    // (scale 2 / width, centre term (right + left) / width) puts the corner at the image's corner.
    const VrLensVertex axis = vr_lens_vertex_about_axis(0, 0, fov);
    CHECK(near(axis.position[0], 0) && near(axis.position[1], 0));
    CHECK(near(axis.texture[0], straight_ahead.texture[0]) && near(axis.texture[1], straight_ahead.texture[1]));
    const VrLensVertex axis_corner = vr_lens_vertex_about_axis(-1.0f, 0.75f, fov);  // the top left corner
    CHECK(near(axis_corner.position[0], -1.0f / 1.5f) && near(axis_corner.position[1], 0.75f / 2.0f));
    CHECK(near(axis_corner.texture[0], 0) && near(axis_corner.texture[1], 0));
    const float centre_x = (fov[1] + fov[0]) / (fov[1] - fov[0]), centre_y = (fov[2] + fov[3]) / (fov[2] - fov[3]);
    CHECK(near(2 * axis_corner.position[0] - centre_x, -1) && near(2 * axis_corner.position[1] - centre_y, 1));
}

static void test_draw(void) {
    VrLensDraw draw;
    const uint64_t rectangle = (uint64_t)(uintptr_t)lens_rectangle_vertices;
    // Not a lens draw, or another vertex stride: left alone.
    CHECK(!vr_lens_draw(PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE, 0x5000, 3, 7, stride, false, &draw));
    CHECK(!vr_lens_draw(PC_VR_LAYER_DRAW_LENS | PC_VR_LAYER_DRAW_EYE, LENS, 48, 7, 16, false, &draw));
    // Without a certificate: the whole image for the screen quad, nothing for the depth mask.
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS | PC_VR_LAYER_DRAW_EYE, LENS, 48, 7, stride, false, &draw));
    CHECK(draw.index_count == 6 && draw.vertex_address == rectangle);
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS_HIDDEN | PC_VR_LAYER_DRAW_EYE, DEPTH, 48, 7, stride, false, &draw));
    CHECK(draw.index_count == 3 && draw.indices[0] == 0 && draw.indices[1] == 0 && draw.indices[2] == 0);
    // A certified eye 1 with both areas: the visible one for the screen quad, the hidden one for the depth mask.
    static const float visible_tangents[] = {0, 0, -1, 1, 1, 1, 1, -1};
    static const uint32_t visible_indices[] = {0, 1, 2, 0, 2, 3};
    static const XrMaskArea visible = {4, 6, visible_tangents, visible_indices};
    static const float hidden_tangents[] = {-2, 2, -1, 2, -2, 1};
    static const uint32_t hidden_indices[] = {0, 1, 2};
    static const XrMaskArea hidden = {3, 3, hidden_tangents, hidden_indices};
    areas[0][1] = &visible;
    areas[1][1] = &hidden;
    certificate = (PcVrRenderCertificate){.id = 7, .native_index = 1};
    const float angles[4] = {-0.9f, 0.7f, 0.8f, -0.95f};
    memcpy(certificate.native_snapshot.eyes[1].fov_left_right_up_down, angles, sizeof angles);
    certificate_known = true;
    const float fov[4] = {tanf(angles[0]), tanf(angles[1]), tanf(angles[2]), tanf(angles[3])};
    // The screen quad: the eye's visible area at its FOV, each triangle turned counter-clockwise (y up) as Labo's
    // polygon is (the fixture's two are clockwise).
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS | PC_VR_LAYER_DRAW_EYE, LENS, 48, 7, stride, false, &draw));
    CHECK(draw.index_count == 6 && draw.vertex_address != rectangle && draw.vertex_size == 4 * sizeof(VrLensVertex));
    CHECK(draw.indices[0] == 0 && draw.indices[1] == 2 && draw.indices[2] == 1);
    CHECK(draw.indices[3] == 0 && draw.indices[4] == 3 && draw.indices[5] == 2);
    const VrLensVertex visible_expected = vr_lens_vertex(-1, 1, fov);
    const VrLensVertex *visible_vertices = (const VrLensVertex *)(uintptr_t)draw.vertex_address;
    CHECK(near(visible_vertices[1].position[0], visible_expected.position[0]) &&
          near(visible_vertices[1].texture[1], visible_expected.texture[1]));
    // The depth mask: the eye's hidden area at its FOV.
    VrLensDraw depth;
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS_HIDDEN | PC_VR_LAYER_DRAW_EYE, DEPTH, 48, 7, stride, false, &depth));
    CHECK(depth.index_count == 3 && depth.vertex_size == 3 * sizeof(VrLensVertex) && depth.vertex_address != rectangle);
    const VrLensVertex hidden_expected = vr_lens_vertex(-2, 2, fov);
    const VrLensVertex *hidden_vertices = (const VrLensVertex *)(uintptr_t)depth.vertex_address;
    CHECK(near(hidden_vertices[0].position[0], hidden_expected.position[0]));
    CHECK(near(hidden_vertices[0].position[1], hidden_expected.position[1]));
    // A program that lays the polygon out about the view axis: the same visible area in that layout, another mesh; the
    // depth mask is never drawn that way.
    VrLensDraw about_axis;
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_EYE, LENS, 48, 7, stride, true, &about_axis));
    CHECK(about_axis.index_count == 6 && about_axis.vertex_address != draw.vertex_address &&
          about_axis.vertex_address != rectangle && about_axis.vertex_size == draw.vertex_size);
    const VrLensVertex axis_expected = vr_lens_vertex_about_axis(-1, 1, fov);
    const VrLensVertex *axis_vertices = (const VrLensVertex *)(uintptr_t)about_axis.vertex_address;
    CHECK(near(axis_vertices[0].position[0], 0) && near(axis_vertices[0].position[1], 0));  // (the axis itself)
    CHECK(near(axis_vertices[1].position[0], axis_expected.position[0]) &&
          near(axis_vertices[1].position[1], axis_expected.position[1]));
    CHECK(near(axis_vertices[1].texture[0], visible_expected.texture[0]) &&
          near(axis_vertices[1].texture[1], visible_expected.texture[1]));
    CHECK(near(visible_vertices[1].position[0], visible_expected.position[0]));  // (the image's layout is untouched)
    VrLensDraw depth_about_axis;
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS_HIDDEN, DEPTH, 48, 7, stride, true, &depth_about_axis) &&
          depth_about_axis.vertex_address == depth.vertex_address);
    // Untagged draws of the same geometries (another call drawing the lens polygon or the depth mask): treated the
    // same; another buffer, or the same buffer with another count, is not one of them.
    VrLensDraw untagged;
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_EYE, LENS, 48, 7, stride, false, &untagged) &&
          untagged.vertex_address == draw.vertex_address);
    CHECK(vr_lens_draw(0, DEPTH, 48, 7, stride, false, &untagged) && untagged.vertex_address == depth.vertex_address);
    CHECK(!vr_lens_draw(0, LENS + 0x100, 48, 7, stride, false, &untagged));
    CHECK(!vr_lens_draw(0, LENS, 3, 7, stride, false, &untagged));
    // The same eye and FOV again: the same mesh. Another FOV: a new mesh, the old one kept.
    VrLensDraw again;
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS_HIDDEN, DEPTH, 48, 7, stride, false, &again) &&
          again.vertex_address == depth.vertex_address);
    certificate.native_snapshot.eyes[1].fov_left_right_up_down[0] = -1.0f;
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS_HIDDEN, DEPTH, 48, 7, stride, false, &again) &&
          again.vertex_address != depth.vertex_address);
    CHECK(near(hidden_vertices[0].position[0], hidden_expected.position[0]));
    // Eye 0 has no mask: the fallbacks.
    certificate.native_index = 0;
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS, LENS, 48, 7, stride, false, &draw) && draw.vertex_address == rectangle);
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS_HIDDEN, DEPTH, 48, 7, stride, false, &draw) && draw.index_count == 3 &&
          draw.indices[2] == 0);
}

int main(void) {
    test_index_upload();
    test_vertex();
    test_draw();
    printf("vr_lens_mask fixture: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
