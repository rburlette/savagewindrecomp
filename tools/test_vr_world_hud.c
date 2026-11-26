// Asset-free check of the world-tied HUD's placement (runtime/nvn/vulkan/vr_world_hud.h, D073, D088): the move of a
// marker's, a reticle's and the fade's draw into its eye's half, the patched vertex constants, the scissor and the
// fragment position's scale. By hand: a reticle and a marker straight ahead at the aim distance land at each eye's own
// centre shifted by that eye's parallax, at the HUD panel's size when Link is as far; a point behind the UI camera or
// behind the eye is refused and counted, leaving the move as it was, and so are an unmatched draw, an uncertified eye
// and a FOV with no width; the fade's move, its patched constants and its fragment scale; the scissor of a moved draw
// (a game scissor moved with it, none, one moved out of the half) and setting it only when it changes; a marker
// turned with the world's up under a rolled head; the corners of a marker and its offset child under independent yaw, pitch, roll and lean,
// checked against 3D points through each asymmetric eye after patching the actual vertex constants; a stale
// move with every field set, kept
// whole by a refused draw and ignored by the hooks, replaced whole by a placed one; the bow's reticle pane and its
// arrow count, linked or at the aim, projected together as one card. The hub (exec.c's part) is transcribed below with
// the executor's state as this file's. No renderer, GPU, game or headset is involved.
#include "../runtime/nvn/vulkan/vr_world_hud.h"
#include "test_pc_vr_settings.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rt_fatal_trace(void) { abort(); }

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; if (failures < 10) printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

// exec.c's view of the HUD panel (vr_eyes.h).
#define VR_LENS_CENTER_NDC 0.125f
#define VR_SCREEN_HALF_W (480.0 * (1.0 - VR_LENS_CENTER_NDC))
#define VR_SCREEN_WIDTH 1.666

// ---------------------------------------------------------------------------------- the executor, as the code reads it
typedef struct { uint64_t addr, size; } Range;
typedef struct { uint32_t width, height; } Target;
static struct {
    Range ubuf[1][16];
    bool ds[1];
    struct { const Target *color[1]; } rt;
    float viewport[1][4];
    int32_t scissor[1][4];
} st;
static struct { struct { unsigned certified; PcVrRenderCertificate eyes[2]; } frame; } vr_eyes;
static uint32_t vr_draw_marker;
static PcVrPaneRole vr_draw_role;
static struct { double distance; uint64_t frame; } vr_aim_result;
static bool report_found;  // (pc_vr_hud_point_by_seq finds the draw's report)
static PcVrHudPoint report;
bool pc_vr_hud_point_by_seq(uint32_t seq, PcVrHudPoint *out) {
    (void)seq;
    if (report_found) *out = report;
    return report_found;
}
static double focus_distance;
static double pc_vr_camera_focus_distance(void) { return focus_distance; }
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static uint32_t pass_w, pass_h;
typedef struct { struct { int32_t x, y; } offset; struct { uint32_t width, height; } extent; } Rect;  // (VkRect2D)
static struct { Rect sc; } dc;
static unsigned scissor_sets;
#define vk_cmd() 0
#define rec_CmdSetScissor(commands, first, count, rect) \
    ((void)(commands), (void)(first), (void)(count), (void)(rect), ++scissor_sets)

// What a placed marker's log line shows, and the fade's eye in its line.
typedef struct {
    const char *how;
    unsigned eye;
    double values[16];  // pane x, y; marker x, y; offset; camera x, y, z; depth m; anchor x, y; eye_at x, y; scale x, y; roll
} MarkerLog;

// ---------------------------------------------------------------------------------- the hub (exec.c), on the header
static VrWorldHud vr_world_hud;
static MarkerLog new_log;
static int new_fade_eye;

static bool world_hud_patch_now(uint64_t address, uint64_t size, float *bound, uint64_t *bound_size) {
    if (!vr_world_hud.active || size < 64) return false;
    static float block[0x10000 / 4];
    const uint64_t copied = size > sizeof block ? sizeof block : size;
    memcpy(block, (const void *)(uintptr_t)address, copied);
    vr_hud_move_patch(&vr_world_hud.move, block);
    memcpy(bound, block, copied);
    *bound_size = copied;
    return true;
}

static bool world_hud_draw_now(void) {
    if (!st.rt.color[0]) return false;
    if (vr_draw_marker == PC_VR_MARKER_FLAT) return false;
    if (!vr_draw_marker && vr_draw_role != PC_VR_PANE_RETICLE) return false;
    const Range pane_constants = st.ubuf[0][0], offset_constants = st.ubuf[0][10];
    if (!pane_constants.addr || pane_constants.size < 64 || !offset_constants.addr || offset_constants.size < 4 ||
        st.ds[0])
        return false;  // (no depth test: ui2d's)
    float origin[2];  // where the pane's (or the text's) origin is, in the layer's NDC before the Labo offset (the log)
    float pane_size;
    if (!vr_hud_marker_block((const float *)(uintptr_t)pane_constants.addr, pane_constants.size, origin, &pane_size))
        return false;
    const float *viewport = st.viewport[0], half_width = st.rt.color[0]->width * 0.5f;
    if (!vr_hud_one_half(viewport, half_width)) return false;  // (a draw over one half only)
    const unsigned eye = vr_hud_eye_of_half(viewport, half_width);
    if (!(vr_eyes.frame.certified >> eye & 1)) return false;
    const PcVrRenderCertificate *cert = &vr_eyes.frame.eyes[eye];
    const XrPoseSnapshot *pose = &cert->native_snapshot;
    if (!(cert->units_per_meter > 0) || !pose->calibration.epoch) return false;

    PcVrHudPoint linked;
    const PcVrHudPoint *marker = NULL;
    const char *how = "none";
    if (vr_draw_marker && pc_vr_hud_point_by_seq(vr_draw_marker, &linked)) {
        vr_hud_marker_on_layer(&linked);
        marker = &linked;
        how = "pane";
        ++vr_world_hud.exact;
    } else if (vr_draw_role == PC_VR_PANE_RETICLE && vr_aim_result.distance > 0) {
        linked = vr_hud_reticle_point(vr_aim_result.distance);
        marker = &linked;
        how = "reticle";
    }
    if (!marker) {
        ++vr_world_hud.unmatched;
        return false;
    }

    VrHudMarkerView seen;
    const VrHudSight sight = vr_hud_marker_view(marker, cert, eye, &seen);
    if (sight == VR_HUD_BEHIND) {
        ++vr_world_hud.behind;
        return false;
    }
    if (sight != VR_HUD_SEEN) return false;  // (a FOV with no width or height)
    const double labo_offset = ((const float *)(uintptr_t)offset_constants.addr)[0];  // (this half's Labo offset, NDC)
    const double tan_per_pixel = VR_SCREEN_WIDTH / (2 * VR_SCREEN_HALF_W);  // (on the HUD panel)
    const double focus = pc_vr_camera_focus_distance();
    VrHudMarkerPlacement placed;
    vr_hud_marker_move(&seen, marker, viewport, labo_offset, tan_per_pixel, focus, &vr_world_hud.move, &placed);
    ++vr_world_hud.draws;
    ++vr_world_hud.frame_draws;
    new_log = (MarkerLog){how, eye, {origin[0] * 640, origin[1] * 360, marker->layout[0], marker->layout[1],
                                     marker->offset, marker->camera[0], marker->camera[1], marker->camera[2],
                                     seen.depth / cert->units_per_meter, placed.anchor[0], placed.anchor[1],
                                     placed.eye_at[0], placed.eye_at[1], placed.scale[0], placed.scale[1],
                                     atan2(placed.image_up[0], -placed.image_up[1]) * (180.0 / 3.14159265358979323846)}};
    return true;
}

