// Asset-free check of the eye masks from the headset (runtime/nvn/vulkan/vr_lens_mask.c, compiled here as it is): a
// tangent becomes the game's lens-polygon vertex through the eye's FOV (edges to the square's edges, y up to v down,
// texture = position + 0.5 with v flipped); only the lens draw is replaced; a certified eye with a visible area gets
// that area's indices and vertices for its own eye and FOV, and a new FOV builds a new mesh while the old one stays;
// without a certificate or a visible area the draw covers the whole image.
//
// The stubs below stand for what the module calls (the render trace's certificates, the XR owner's visible areas). No
// GPU, OpenXR runtime or game is involved.
#include "../runtime/nvn/vulkan/vr_lens_mask.c"

#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; if (failures < 10) printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

// ------------------------------------------------------------------------------------------------- what the module calls
static PcVrRenderCertificate certificate;  // (resolved for its id when certificate_known)
static bool certificate_known;
static const XrVisibleArea *areas[2];

bool pc_vr_render_trace_resolve(uint64_t id, PcVrRenderCertificate *out) {
    if (!certificate_known || id != certificate.id) return false;
    *out = certificate;
    return true;
}
const XrVisibleArea *xr_runtime_visible_area(unsigned eye) { return eye < 2 ? areas[eye] : NULL; }

static bool near(float value, float expected) { return fabsf(value - expected) < 1e-5f; }

// ------------------------------------------------------------------------------------------------------------- checks
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
}

static void test_draw(void) {
    VrLensDraw draw;
    // Not the lens draw: left alone.
    CHECK(!vr_lens_draw(PC_VR_LAYER_DRAW_FINAL | PC_VR_LAYER_DRAW_EYE, 7, &draw));
    // The lens draw without a certificate: the whole image.
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS | PC_VR_LAYER_DRAW_EYE, 7, &draw));
    CHECK(draw.index_count == 6 && draw.vertex_address == (uint64_t)(uintptr_t)lens_rectangle_vertices);
    // A certified eye 1 with a visible area (a fan of two triangles): that area, at that eye's FOV.
    static const float tangents[] = {0, 0, -1, 1, 1, 1, 1, -1};
    static const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    static const XrVisibleArea area = {4, 6, tangents, indices};
    areas[1] = &area;
    certificate = (PcVrRenderCertificate){.id = 7, .native_index = 1};
    const float angles[4] = {-0.9f, 0.7f, 0.8f, -0.95f};
    memcpy(certificate.native_snapshot.eyes[1].fov_left_right_up_down, angles, sizeof angles);
    certificate_known = true;
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS | PC_VR_LAYER_DRAW_EYE, 7, &draw));
    CHECK(draw.index_count == 6 && draw.indices[1] == 1 && draw.indices[5] == 3);
    CHECK(draw.vertex_size == 4 * sizeof(VrLensVertex) &&
          draw.vertex_address != (uint64_t)(uintptr_t)lens_rectangle_vertices);
    const VrLensVertex *vertices = (const VrLensVertex *)(uintptr_t)draw.vertex_address;
    const float fov[4] = {tanf(angles[0]), tanf(angles[1]), tanf(angles[2]), tanf(angles[3])};
    const VrLensVertex expected = vr_lens_vertex(-1, 1, fov);
    CHECK(near(vertices[1].position[0], expected.position[0]) && near(vertices[1].texture[1], expected.texture[1]));
    // The same eye and FOV again: the same mesh.
    VrLensDraw again;
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS, 7, &again) && again.vertex_address == draw.vertex_address);
    // Another FOV: a new mesh, the old one kept.
    certificate.native_snapshot.eyes[1].fov_left_right_up_down[0] = -1.0f;
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS, 7, &again) && again.vertex_address != draw.vertex_address);
    CHECK(near(vertices[1].position[0], expected.position[0]));
    // Eye 0 has no visible area: the whole image.
    certificate.native_index = 0;
    CHECK(vr_lens_draw(PC_VR_LAYER_DRAW_LENS, 7, &draw) &&
          draw.vertex_address == (uint64_t)(uintptr_t)lens_rectangle_vertices);
}

int main(void) {
    test_vertex();
    test_draw();
    printf("vr_lens_mask fixture: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