static bool hud_fade_now(void) {
    if (!st.rt.color[0] || vr_draw_role != PC_VR_PANE_FADE || st.ds[0]) return false;  // (by its role alone)
    const float *viewport = st.viewport[0], half_width = st.rt.color[0]->width * 0.5f;
    if (!vr_hud_one_half(viewport, half_width)) return false;
    const Range offset_constants = st.ubuf[0][10];
    const double labo_offset = offset_constants.addr && offset_constants.size >= 4 ?
        ((const float *)(uintptr_t)offset_constants.addr)[0] : 0;
    vr_hud_fade_move(&vr_world_hud.move, viewport, labo_offset);
    new_fade_eye = viewport[0] >= half_width * 0.5f;
    return true;
}

static void world_hud_scissor_now(void) {
    if (!vr_world_hud.active || !dc.sc.extent.width || !dc.sc.extent.height) return;
    int pixels[4];  // left, top, right, bottom, in the pass's device pixels
    vr_hud_move_scissor(&vr_world_hud.move, st.viewport[0], st.scissor[0], st.rt.color[0]->height, vp_sx, vp_sy, vp_ox,
                        vp_oy, pass_w, pass_h, pixels);
    Rect rect = {{pixels[0], pixels[1]}, {(uint32_t)(pixels[2] - pixels[0]), (uint32_t)(pixels[3] - pixels[1])}};
    if (!memcmp(&dc.sc, &rect, sizeof rect)) return;
    rec_CmdSetScissor(vk_cmd(), 0, 1, &rect);
    dc.sc = rect;
}

static void world_hud_rscale_now(float rscale[4]) {
    if (!vr_world_hud.active) return;
    vr_hud_move_rscale(&vr_world_hud.move, rscale);
}

// ---------------------------------------------------------------------------------------------------------- helpers
// A deterministic pseudo-random sequence (xorshift64), and a number in [low, high) from it.
static uint64_t random_state = 0x9e3779b97f4a7c15ull;
static double random_between(double low, double high) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return low + (high - low) * (double)(random_state >> 11) / 9007199254740992.0;
}

static void quaternion_about_axis(double axis_x, double axis_y, double axis_z, double angle_radians, double out[4]) {
    const double sine = sin(angle_radians / 2);
    out[0] = axis_x * sine;
    out[1] = axis_y * sine;
    out[2] = axis_z * sine;
    out[3] = cos(angle_radians / 2);
}

// first after second: the rotation that applies second, then first.
static void quaternion_product(const double first[4], const double second[4], double out[4]) {
    out[0] = first[3] * second[0] + first[0] * second[3] + first[1] * second[2] - first[2] * second[1];
    out[1] = first[3] * second[1] - first[0] * second[2] + first[1] * second[3] + first[2] * second[0];
    out[2] = first[3] * second[2] + first[0] * second[1] - first[1] * second[0] + first[2] * second[3];
    out[3] = first[3] * second[3] - first[0] * second[0] - first[1] * second[1] - first[2] * second[2];
}

// A move with every field set to its own value (an earlier draw's, stale).
static void random_move(VrHudMove *move) {
    *move = (VrHudMove){0};
    for (unsigned i = 0; i < 4; ++i) move->linear[i] = (float)random_between(-2, 2);
    for (unsigned i = 0; i < 2; ++i) {
        move->offset[i] = (float)random_between(-500, 500);
        move->flat_scale[i] = (float)random_between(0.2, 2);
        move->flat_offset[i] = (float)random_between(-900, 900);
    }
    for (unsigned i = 0; i < 6; ++i) move->clip[i] = (float)random_between(-3, 3);
    move->half_left = (float)random_between(0, 2000);
    move->half_right = move->half_left + (float)random_between(1, 2000);
}

// The eye certificates: both eyes from one pose, the D053 calibration, the world scale and the eyes' FOVs.
static void set_eyes(const double heading[4], const float origin[3], const double head[4], double eye_spacing,
                     const float fov[4], double units_per_meter, uint64_t epoch) {
    for (unsigned certificate = 0; certificate < 2; ++certificate) {
        PcVrRenderCertificate *cert = &vr_eyes.frame.eyes[certificate];
        cert->units_per_meter = units_per_meter;
        XrPoseSnapshot *pose = &cert->native_snapshot;
        pose->calibration.epoch = epoch;
        for (unsigned j = 0; j < 4; ++j) pose->calibration.heading_xyzw[j] = (float)heading[j];
        for (unsigned j = 0; j < 3; ++j) pose->calibration.origin_xyz[j] = origin[j];
        for (unsigned eye = 0; eye < 2; ++eye) {
            XrEyeSnapshot *eye_pose = &pose->eyes[eye];
            for (unsigned j = 0; j < 4; ++j) eye_pose->orientation_xyzw[j] = (float)head[j];
            const double side[3] = {eye ? eye_spacing / 2 : -eye_spacing / 2, 0, 0};
            double offset[3];
            vr_rotate(head, side, offset);
            for (unsigned j = 0; j < 3; ++j) eye_pose->position_xyz[j] = origin[j] + (float)offset[j];
            for (unsigned j = 0; j < 4; ++j) eye_pose->fov_left_right_up_down[j] = fov[j];
        }
    }
}

// Blocks of vertex constants as nn::ui2d binds them (floats; the layer's projection, then the pane's view rows and
// rectangle): a pane at a depth with a scale, a turn and a rectangle.
static float pane_constants_block[512], offset_constants_block[4];
static Target target;

static void pane_block(float *block, double depth, double scale, double turn, double translate_x, double translate_y,
                       double width, double height, double rect_left, double rect_top) {
    memset(block, 0, 136 * sizeof *block);
    block[0] = 2.74748f / (16.0f / 9.0f);
    block[5] = 2.74748f;
    block[10] = -1.0001f;
    block[11] = -0.20001f;
    block[14] = -1;
    float *model_view = block + 16, *rect = block + 33 * 4;
    model_view[0] = (float)(cos(turn) * scale);
    model_view[1] = (float)(-sin(turn) * scale);
    model_view[3] = (float)translate_x;
    model_view[4] = (float)(sin(turn) * scale);
    model_view[5] = (float)(cos(turn) * scale);
    model_view[7] = (float)translate_y;
    model_view[10] = 1;
    model_view[11] = (float)-depth;
    model_view[15] = 1;
    rect[0] = (float)width;
    rect[1] = (float)height;
    rect[2] = (float)rect_left;
    rect[3] = (float)rect_top;
}

// An independent 3D reference. The marker faces the midpoint of the eyes. Its right stays horizontal, and its up is
// perpendicular to that right and the line of sight. The same point on that surface is then seen from each eye.
static void card_position_in_eye(const double anchor[3], const double midpoint[3], double right_pixels, double up_pixels,
                                 double tan_per_pixel, double focus, double minimum_scale, const XrEyeSnapshot *pose,
                                 double ndc[2]) {
    const double toward[3] = {midpoint[0] - anchor[0], midpoint[1] - anchor[1], midpoint[2] - anchor[2]};
    const double horizontal = hypot(toward[0], toward[2]);
    const double distance = hypot(horizontal, toward[1]);
    const double azimuth = atan2(toward[0], toward[2]), elevation = atan2(toward[1], horizontal);
    const double right[3] = {cos(azimuth), 0, -sin(azimuth)};
    const double up[3] = {-sin(azimuth) * sin(elevation), cos(elevation), -cos(azimuth) * sin(elevation)};
    const double metres_per_pixel = tan_per_pixel * distance * fmin(1.5, fmax(minimum_scale, focus / distance));
    double from_eye[3];
    for (unsigned axis = 0; axis < 3; ++axis)
        from_eye[axis] = anchor[axis] - pose->position_xyz[axis] +
                         metres_per_pixel * (right_pixels * right[axis] + up_pixels * up[axis]);
    const double inverse[4] = {-pose->orientation_xyzw[0], -pose->orientation_xyzw[1], -pose->orientation_xyzw[2],
                               pose->orientation_xyzw[3]};
    double view[3];
    vr_rotate(inverse, from_eye, view);
    const float *fov = pose->fov_left_right_up_down;
    ndc[0] = 2 * (view[0] / -view[2] - tan(fov[0])) / (tan(fov[1]) - tan(fov[0])) - 1;
    ndc[1] = 2 * (view[1] / -view[2] - tan(fov[3])) / (tan(fov[2]) - tan(fov[3])) - 1;
}

static void check_card_projection(void) {
    const double identity[4] = {0, 0, 0, 1};
    const float origin[3] = {0};
    const float fov[2][4] = {{-0.97f, 0.68f, 0.74f, -0.83f}, {-0.70f, 0.94f, 0.81f, -0.76f}};
    const struct { double yaw, pitch, roll, lean[3]; } poses[] = {
        {0}, {0.47, 0, 0, {0}}, {0, -0.48, 0, {0}}, {0, 0, 0.62, {0}},
        {0, 0, 0, {0.21, -0.09, 0.14}}, {-0.28, 0.22, -0.35, {0.18, 0.11, -0.06}}};
    const double tan_per_pixel = VR_SCREEN_WIDTH / (2 * VR_SCREEN_HALF_W);
    const double focus = 5;
    const float anchors[][3] = {{0, 0, -2.2f}, {1.65f, 0.45f, -2.2f}, {0, 0, -33}};
    const double corners[][2] = {{0, 0}, {-62, -19}, {62, -19}, {62, 19}, {-62, 19}};
    unsigned clipped_corners = 0, visible_corners = 0;
    for (unsigned sample = 0; sample < sizeof poses / sizeof *poses; ++sample) {
        double yaw[4], pitch[4], roll[4], yaw_pitch[4], head[4];
        quaternion_about_axis(0, 1, 0, poses[sample].yaw, yaw);
        quaternion_about_axis(1, 0, 0, poses[sample].pitch, pitch);
        quaternion_about_axis(0, 0, 1, poses[sample].roll, roll);
        quaternion_product(yaw, pitch, yaw_pitch);
        quaternion_product(yaw_pitch, roll, head);
        set_eyes(identity, origin, head, 0.064, fov[0], 1, 1);
        PcVrRenderCertificate *certificate = &vr_eyes.frame.eyes[0];
        for (unsigned eye = 0; eye < 2; ++eye) {
            XrEyeSnapshot *pose = &certificate->native_snapshot.eyes[eye];
            memcpy(pose->fov_left_right_up_down, fov[eye], sizeof fov[eye]);
            for (unsigned axis = 0; axis < 3; ++axis) pose->position_xyz[axis] += (float)poses[sample].lean[axis];
        }
        for (unsigned eye = 0; eye < 2; ++eye) {
            const float viewport[4] = {eye * 960.0f, 0, 960, 1080};
            const double post_offset = eye ? -0.13 : 0.14;  // c13 adds this much to the projected NDC
            for (unsigned marker_index = 0; marker_index < sizeof anchors / sizeof *anchors; ++marker_index) {
                PcVrHudPoint marker = {.layout = {73, -41}, .frame = 1};
                const bool reticle = marker_index == 2;
                if (reticle) marker = vr_hud_reticle_point(-anchors[marker_index][2]);
                else memcpy(marker.camera, anchors[marker_index], sizeof marker.camera);
                const double minimum_scale = reticle ? 0.6 : 0.4;
                const double anchor[3] = {marker.camera[0], marker.camera[1], marker.camera[2]};
                VrHudMarkerView seen;
                const VrHudSight sight = vr_hud_marker_view(&marker, certificate, eye, &seen);
                CHECK(sight == VR_HUD_SEEN);
                if (sight != VR_HUD_SEEN) continue;
                VrHudMove move;
                VrHudMarkerPlacement placed;
                vr_hud_marker_move(&seen, &marker, viewport, post_offset, tan_per_pixel, focus, &move, &placed);
                // A pane's own scissor encloses the projected rectangle when the marker is within view.
                const int32_t scissor[4] = {(int32_t)viewport[0] + 490, 575, 68, 34};
                const double source_x = viewport[0] + 0.5 * viewport[2] * (1 + marker.layout[0] / 640.0 + post_offset);
                const double source_y = viewport[1] + 0.5 * viewport[3] * (1 - marker.layout[1] / 360.0);
                double bounds[4] = {1e6, 1e6, -1e6, -1e6};
                for (unsigned corner = 0; corner < 4; ++corner) {
                    const double right = scissor[0] + ((corner & 1) ? scissor[2] : 0) - source_x;
                    const double up = source_y - scissor[1] - ((corner & 2) ? scissor[3] : 0);
                    double expected[2];
                    card_position_in_eye(anchor, poses[sample].lean, right, up, tan_per_pixel, focus, minimum_scale,
                                         &certificate->native_snapshot.eyes[eye], expected);
                    const double pixel_x = viewport[0] + 0.5 * viewport[2] * (1 + expected[0]);
                    const double pixel_y = viewport[1] + 0.5 * viewport[3] * (1 - expected[1]);
                    bounds[0] = fmin(bounds[0], pixel_x);
                    bounds[1] = fmin(bounds[1], pixel_y);
                    bounds[2] = fmax(bounds[2], pixel_x);
                    bounds[3] = fmax(bounds[3], pixel_y);
                }
                if (bounds[0] > viewport[0] && bounds[2] < viewport[0] + viewport[2] &&
                    bounds[1] > viewport[1] && bounds[3] < viewport[1] + viewport[3]) {
                    int pixels[4];
                    vr_hud_move_scissor(&move, viewport, scissor, 1080, 1, 1, 0, 0, 1920, 1080, pixels);
                    for (unsigned edge = 0; edge < 4; ++edge)
                        CHECK(fabs(pixels[edge] - (edge < 2 ? floor(bounds[edge]) : ceil(bounds[edge]))) <= 1);
                }
                // Two pane origins on one marker. Construct the guest c3 for each so its vertex offsets measure guest
                // pixels from that origin. The depth (old clip w) also differs between them, as in separate draws.
                for (unsigned child = 0; child < 2; ++child) {
                    const double child_right = child ? 36 : 0, child_up = child ? -11 : 0;
                    const float guest_depth = child ? 4 : 7;
                    float block[16] = {0};
                    block[0] = 2 * guest_depth / viewport[2];
                    block[5] = 2 * guest_depth / viewport[3];
                    block[3] = (float)(guest_depth * (marker.layout[0] / 640.0 + 2 * child_right / viewport[2]));
                    block[7] = (float)(guest_depth * (marker.layout[1] / 360.0 + 2 * child_up / viewport[3]));
                    block[10] = -0.75f;
                    block[14] = -1;
                    vr_hud_move_patch(&move, block);
                    for (unsigned corner = 0; corner < sizeof corners / sizeof *corners; ++corner) {
                        const double vertex[4] = {corners[corner][0], corners[corner][1], -guest_depth, 1};
                        double clip[4] = {0}, expected[2];
                        for (unsigned row = 0; row < 4; ++row)
                            for (unsigned column = 0; column < 4; ++column)
                                clip[row] += block[4 * row + column] * vertex[column];
                        card_position_in_eye(anchor, poses[sample].lean, child_right + vertex[0], child_up + vertex[1],
                                             tan_per_pixel, focus, minimum_scale, &certificate->native_snapshot.eyes[eye], expected);
                        CHECK(clip[3] > 0 && clip[2] >= 0 && clip[2] <= clip[3]);
                        CHECK(fabs(clip[0] / clip[3] + post_offset - expected[0]) * viewport[2] < 0.004);
                        CHECK(fabs(clip[1] / clip[3] - expected[1]) * viewport[3] < 0.004);
                        if (fabs(expected[0]) > 1 || fabs(expected[1]) > 1) ++clipped_corners;
                        else ++visible_corners;
                    }
                }
            }
        }
    }
    CHECK(clipped_corners > 0 && visible_corners > 0);
}

// Trace the centre and an offset point of the bow's reticle pane and arrow-count text through the normal draw and
// constant-binding paths. Both keep their original anchor and project together as one card under yaw, pitch and roll.
static void check_reticle_placement(void) {
    const double identity[4] = {0, 0, 0, 1};
    const float origin[3] = {0, 1.55f, 0};
    const float fovs[2][4] = {{-0.95f, 0.71f, 0.76f, -0.87f}, {-0.67f, 0.99f, 0.83f, -0.73f}};
    double yaw[4], pitch[4], roll[4], yaw_pitch[4], head[4];
    quaternion_about_axis(0, 1, 0, 0.43, yaw);
    quaternion_about_axis(1, 0, 0, -0.32, pitch);
    quaternion_about_axis(0, 0, 1, 0.39, roll);
    quaternion_product(yaw, pitch, yaw_pitch);
    quaternion_product(yaw_pitch, roll, head);
    set_eyes(identity, origin, head, 0.064, fovs[0], 10, 1);
    vr_eyes.frame.certified = 3;
    memset(&st, 0, sizeof st);
    target = (Target){1920, 1080};
    st.rt.color[0] = &target;
    vr_draw_role = PC_VR_PANE_RETICLE;
    vr_aim_result.distance = 33;
    focus_distance = 2;
    memset(&vr_world_hud, 0, sizeof vr_world_hud);
    for (unsigned eye = 0; eye < 2; ++eye) {
        memcpy(vr_eyes.frame.eyes[eye].native_snapshot.eyes[eye].fov_left_right_up_down, fovs[eye], sizeof fovs[eye]);
        const float viewport[4] = {eye * 960.0f, 0, 960, 1080};
        memcpy(st.viewport[0], viewport, sizeof viewport);
        offset_constants_block[0] = eye ? -0.12f : 0.14f;
        st.ubuf[0][10] = (Range){(uintptr_t)offset_constants_block, 16};
        for (unsigned text = 0; text < 2; ++text) {
            pane_block(pane_constants_block, 900, 1, 0, 0, 0, 35, 32, -17, 16);
            const unsigned size = text ? 128 : 34 * 16;
            if (text) {
                pane_constants_block[3] = 0.06f;
                pane_constants_block[7] = -0.04f;
                pane_constants_block[15] = 0.85f;
            }
            const uintptr_t address = (uintptr_t)pane_constants_block;
            st.ubuf[0][0] = (Range){address, size};
            for (unsigned linked = 0; linked < 2; ++linked) {
                vr_draw_marker = linked ? 7 : 0;
                report_found = true;
                report = (PcVrHudPoint){.layout = {32, -16}, .camera = {8, 3, -35}, .frame = 4, .seq = 7};
                vr_world_hud.active = world_hud_draw_now();
                CHECK(vr_world_hud.active && vr_world_hud.move.card);
                CHECK(!strcmp(new_log.how, linked ? "pane" : "reticle"));
                float bound[136];
                uint64_t bound_size = 0;
                CHECK(world_hud_patch_now(address, size, bound, &bound_size) && bound_size == size);
                if (bound_size != size) continue;
                const double midpoint[3] = {origin[0], origin[1], origin[2]};
                const double anchor[3] = {midpoint[0] + (linked ? 0.8 : 0), midpoint[1] + (linked ? 0.3 : 0),
                                          midpoint[2] - (linked ? 3.5 : 3.3)};
                for (unsigned corner = 0; corner < 2; ++corner) {
                    const double vertex[4] = {corner * (text ? 0.012 : 11), corner * (text ? 0.007 : 7),
                                              text ? 0 : -900, 1};
                    double guest[4] = {0}, clip[4] = {0};
                    for (unsigned row = 0; row < 4; ++row)
                        for (unsigned col = 0; col < 4; ++col) {
                            guest[row] += pane_constants_block[4 * row + col] * vertex[col];
                            clip[row] += bound[4 * row + col] * vertex[col];
                        }
                    const double right = 0.5 * viewport[2] * (guest[0] / guest[3] - new_log.values[2] / 640);
                    const double up = 0.5 * viewport[3] * (guest[1] / guest[3] - new_log.values[3] / 360);
                    const double tan_per_pixel = VR_SCREEN_WIDTH / (2 * VR_SCREEN_HALF_W);
                    double expected[2];
                    card_position_in_eye(anchor, midpoint, right, up, tan_per_pixel, focus_distance, linked ? 0.4 : 0.6,
                                         &vr_eyes.frame.eyes[eye].native_snapshot.eyes[eye], expected);
                    CHECK(fabs(clip[0] / clip[3] + offset_constants_block[0] - expected[0]) * viewport[2] < 0.004);
                    CHECK(fabs(clip[1] / clip[3] - expected[1]) * viewport[3] < 0.004);
                }
            }
        }
    }
}

int main(void) {
    const float flat_fov[4] = {-0.7853982f, 0.7853982f, 0.7853982f, -0.7853982f};  // (about 45 degrees each way)
    const double no_turn[4] = {0, 0, 0, 1};
    const float calibration_origin[3] = {0, 1.6f, 0};
    check_card_projection();

    // ---- By hand: a reticle straight ahead at the aim distance (10 m, at 10 game units a metre), each eye level and
    // looking along the calibrated heading, 64 mm apart. The point lies 32 mm across from each eye: the left eye sees
    // it right of its centre by a tangent of 0.0032, the right eye left of it. With these FOVs (tangents of about 1
    // each way) that is 0.0032 of the half's NDC, 1.6 guest pixels in a half 1000 wide. Link at the same distance:
    // the HUD panel's own size.
    target = (Target){2000, 1000};
    memset(&st, 0, sizeof st);
    st.rt.color[0] = &target;
    pane_block(pane_constants_block, 1000, 1, 0, 0, 0, 40, 40, -20, 20);
    st.ubuf[0][0] = (Range){(uint64_t)(uintptr_t)pane_constants_block, 34 * 16};
    offset_constants_block[0] = 0;
    st.ubuf[0][10] = (Range){(uint64_t)(uintptr_t)offset_constants_block, 16};
    memset(&vr_eyes, 0, sizeof vr_eyes);
    set_eyes(no_turn, calibration_origin, no_turn, 0.064, flat_fov, 10, 1);
    vr_eyes.frame.certified = 3;
    vr_aim_result.distance = 100;
    vr_aim_result.frame = 500;
    focus_distance = 10;
    const double left_tangent = tan(flat_fov[0]), right_tangent = tan(flat_fov[1]);
    const double tan_per_pixel = VR_SCREEN_WIDTH / (2 * VR_SCREEN_HALF_W);
    for (unsigned linked_by_pane = 0; linked_by_pane < 2; ++linked_by_pane) {
        for (unsigned eye = 0; eye < 2; ++eye) {
            vr_draw_marker = linked_by_pane ? 7 : 0;
            vr_draw_role = linked_by_pane ? PC_VR_PANE_NONE : PC_VR_PANE_RETICLE;
            report_found = true;
            report = (PcVrHudPoint){.camera = {0, 0, -100}, .frame = 400, .seq = 7};
            const float viewport[4] = {eye ? 1000.0f : 0.0f, 0, 1000, 1000};
            memcpy(st.viewport[0], viewport, sizeof viewport);
            memset(&vr_world_hud, 0, sizeof vr_world_hud);
            vr_world_hud.active = world_hud_draw_now();
            CHECK(vr_world_hud.active && new_log.eye == eye);
            CHECK(!strcmp(new_log.how, linked_by_pane ? "pane" : "reticle"));
            const double parallax = (eye ? -0.032f : 0.032f) / 10.0;  // (the tangent across from the eye)
            const double expected_x = viewport[0] + (parallax - left_tangent) / (right_tangent - left_tangent) * 1000;
            const VrHudMove *move = &vr_world_hud.move;
            // (the anchor, the half's centre where the game drew the reticle, lands at the eye's centre, shifted)
            float block[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            vr_hud_move_patch(move, block);
            const double moved_x = viewport[0] + 500 * (1 + block[3] / block[15]);
            const double moved_y = 500 * (1 - block[7] / block[15]);
            CHECK(fabs(moved_x - expected_x) < 1e-3 && fabs(moved_y - 500) < 1e-3);
            CHECK(fabs(moved_x - (viewport[0] + 500) - (eye ? -1.6 : 1.6)) < 1e-3);
            // (on clip coordinates: the half's centre, (0, 0, w 1), moves by that parallax over the FOV's half-width)
            CHECK(fabs(block[3] / block[15] - parallax / ((right_tangent - left_tangent) / 2)) < 1e-6 &&
                  fabs(block[7] / block[15]) < 1e-6);
            // (the HUD panel's size: one guest pixel of the panel at the eye's FOV, Link 10 m away as the point)
            const double panel_scale = tan_per_pixel * 1000 / (right_tangent - left_tangent);
            CHECK(fabs(move->flat_scale[0] / panel_scale - 1) < 1e-4 && fabs(move->flat_scale[1] / panel_scale - 1) < 1e-4);
            // (level: upright, not turned)
            CHECK(block[1] == 0 && block[4] == 0 && fabs(new_log.values[15]) < 1e-9);
            CHECK(move->half_left == viewport[0] && move->half_right == viewport[0] + 1000);
        }
    }
    // A far marker keeps VR_WORLD_HUD_SIZE_MIN of the panel's size, the reticle VR_AIM_SIZE_MIN; a near one at most
    // VR_WORLD_HUD_SIZE_MAX.
    {
        const float viewport[4] = {0, 0, 1000, 1000};
        memcpy(st.viewport[0], viewport, sizeof viewport);
        const double panel_scale = tan_per_pixel * 1000 / (right_tangent - left_tangent);
        const struct { bool reticle; float depth; double size; } sizes[] = {
            {false, -1000, VR_WORLD_HUD_SIZE_MIN}, {true, -1000, VR_AIM_SIZE_MIN}, {false, -20, VR_WORLD_HUD_SIZE_MAX}};
        for (unsigned i = 0; i < 3; ++i) {
            vr_draw_marker = sizes[i].reticle ? 0 : 7;
            vr_draw_role = sizes[i].reticle ? PC_VR_PANE_RETICLE : PC_VR_PANE_NONE;
            report = (PcVrHudPoint){.camera = {0, 0, sizes[i].depth}, .frame = 400, .seq = 7};
            vr_aim_result.distance = -sizes[i].depth;
            CHECK(world_hud_draw_now() && fabs(vr_world_hud.move.flat_scale[0] / panel_scale - sizes[i].size) < 1e-6);
        }
        vr_aim_result.distance = 100;
    }

    // ---- By hand: refused draws leave the move as it was. A point behind the UI camera, and one ahead of it but
    // behind the eye (the head turned round), are counted behind; a linked draw whose report is gone, with no
    // reticle, unmatched; an uncertified eye and a FOV with no width count nothing.
    {
        const float viewport[4] = {0, 0, 1000, 1000};
        memcpy(st.viewport[0], viewport, sizeof viewport);
        vr_draw_marker = 7;
        vr_draw_role = PC_VR_PANE_NONE;
        report = (PcVrHudPoint){.camera = {0, 0, 5}, .frame = 400, .seq = 7};
        memset(&vr_world_hud, 0, sizeof vr_world_hud);
        vr_world_hud.move.offset[0] = 123;  // (a move left by an earlier draw)
        const VrWorldHud before = vr_world_hud;
        CHECK(!world_hud_draw_now() && vr_world_hud.behind == 1 && vr_world_hud.draws == 0);
        CHECK(!memcmp(&vr_world_hud.move, &before.move, sizeof before.move));
        double turned_round[4];
        quaternion_about_axis(0, 1, 0, 3.14159265358979323846, turned_round);
        set_eyes(no_turn, calibration_origin, turned_round, 0.064, flat_fov, 10, 1);
        report.camera[2] = -100;
        CHECK(!world_hud_draw_now() && vr_world_hud.behind == 2 && vr_world_hud.draws == 0);
        set_eyes(no_turn, calibration_origin, no_turn, 0.064, flat_fov, 10, 1);
        report_found = false;
        CHECK(!world_hud_draw_now() && vr_world_hud.unmatched == 1 && vr_world_hud.behind == 2);
        report_found = true;
        vr_eyes.frame.certified = 2;  // (the right eye only: this draw is the left's)
        CHECK(!world_hud_draw_now() && vr_world_hud.unmatched == 1 && vr_world_hud.behind == 2);
        vr_eyes.frame.certified = 3;
        const float no_width[4] = {0.5f, 0.5f, 0.7853982f, -0.7853982f};
        set_eyes(no_turn, calibration_origin, no_turn, 0.064, no_width, 10, 1);
        CHECK(!world_hud_draw_now() && vr_world_hud.unmatched == 1 && vr_world_hud.behind == 2);
        // (linked by pane: behind the camera, behind the eye, no width)
        CHECK(!memcmp(&vr_world_hud.move, &before.move, sizeof before.move) && vr_world_hud.exact == 3);
        set_eyes(no_turn, calibration_origin, no_turn, 0.064, flat_fov, 10, 1);
        // (a pane larger than the old cap is placed all the same: the binding says it is the marker's; the flat piece
        // is not one: nothing counted)
        pane_block(pane_constants_block, 1000, 1, 0, 0, 0, 450, 40, -20, 20);
        CHECK(world_hud_draw_now() && vr_world_hud.exact == 4 && vr_world_hud.draws == 1);
        pane_block(pane_constants_block, 1000, 1, 0, 0, 0, 40, 40, -20, 20);
        vr_draw_marker = PC_VR_MARKER_FLAT;
        CHECK(!world_hud_draw_now() && vr_world_hud.exact == 4);
        vr_draw_marker = 7;
        CHECK(world_hud_draw_now() && vr_world_hud.exact == 5 && vr_world_hud.draws == 2 && vr_world_hud.frame_draws == 2);
    }

    // ---- By hand: the fade, in the left half with a Labo offset of 0.13: 1.3 times about the half's centre, which
    // on clip coordinates (the offset added after them) is x' = 1.3 x + 0.3 x 0.13 w, y' = 1.3 y. The pane is known
    // by its role: one that does not cover the layer is the fade all the same, one of another role is not.
    {
        const float viewport[4] = {0, 0, 1000, 1000};
        memcpy(st.viewport[0], viewport, sizeof viewport);
        vr_draw_marker = 0;
        vr_draw_role = PC_VR_PANE_FADE;
        offset_constants_block[0] = 0.13f;
        pane_block(pane_constants_block, 1000, 1, 0, 0, 0, 1400, 800, -700, 400);
        CHECK(hud_fade_now() && new_fade_eye == 0);
        const VrHudMove *move = &vr_world_hud.move;
        CHECK(move->clip[0] == 1.3f && move->clip[1] == 0 && move->clip[3] == 0 && move->clip[4] == 1.3f);
        CHECK(fabs(move->clip[2] - 0.3 * 0.13f) < 1e-6 && move->clip[5] == 0);
        CHECK(move->flat_scale[0] == 1.3f && fabs(move->flat_offset[0] - -150) < 1e-3 && fabs(move->flat_offset[1] - -150) < 1e-3);
        // (its constants: rows x and y of the identity become the move; its fragment scale takes 1.3 and -150)
        float block[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        vr_hud_move_patch(move, block);
        CHECK(block[0] == 1.3f && block[1] == 0 && block[3] == move->clip[2] && block[5] == 1.3f && block[7] == 0);
        CHECK(block[10] == 1 && block[15] == 1);
        float rscale[4] = {2, 2, 10, 0};
        vr_hud_move_rscale(move, rscale);
        CHECK(rscale[0] == 2.6f && fabs(rscale[2] - (10 + 2 * move->flat_offset[0])) < 1e-3 && rscale[3] == 2 * move->flat_offset[1]);
        pane_block(pane_constants_block, 1000, 1, 0, 0, 0, 1000, 800, -500, 400);  // (0.77 of the layer's width)
        CHECK(hud_fade_now());
        vr_draw_role = PC_VR_PANE_BOX;
        CHECK(!hud_fade_now());
        offset_constants_block[0] = 0;
    }

    // ---- By hand: the scissor of a moved draw. The move: 100 right and 50 down, not scaled, in a left half 960
    // wide; device pixels twice the guest's, 10 right; a pass of 1920 x 2160.
    {
        const float viewport[4] = {0, 0, 960, 1080};
        VrHudMove move;
        vr_hud_move_place(&move, viewport, 0, (const double[2]){480, 540}, (const double[2]){580, 590},
                          (const double[2]){1, 1}, (const double[2]){0, -1});
        CHECK(move.linear[0] == 1 && move.linear[1] == 0 && move.linear[2] == 0 && move.linear[3] == 1);
        CHECK(move.offset[0] == 100 && move.offset[1] == 50);
        int pixels[4];
        // (the game's scissor, 100..400 by 200..600, moved: 200..500 by 250..650)
        vr_hud_move_scissor(&move, viewport, (const int32_t[4]){100, 200, 300, 400}, 1080, 2, 2, 10, 0, 1920, 2160,
                            pixels);
        CHECK(pixels[0] == 410 && pixels[1] == 500 && pixels[2] == 1010 && pixels[3] == 1300);
        // (none: the half, its right edge cut at the pass)
        vr_hud_move_scissor(&move, viewport, (const int32_t[4]){0, 0, 16384, 16384}, 1080, 2, 2, 10, 0, 1920, 2160,
                            pixels);
        CHECK(pixels[0] == 10 && pixels[1] == 0 && pixels[2] == 1920 && pixels[3] == 2160);
        // (one moved past the half's right edge: kept inside the half)
        vr_hud_move_scissor(&move, viewport, (const int32_t[4]){800, 0, 150, 100}, 1080, 2, 2, 10, 0, 1920, 2160,
                            pixels);
        CHECK(pixels[0] == 1810 && pixels[1] == 100 && pixels[2] == 1920 && pixels[3] == 300);
        // (moved out of the half entirely: empty)
        vr_hud_move_scissor(&move, viewport, (const int32_t[4]){900, 0, 50, 100}, 1080, 1, 1, 0, 0, 1920, 2160, pixels);
        CHECK(pixels[0] == 1000 && pixels[2] == 1000);
        // A card's scissor crosses the eye plane. w = x + 0.1, x' = 0.3x + 0.1. Its visible part runs from
        // x'/w = 0.4 at x = 0.6 to the right edge, with both y bounds running outside the half as w tends to zero.
        const float small_view[4] = {0, 0, 100, 100};
        const VrHudMove crossing = {.clip = {0.3f, 0, 0.1f, 0, 1, 0}, .clip_w = {1, 0, 0.1f},
                                    .card = true, .half_right = 100};
        vr_hud_move_scissor(&crossing, small_view, (const int32_t[4]){20, 40, 60, 20}, 100, 1, 1, 0, 0, 100, 100, pixels);
        CHECK(abs(pixels[0] - 70) <= 1 && pixels[1] == 0 && pixels[2] == 100 && pixels[3] == 100);
        vr_hud_move_scissor(&crossing, small_view, (const int32_t[4]){10, 40, 10, 20}, 100, 1, 1, 0, 0, 100, 100, pixels);
        CHECK(pixels[0] == pixels[2] && pixels[1] == pixels[3]);
        // (the hub sets it once, and not again while it holds)
        memcpy(st.viewport[0], viewport, sizeof viewport);
        const int32_t game_scissor[4] = {100, 200, 300, 400};
        memcpy(st.scissor[0], game_scissor, sizeof game_scissor);
        target = (Target){1920, 1080};
        st.rt.color[0] = &target;
        vp_sx = vp_sy = 2;
        vp_ox = 10;
        vp_oy = 0;
        pass_w = 1920;
        pass_h = 2160;
        vr_world_hud.active = true;
        vr_world_hud.move = move;
        dc.sc = (Rect){{0, 0}, {1920, 2160}};
        scissor_sets = 0;
        world_hud_scissor_now();
        CHECK(scissor_sets == 1 && dc.sc.offset.x == 410 && dc.sc.extent.width == 600 && dc.sc.extent.height == 800);
        world_hud_scissor_now();
        CHECK(scissor_sets == 1);
        vr_world_hud.active = false;
        world_hud_scissor_now();
        float rscale[4] = {2, 2, 10, 0};
        world_hud_rscale_now(rscale);
        CHECK(scissor_sets == 1 && rscale[0] == 2 && rscale[2] == 10);
        vp_sx = vp_sy = 1;
        vp_ox = vp_oy = 0;
    }

    // ---- By hand: the reticle straight ahead in the left half, as above, with the head rolled 0.3 rad: it turns with
    // the world's up as the eye sees it (its roll in the image 0.3 rad, 17.19 degrees).
    {
        target = (Target){2000, 1000};
        memset(&st, 0, sizeof st);
        st.rt.color[0] = &target;
        pane_block(pane_constants_block, 1000, 1, 0, 0, 0, 40, 40, -20, 20);
        st.ubuf[0][0] = (Range){(uint64_t)(uintptr_t)pane_constants_block, 34 * 16};
        offset_constants_block[0] = 0;
        st.ubuf[0][10] = (Range){(uint64_t)(uintptr_t)offset_constants_block, 16};
        const float viewport[4] = {0, 0, 1000, 1000};
        memcpy(st.viewport[0], viewport, sizeof viewport);
        memset(&vr_eyes, 0, sizeof vr_eyes);
        set_eyes(no_turn, calibration_origin, no_turn, 0.064, flat_fov, 10, 1);
        vr_eyes.frame.certified = 3;
        vr_aim_result.distance = 100;
        vr_aim_result.frame = 500;
        vr_draw_marker = 0;
        vr_draw_role = PC_VR_PANE_RETICLE;
        double rolled[4];
        quaternion_about_axis(0, 0, 1, 0.3, rolled);
        set_eyes(no_turn, calibration_origin, rolled, 0.064, flat_fov, 10, 1);
        memset(&vr_world_hud, 0, sizeof vr_world_hud);
        CHECK(world_hud_draw_now());
        CHECK(vr_world_hud.draws == 1 && vr_world_hud.unmatched == 0 && vr_world_hud.behind == 0);
        CHECK(fabs(new_log.values[15] - 0.3 * 180 / 3.14159265358979323846) < 0.01);
        set_eyes(no_turn, calibration_origin, no_turn, 0.064, flat_fov, 10, 1);

        // ---- By hand: a stale move with every field set (an earlier draw's). A refused draw (behind the UI camera)
        // leaves all of it, bit for bit, and the hooks ignore it while the draw is not moved; a placed draw replaces
        // every field.
        VrHudMove stale;
        random_move(&stale);
        stale.card = true;
        stale.post_offset = -0.127;
        stale.clip_w[0] = 0.23f;
        stale.clip_w[1] = -0.12f;
        stale.clip_w[2] = 1.45f;
        vr_world_hud.move = stale;
        vr_world_hud.active = false;
        vr_draw_marker = 7;
        vr_draw_role = PC_VR_PANE_NONE;
        report_found = true;
        report = (PcVrHudPoint){.camera = {0, 0, 5}, .frame = 400, .seq = 7};
        CHECK(!world_hud_draw_now() && !memcmp(&vr_world_hud.move, &stale, sizeof stale));
        float rscale[4] = {2, 2, 10, 0};
        world_hud_rscale_now(rscale);
        dc.sc = (Rect){{0, 0}, {1, 1}};
        scissor_sets = 0;
        world_hud_scissor_now();
        float bound[16];
        uint64_t bound_size = 0;
        CHECK(rscale[0] == 2 && rscale[2] == 10 && scissor_sets == 0 &&
              !world_hud_patch_now((uint64_t)(uintptr_t)pane_constants_block, 64, bound, &bound_size));
        report.camera[2] = -100;
        CHECK(world_hud_draw_now());
        const VrHudMove *placed = &vr_world_hud.move;
        const VrHudMove placed_from_stale = *placed;
        vr_world_hud.move = (VrHudMove){0};
        CHECK(world_hud_draw_now());
        CHECK(!memcmp(placed_from_stale.clip, placed->clip, sizeof placed->clip) &&
              !memcmp(placed_from_stale.clip_w, placed->clip_w, sizeof placed->clip_w) &&
              !memcmp(placed_from_stale.flat_scale, placed->flat_scale, sizeof placed->flat_scale) &&
              !memcmp(placed_from_stale.flat_offset, placed->flat_offset, sizeof placed->flat_offset) &&
              placed_from_stale.card == placed->card && placed_from_stale.post_offset == placed->post_offset &&
              placed_from_stale.half_left == placed->half_left && placed_from_stale.half_right == placed->half_right);
    }

    check_reticle_placement();
    printf("vr_world_hud fixture: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
