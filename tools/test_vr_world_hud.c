// Asset-free check of the world-tied HUD's placement (runtime/nvn/vulkan/vr_world_hud.h, D073, D088): the move of a
// marker's, a reticle's and the fade's draw into its eye's half, the patched vertex constants, the scissor and the
// fragment position's scale. By hand: a reticle and a marker straight ahead at the aim distance land at each eye's own
// centre shifted by that eye's parallax, at the HUD panel's size when Link is as far; a point behind the UI camera or
// behind the eye is refused and counted, leaving the move as it was, and so are an unmatched draw, an uncertified eye
// and a FOV with no width; the fade's move, its patched constants and its fragment scale; the scissor of a moved draw
// (a game scissor moved with it, none, one moved out of the half) and setting it only when it changes; the switches
// as exec.c reads them from the configuration (runtime/pc_vr_config.c; SAVAGEWIND_PC_VR_WORLD_HUD: only "0" is off;
// SAVAGEWIND_PC_VR_WORLD_HUD_ROLL: unset turns with the world's up, "0" upright, a number of degrees more); a stale
// move with every field set, kept
// whole by a refused draw and ignored by the hooks, replaced whole by a placed one. Then, on 1,000,000 random draws
// (markers linked by pane, reticles, fades, flat pieces, other roles and blocks; both eyes; turned with the world's
// up, upright, and with the test turn; one in twenty starting from a stale move with every field set), the same bits
// as the code at ca3cd86 (exec.c),
// transcribed below with the executor's state as this file's: whether the draw is placed, the move, the counters and
// every value its log line shows; then for the draw the scissor, the fragment position's scale and the patched
// constants, with the router's answer and with the move forced on. The hub (exec.c's part) is transcribed as it is now.
// No renderer, GPU, game or headset is involved.
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
static uint64_t present_count;
static uint64_t renderer_present_count(void) { return present_count; }
static bool report_found;  // (pc_vr_hud_point_by_seq finds the draw's report)
static PcVrHudPoint report;
bool pc_vr_hud_point_by_seq(uint32_t seq, PcVrHudPoint *out) {
    (void)seq;
    if (report_found) *out = report;
    return report_found;
}
static double focus_distance;
static double pc_vr_camera_focus_distance(void) { return focus_distance; }
// The switches. The code as it was read them once into statics of its functions, here this file's:
// SAVAGEWIND_PC_VR_WORLD_HUD into world_hud_off, SAVAGEWIND_PC_VR_WORLD_HUD_ROLL into keep_image_up and test_turn (at
// -1, from the environment); the random cases set them directly. The hub as it is now reads them from the configuration
// (pc_vr_config()), here configuration, which the random cases set to the same values.
static int world_hud_off = -1;
static int keep_image_up = -1;
static double test_turn;
static PcVrConfig configuration;
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

// ------------------------------------------------------------------------------------- the code at ca3cd86 (exec.c)
static struct {
    bool active;
    float linear[4], offset[2];  // the move, in guest pixels: a row-major 2x2 and an offset
    float clip[6];
    float flat_scale[2], flat_offset[2];
    float half_left, half_right;  // the eye's half of the final image, which the scissor stays inside
    uint64_t draws, unmatched, behind, logged, frame_draws, exact;
} old_hud;
static MarkerLog old_log;
static int old_fade_eye;

#define VR_WORLD_HUD_SIZE_MIN_BEFORE 0.4
#define VR_WORLD_HUD_SIZE_MAX_BEFORE 1.5
#define VR_AIM_SIZE_MIN_BEFORE 0.6
#define VR_WORLD_HUD_PANE_MAX_BEFORE 400.0f
#define VR_UI_TAN_X_BEFORE 0.82904
#define VR_HUD_LAYER_X_BEFORE 0.92f
#define VR_HUD_LAYER_Y_BEFORE 0.46f
#define VR_FADE_GROW_BEFORE 1.3f

static void world_hud_place_before(const float *viewport, double labo_offset, const double anchor[2],
                                   const double eye[2], const double scale[2], const double up[2]) {
    // In pixels: the draw's right (1, 0) goes to (-up_y, up_x) and its down (0, 1) to (-up_x, -up_y), each scaled.
    const double linear[4] = {-up[1] * scale[0], -up[0] * scale[1], up[0] * scale[0], -up[1] * scale[1]};
    const double offset[2] = {eye[0] - (linear[0] * anchor[0] + linear[1] * anchor[1]),
                              eye[1] - (linear[2] * anchor[0] + linear[3] * anchor[1])};
    for (unsigned i = 0; i < 4; ++i) old_hud.linear[i] = (float)linear[i];
    old_hud.offset[0] = (float)offset[0];
    old_hud.offset[1] = (float)offset[1];
    const double half_w = viewport[2] * 0.5, half_h = viewport[3] * 0.5;
    const double centre[2] = {viewport[0] + half_w, viewport[1] + half_h};
    const double ndc[4] = {linear[0], linear[1] * -half_h / half_w, linear[2] * half_w / -half_h, linear[3]};
    const double shift[2] = {(linear[0] * centre[0] + linear[1] * centre[1] + offset[0] - centre[0]) / half_w,
                             (linear[2] * centre[0] + linear[3] * centre[1] + offset[1] - centre[1]) / -half_h};
    old_hud.clip[0] = (float)ndc[0];
    old_hud.clip[1] = (float)ndc[1];
    old_hud.clip[2] = (float)(ndc[0] * labo_offset + shift[0] - labo_offset);
    old_hud.clip[3] = (float)ndc[2];
    old_hud.clip[4] = (float)ndc[3];
    old_hud.clip[5] = (float)(ndc[2] * labo_offset + shift[1]);
    old_hud.flat_scale[0] = (float)scale[0];
    old_hud.flat_scale[1] = (float)scale[1];
    old_hud.flat_offset[0] = (float)(eye[0] - scale[0] * anchor[0]);
    old_hud.flat_offset[1] = (float)(eye[1] - scale[1] * anchor[1]);
    old_hud.half_left = viewport[0];
    old_hud.half_right = viewport[0] + viewport[2];
}

// (the bind, ubo_span(vk_transient_copy(block, copied, 256), copied), copies the block out)
static bool world_hud_patch_before(uint64_t address, uint64_t size, float *bound, uint64_t *bound_size) {
    if (!old_hud.active || size < 64) return false;
    static float block[0x10000 / 4];
    const uint64_t copied = size > sizeof block ? sizeof block : size;
    memcpy(block, (const void *)(uintptr_t)address, copied);
    const float *clip = old_hud.clip;
    for (unsigned column = 0; column < 4; ++column) {
        const float row_x = block[column], row_y = block[4 + column], row_w = block[12 + column];
        block[column] = clip[0] * row_x + clip[1] * row_y + clip[2] * row_w;
        block[4 + column] = clip[3] * row_x + clip[4] * row_y + clip[5] * row_w;
    }
    memcpy(bound, block, copied);
    *bound_size = copied;
    return true;
}

static bool world_hud_draw_before(void) {
    if (world_hud_off < 0)
        world_hud_off = getenv("SAVAGEWIND_PC_VR_WORLD_HUD") && !strcmp(getenv("SAVAGEWIND_PC_VR_WORLD_HUD"), "0");
    if (world_hud_off || !st.rt.color[0]) return false;
    if (vr_draw_marker == PC_VR_MARKER_FLAT) return false;
    if (!vr_draw_marker && vr_draw_role != PC_VR_PANE_RETICLE) return false;
    const Range pane_constants = st.ubuf[0][0], offset_constants = st.ubuf[0][10];
    if (!pane_constants.addr || pane_constants.size < 64 || !offset_constants.addr || offset_constants.size < 4 ||
        st.ds[0])
        return false;  // (no depth test: ui2d's)
    const float *block = (const float *)(uintptr_t)pane_constants.addr;
    const bool flat = block[0] > 0 && fabsf(fabsf(block[5]) / block[0] - 16.0f / 9.0f) < 0.002f;
    float origin_x, origin_y;  // where the pane's (or the text's) origin is, in the layer's NDC before the Labo offset
    float size = 0;            // the pane's larger side in layout units (0: a text)
    if (flat && block[5] > 0 && fabsf(block[5] - 2.74748f) < 0.001f && block[14] == -1 && block[15] == 0 &&
        !block[1] && !block[2] && !block[3] && !block[4] && !block[6] && !block[7] && !block[8] && !block[9] &&
        !block[12] && !block[13] && pane_constants.size >= 34 * 16) {
        const float *model_view = block + 16, origin_depth = -model_view[11];
        if (!(origin_depth > 0)) return false;
        origin_x = block[0] * model_view[3] / origin_depth;
        origin_y = block[5] * model_view[7] / origin_depth;
        const float *rect = block + 33 * 4;  // (the pane's rectangle: width, height, left, top)
        size = fmaxf(fabsf(rect[0]) * hypotf(model_view[0], model_view[4]),
                     fabsf(rect[1]) * hypotf(model_view[1], model_view[5]));
    } else if (pane_constants.size == 128 && block[14] == -1 && block[15] > 0 && !block[2] && !block[6] && !block[8] &&
               !block[9] && !block[12] && !block[13]) {
        origin_x = block[3] / block[15];
        origin_y = block[7] / block[15];
    } else return false;
    if (size > VR_WORLD_HUD_PANE_MAX_BEFORE) return false;  // (a screen-sized pane is no marker's)
    const float *viewport = st.viewport[0], half_width = st.rt.color[0]->width * 0.5f;
    if (fabsf(viewport[2] - half_width) > 1 || !(viewport[3] > 0)) return false;  // (a draw over one half only)
    const unsigned eye = viewport[0] + viewport[2] * 0.5f >= half_width;
    if (!(vr_eyes.frame.certified >> eye & 1)) return false;
    const PcVrRenderCertificate *cert = &vr_eyes.frame.eyes[eye];
    const XrPoseSnapshot *pose = &cert->native_snapshot;
    if (!(cert->units_per_meter > 0) || !pose->calibration.epoch) return false;

    PcVrHudPoint linked;
    const PcVrHudPoint *marker = NULL;
    const char *how = "none";
    if (vr_draw_marker && pc_vr_hud_point_by_seq(vr_draw_marker, &linked)) {
        linked.layout[0] *= VR_HUD_LAYER_X_BEFORE;
        linked.layout[1] *= VR_HUD_LAYER_Y_BEFORE;
        marker = &linked;
        how = "pane";
        ++old_hud.exact;
    } else if (vr_draw_role == PC_VR_PANE_RETICLE && vr_aim_result.distance > 0 &&
               vr_aim_result.frame + 8 >= renderer_present_count()) {
        linked = (PcVrHudPoint){.camera = {0, 0, -(float)vr_aim_result.distance}};
        marker = &linked;
        how = "reticle";
    }
    if (!marker) {
        ++old_hud.unmatched;
        return false;
    }

    const double depth = -marker->camera[2];
    if (!(depth > 1e-3)) {
        ++old_hud.behind;
        return false;
    }
    const double units_per_meter = cert->units_per_meter;
    const double point[3] = {(marker->camera[0] + marker->offset * VR_UI_TAN_X_BEFORE * depth) / units_per_meter,
                             marker->camera[1] / units_per_meter, marker->camera[2] / units_per_meter};
    const double heading[4] = {pose->calibration.heading_xyzw[0], pose->calibration.heading_xyzw[1],
                               pose->calibration.heading_xyzw[2], pose->calibration.heading_xyzw[3]};
    const XrEyeSnapshot *eye_pose = &pose->eyes[eye];
    const double to_eye[4] = {-eye_pose->orientation_xyzw[0], -eye_pose->orientation_xyzw[1],
                              -eye_pose->orientation_xyzw[2], eye_pose->orientation_xyzw[3]};
    const double left = tan(eye_pose->fov_left_right_up_down[0]), right = tan(eye_pose->fov_left_right_up_down[1]);
    const double up = tan(eye_pose->fov_left_right_up_down[2]), down = tan(eye_pose->fov_left_right_up_down[3]);
    double local[3], from_eye[3], view[3], image[2];
    vr_rotate(heading, point, local);
    for (unsigned i = 0; i < 3; ++i)
        from_eye[i] = pose->calibration.origin_xyz[i] + local[i] - eye_pose->position_xyz[i];
    if (!vr_eye_image_position(from_eye, eye_pose->orientation_xyzw, (const double[4]){left, right, up, down}, 1e-4,
                               view, image)) {
        ++old_hud.behind;
        return false;
    }
    if (!(right > left) || !(up > down)) return false;
    const double labo_offset = ((const float *)(uintptr_t)offset_constants.addr)[0];  // (this half's Labo offset, NDC)
    const double tan_per_pixel = VR_SCREEN_WIDTH / (2 * VR_SCREEN_HALF_W);  // (on the HUD panel)
    const double point_x = viewport[0] + (marker->layout[0] / 640.0 + labo_offset + 1) * 0.5 * viewport[2];
    const double point_y = viewport[1] + (1 - marker->layout[1] / 360.0) * 0.5 * viewport[3];
    const double scale_x = tan_per_pixel * viewport[2] / (right - left);
    const double scale_y = tan_per_pixel * viewport[3] / (up - down);
    const double focus = pc_vr_camera_focus_distance();
    const double away = sqrt(from_eye[0] * from_eye[0] + from_eye[1] * from_eye[1] + from_eye[2] * from_eye[2]);
    const double least = marker->frame ? VR_WORLD_HUD_SIZE_MIN_BEFORE : VR_AIM_SIZE_MIN_BEFORE;  // (frame 0: crosshair)
    const double grow = fmin(VR_WORLD_HUD_SIZE_MAX_BEFORE, fmax(least, (focus > 0 ? focus : 1) / away));
    if (keep_image_up < 0) {
        const char *option = getenv("SAVAGEWIND_PC_VR_WORLD_HUD_ROLL");
        keep_image_up = option && !strcmp(option, "0");
        test_turn = option ? atof(option) * (3.14159265358979323846 / 180.0) : 0;
    }
    double image_up[2] = {0, -1};
    if (!keep_image_up) {
        const double local_up[3] = {0, 1, 0};
        double view_up[3];
        vr_rotate(to_eye, local_up, view_up);
        const double tan_step_x = (-view_up[0] * view[2] + view[0] * view_up[2]) / (view[2] * view[2]);
        const double tan_step_y = (-view_up[1] * view[2] + view[1] * view_up[2]) / (view[2] * view[2]);
        const double step_x = tan_step_x * viewport[2] / (right - left), step_y = -tan_step_y * viewport[3] / (up - down);
        const double length = sqrt(step_x * step_x + step_y * step_y);
        if (length > 1e-9) {
            image_up[0] = step_x / length;
            image_up[1] = step_y / length;
        }
        if (test_turn != 0) {  // (clockwise in the image, whose y runs down)
            const double turned_x = image_up[0] * cos(test_turn) - image_up[1] * sin(test_turn);
            image_up[1] = image_up[0] * sin(test_turn) + image_up[1] * cos(test_turn);
            image_up[0] = turned_x;
        }
    }
    const double anchor[2] = {point_x, point_y};
    const double eye_at[2] = {viewport[0] + image[0] * viewport[2], viewport[1] + image[1] * viewport[3]};
    const double scale[2] = {grow * scale_x, grow * scale_y};
    world_hud_place_before(viewport, labo_offset, anchor, eye_at, scale, image_up);
    ++old_hud.draws;
    ++old_hud.frame_draws;
    if (old_hud.logged < 16 || !(old_hud.draws % 8192)) ++old_hud.logged;
    old_log = (MarkerLog){how, eye, {origin_x * 640, origin_y * 360, marker->layout[0], marker->layout[1],
                                     marker->offset, marker->camera[0], marker->camera[1], marker->camera[2],
                                     depth / units_per_meter, anchor[0], anchor[1], eye_at[0], eye_at[1], scale[0],
                                     scale[1], atan2(image_up[0], -image_up[1]) * (180.0 / 3.14159265358979323846)}};
    return true;
}

static bool hud_fade_before(void) {
    if (!st.rt.color[0] || vr_draw_role != PC_VR_PANE_FADE) return false;  // (the screen Fade's black pane)
    const Range pane_constants = st.ubuf[0][0];
    if (!pane_constants.addr || pane_constants.size < 34 * 16 || st.ds[0]) return false;
    const float *block = (const float *)(uintptr_t)pane_constants.addr, *model_view = block + 16,
                *rect = block + 33 * 4;
    if (!(block[0] > 0 && fabsf(block[5] - 2.74748f) < 0.001f && fabsf(block[5] / block[0] - 16.0f / 9.0f) < 0.002f &&
          block[14] == -1 && block[15] == 0) || !(rect[0] > 0 && rect[1] > 0)) return false;
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    for (int k = 0; k < 4; k++) {
        const float corner_x = (k & 1) * rect[0] + rect[2], corner_y = rect[3] - (k >> 1) * rect[1];
        const float corner_depth = -(model_view[8] * corner_x + model_view[9] * corner_y + model_view[11]);
        if (!(corner_depth > 0)) return false;
        const float ndc_x =
            block[0] * (model_view[0] * corner_x + model_view[1] * corner_y + model_view[3]) / corner_depth;
        const float ndc_y =
            block[5] * (model_view[4] * corner_x + model_view[5] * corner_y + model_view[7]) / corner_depth;
        min_x = fminf(min_x, ndc_x);
        max_x = fmaxf(max_x, ndc_x);
        min_y = fminf(min_y, ndc_y);
        max_y = fmaxf(max_y, ndc_y);
    }
    if (!(min_x <= -0.98f && max_x >= 0.98f && min_y <= -0.98f && max_y >= 0.98f)) return false;
    const float *viewport = st.viewport[0], half_width = st.rt.color[0]->width * 0.5f;
    if (fabsf(viewport[2] - half_width) > 1 || !(viewport[3] > 0)) return false;
    const Range offset_constants = st.ubuf[0][10];
    const double labo_offset = offset_constants.addr && offset_constants.size >= 4 ?
        ((const float *)(uintptr_t)offset_constants.addr)[0] : 0;
    const double centre[2] = {viewport[0] + viewport[2] * 0.5, viewport[1] + viewport[3] * 0.5};
    const double grow[2] = {VR_FADE_GROW_BEFORE, VR_FADE_GROW_BEFORE}, image_up[2] = {0, -1};
    world_hud_place_before(viewport, labo_offset, centre, centre, grow, image_up);
    old_fade_eye = viewport[0] >= half_width * 0.5f;
    return true;
}

static void world_hud_scissor_before(void) {
    if (!old_hud.active || !dc.sc.extent.width || !dc.sc.extent.height) return;
    const float *viewport = st.viewport[0];
    double left = old_hud.half_left, right = old_hud.half_right, top = 0, bottom = st.rt.color[0]->height;
    const double game_left = st.scissor[0][2] >= 16384 ? viewport[0] : st.scissor[0][0];
    const double game_right = st.scissor[0][2] >= 16384 ? viewport[0] + viewport[2] : game_left + st.scissor[0][2];
    const double game_top = st.scissor[0][3] >= 16384 ? viewport[1] : st.scissor[0][1];
    const double game_bottom = st.scissor[0][3] >= 16384 ? viewport[1] + viewport[3] : game_top + st.scissor[0][3];
    if (game_left > viewport[0] + 0.5 || game_right < viewport[0] + viewport[2] - 0.5 || game_top > viewport[1] + 0.5 ||
        game_bottom < viewport[1] + viewport[3] - 0.5) {
        double moved_left = 1e30, moved_right = -1e30, moved_top = 1e30, moved_bottom = -1e30;
        for (unsigned corner = 0; corner < 4; ++corner) {
            const double corner_x = corner & 1 ? game_right : game_left, corner_y = corner & 2 ? game_bottom : game_top;
            const double moved_x = old_hud.offset[0] + old_hud.linear[0] * corner_x + old_hud.linear[1] * corner_y;
            const double moved_y = old_hud.offset[1] + old_hud.linear[2] * corner_x + old_hud.linear[3] * corner_y;
            moved_left = fmin(moved_left, moved_x);
            moved_right = fmax(moved_right, moved_x);
            moved_top = fmin(moved_top, moved_y);
            moved_bottom = fmax(moved_bottom, moved_y);
        }
        left = fmax(left, moved_left);
        right = fmin(right, moved_right);
        top = fmax(top, moved_top);
        bottom = fmin(bottom, moved_bottom);
    }
    int left_px = (int)floor(left * vp_sx + vp_ox), right_px = (int)ceil(right * vp_sx + vp_ox);
    int top_px = (int)floor(top * vp_sy + vp_oy), bottom_px = (int)ceil(bottom * vp_sy + vp_oy);
    if (left_px < 0) left_px = 0;
    if (top_px < 0) top_px = 0;
    if (right_px > (int)pass_w) right_px = (int)pass_w;
    if (bottom_px > (int)pass_h) bottom_px = (int)pass_h;
    if (right_px < left_px) right_px = left_px;
    if (bottom_px < top_px) bottom_px = top_px;
    Rect rect = {{left_px, top_px}, {(uint32_t)(right_px - left_px), (uint32_t)(bottom_px - top_px)}};
    if (!memcmp(&dc.sc, &rect, sizeof rect)) return;
    rec_CmdSetScissor(vk_cmd(), 0, 1, &rect);
    dc.sc = rect;
}

static void world_hud_rscale_before(float rscale[4]) {
    if (!old_hud.active) return;
    rscale[2] += rscale[0] * old_hud.flat_offset[0];
    rscale[3] += rscale[1] * old_hud.flat_offset[1];
    rscale[0] *= old_hud.flat_scale[0];
    rscale[1] *= old_hud.flat_scale[1];
}

// ------------------------------------------------------------------------------ the hub now (exec.c), on the header
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
    const PcVrConfig *config = &configuration;
    if (!config->play.world_hud || !st.rt.color[0]) return false;
    if (vr_draw_marker == PC_VR_MARKER_FLAT) return false;
    if (!vr_draw_marker && vr_draw_role != PC_VR_PANE_RETICLE) return false;
    const Range pane_constants = st.ubuf[0][0], offset_constants = st.ubuf[0][10];
    if (!pane_constants.addr || pane_constants.size < 64 || !offset_constants.addr || offset_constants.size < 4 ||
        st.ds[0])
        return false;  // (no depth test: ui2d's)
    float origin[2];  // where the pane's (or the text's) origin is, in the layer's NDC before the Labo offset (the log)
    if (!vr_hud_marker_block((const float *)(uintptr_t)pane_constants.addr, pane_constants.size, origin))
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
    } else if (vr_draw_role == PC_VR_PANE_RETICLE && vr_aim_result.distance > 0 &&
               vr_aim_result.frame + 8 >= renderer_present_count()) {
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
    vr_hud_marker_move(&seen, marker, viewport, labo_offset, tan_per_pixel, focus, config->play.world_hud_upright,
                       config->play.world_hud_test_turn, &vr_world_hud.move, &placed);
    ++vr_world_hud.draws;
    ++vr_world_hud.frame_draws;
    if (vr_world_hud.logged < 16 || !(vr_world_hud.draws % 8192)) ++vr_world_hud.logged;
    new_log = (MarkerLog){how, eye, {origin[0] * 640, origin[1] * 360, marker->layout[0], marker->layout[1],
                                     marker->offset, marker->camera[0], marker->camera[1], marker->camera[2],
                                     seen.depth / cert->units_per_meter, placed.anchor[0], placed.anchor[1],
                                     placed.eye_at[0], placed.eye_at[1], placed.scale[0], placed.scale[1],
                                     atan2(placed.image_up[0], -placed.image_up[1]) * (180.0 / 3.14159265358979323846)}};
    return true;
}

static bool hud_fade_now(void) {
    if (!st.rt.color[0] || vr_draw_role != PC_VR_PANE_FADE) return false;  // (the screen Fade's black pane)
    const Range pane_constants = st.ubuf[0][0];
    if (!pane_constants.addr || pane_constants.size < 34 * 16 || st.ds[0]) return false;
    if (!vr_hud_fade_block((const float *)(uintptr_t)pane_constants.addr)) return false;
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
// The two states hold the same move, counters and answer.
static bool same_state(void) {
    const VrHudMove *move = &vr_world_hud.move;
    return old_hud.active == vr_world_hud.active && !memcmp(old_hud.linear, move->linear, sizeof move->linear) &&
           !memcmp(old_hud.offset, move->offset, sizeof move->offset) &&
           !memcmp(old_hud.clip, move->clip, sizeof move->clip) &&
           !memcmp(old_hud.flat_scale, move->flat_scale, sizeof move->flat_scale) &&
           !memcmp(old_hud.flat_offset, move->flat_offset, sizeof move->flat_offset) &&
           !memcmp(&old_hud.half_left, &move->half_left, sizeof move->half_left) &&
           !memcmp(&old_hud.half_right, &move->half_right, sizeof move->half_right) &&
           old_hud.draws == vr_world_hud.draws && old_hud.unmatched == vr_world_hud.unmatched &&
           old_hud.behind == vr_world_hud.behind && old_hud.logged == vr_world_hud.logged &&
           old_hud.frame_draws == vr_world_hud.frame_draws && old_hud.exact == vr_world_hud.exact;
}

static bool same_log(void) {
    const bool same_how = old_log.how == new_log.how || (old_log.how && new_log.how && !strcmp(old_log.how, new_log.how));
    return same_how && old_log.eye == new_log.eye &&
           !memcmp(old_log.values, new_log.values, sizeof old_log.values);
}

// A deterministic pseudo-random sequence (xorshift64), and a number in [low, high) from it.
static uint64_t random_state = 0x9e3779b97f4a7c15ull;
static double random_between(double low, double high) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return low + (high - low) * (double)(random_state >> 11) / 9007199254740992.0;
}
static bool random_chance(double probability) { return random_between(0, 1) < probability; }

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

static void random_small_rotation(double largest_angle, double out[4]) {
    double axis[3] = {random_between(-1, 1), random_between(-1, 1), random_between(-1, 1)};
    const double length = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]) + 1e-12;
    quaternion_about_axis(axis[0] / length, axis[1] / length, axis[2] / length,
                          random_between(-largest_angle, largest_angle), out);
}

// A move with every field set to its own value (an earlier draw's, stale), and the same in the old code's state.
static void random_move(VrHudMove *move) {
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
static void set_old_move(const VrHudMove *move) {
    memcpy(old_hud.linear, move->linear, sizeof old_hud.linear);
    memcpy(old_hud.offset, move->offset, sizeof old_hud.offset);
    memcpy(old_hud.clip, move->clip, sizeof old_hud.clip);
    memcpy(old_hud.flat_scale, move->flat_scale, sizeof old_hud.flat_scale);
    memcpy(old_hud.flat_offset, move->flat_offset, sizeof old_hud.flat_offset);
    old_hud.half_left = move->half_left;
    old_hud.half_right = move->half_right;
}
static unsigned stale_moves, stale_moves_kept;

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
static float pane_constants_block[512], offset_constants_block[4], patch_source[0x11000 / 4];
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

static void random_pane_block(void) {
    const double depth = random_chance(0.03) ? random_between(-10, 0) : random_between(500, 1500);
    pane_block(pane_constants_block, depth, random_between(0.5, 1.5), random_chance(0.7) ? 0 : random_between(-1, 1),
               random_between(-600, 600), random_between(-360, 360), random_between(1, 500), random_between(1, 500),
               random_between(-250, 0), random_between(0, 250));
    st.ubuf[0][0].size = random_chance(0.9) ? 34 * 16 : 34 * 16 + 16 * (uint64_t)random_between(1, 10);
}

static void random_text_block(void) {
    float *block = pane_constants_block;
    for (unsigned i = 0; i < 32; ++i) block[i] = (float)random_between(-2, 2);
    block[2] = block[6] = block[8] = block[9] = block[12] = block[13] = 0;
    block[14] = -1;
    block[15] = random_chance(0.05) ? (float)random_between(-1, 0) : (float)random_between(0.5, 2);
    st.ubuf[0][0].size = 128;
}

// A fade's pane: a rectangle about the layer's centre that covers it, or nearly.
static void random_fade_block(void) {
    const double depth = random_between(500, 1500), width = random_between(0.9, 1.6) * depth,
                 height = random_between(0.6, 0.9) * depth;
    pane_block(pane_constants_block, depth, 1, 0, random_between(-20, 20), random_between(-20, 20), width, height,
               -width / 2 + random_between(-10, 10), height / 2 + random_between(-10, 10));
    if (random_chance(0.05)) pane_constants_block[16 + 8] = (float)random_between(-0.01, 0.01);
    st.ubuf[0][0].size = 34 * 16;
}

// Now and then one of the values the blocks are told by, off.
static void perturb_block(void) {
    if (!random_chance(0.15)) return;
    static const unsigned checked[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 132, 133};
    const unsigned index = checked[(unsigned)random_between(0, sizeof checked / sizeof checked[0])];
    if (index == 5 && random_chance(0.7)) pane_constants_block[5] += (float)random_between(-0.002, 0.002);
    else pane_constants_block[index] = random_chance(0.3) ? 0 : (float)random_between(-3, 3);
    if (random_chance(0.2)) st.ubuf[0][0].size = (uint64_t)random_between(0, 600);
}

// A random draw: what it is, its constants, its half, the eyes, the marker report, the aim, the switches.
static void random_draw(void) {
    memset(&st, 0, sizeof st);
    memset(&vr_eyes, 0, sizeof vr_eyes);
    // (the target, and the draw's half)
    target.width = (uint32_t)random_between(800, 4000);
    target.height = (uint32_t)random_between(500, 2200);
    st.rt.color[0] = random_chance(0.98) ? &target : NULL;
    const float half_width = target.width * 0.5f;
    const bool right_half = random_chance(0.5);
    float *viewport = st.viewport[0];
    viewport[0] = right_half ? half_width : 0;
    viewport[1] = 0;
    viewport[2] = half_width;
    viewport[3] = (float)target.height;
    const double viewport_kind = random_between(0, 1);
    if (viewport_kind < 0.1) {
        viewport[0] += (float)random_between(-0.5, 0.5);
        viewport[2] += (float)random_between(-0.99, 0.99);
    } else if (viewport_kind < 0.15) {
        viewport[2] += (float)random_between(-5, 5);
    } else if (viewport_kind < 0.17) {
        viewport[3] = 0;
    } else if (viewport_kind < 0.2) {
        viewport[1] = (float)random_between(0, 100);
        viewport[3] = (float)random_between(100, target.height);
    }
    // (the game's scissor: none, inside the viewport, or anything)
    const double scissor_kind = random_between(0, 1);
    int32_t *scissor = st.scissor[0];
    if (scissor_kind < 0.4) {
        scissor[0] = scissor[1] = 0;
        scissor[2] = scissor[3] = 16384 + (int32_t)random_between(0, 1000);
    } else if (scissor_kind < 0.6) {
        scissor[0] = (int32_t)viewport[0];
        scissor[1] = (int32_t)viewport[1];
        scissor[2] = (int32_t)viewport[2];
        scissor[3] = (int32_t)viewport[3];
    } else if (scissor_kind < 0.9) {
        scissor[0] = (int32_t)(viewport[0] + random_between(0, viewport[2] / 2));
        scissor[1] = (int32_t)(viewport[1] + random_between(0, viewport[3] / 2));
        scissor[2] = (int32_t)random_between(1, viewport[2] / 2);
        scissor[3] = (int32_t)random_between(1, viewport[3] / 2);
    } else {
        for (unsigned i = 0; i < 4; ++i) scissor[i] = (int32_t)random_between(-100, 20000);
    }
    st.ds[0] = random_chance(0.02);
    // (the Labo offset, c13)
    offset_constants_block[0] = (float)random_between(-0.2, 0.2);
    st.ubuf[0][10] = (Range){random_chance(0.98) ? (uint64_t)(uintptr_t)offset_constants_block : 0,
                             random_chance(0.97) ? 16 : (uint64_t)random_between(0, 4)};
    // (the eyes: a head turned a little from the calibrated heading, now and then anywhere)
    double heading[4], head[4], turn[4];
    quaternion_about_axis(0, 1, 0, random_between(-3.1, 3.1), heading);
    if (random_chance(0.9)) {
        random_small_rotation(0.35, turn);
        quaternion_product(heading, turn, head);
    } else {
        random_small_rotation(3.1, head);
    }
    const float origin[3] = {(float)random_between(-0.5, 0.5), 1.6f + (float)random_between(-0.2, 0.2),
                             (float)random_between(-0.5, 0.5)};
    float fov[4] = {(float)random_between(-0.9, -0.7), (float)random_between(0.7, 0.9),
                    (float)random_between(0.7, 0.9), (float)random_between(-0.9, -0.7)};
    const double fov_kind = random_between(0, 1);
    if (fov_kind < 0.01) fov[1] = fov[0];
    else if (fov_kind < 0.02) fov[3] = fov[2];
    else if (fov_kind < 0.03) {
        const float left = fov[0];
        fov[0] = fov[1];
        fov[1] = left;
    }
    const double scale_kind = random_between(0, 1);
    const double units_per_meter = scale_kind < 0.01 ? 0 : scale_kind < 0.015 ? NAN : random_between(5, 15);
    set_eyes(heading, origin, head, random_between(0.055, 0.075), fov, units_per_meter,
             random_chance(0.98) ? (uint64_t)random_between(1, 1000) : 0);
    vr_eyes.frame.certified = random_chance(0.9) ? 3 : (unsigned)random_between(0, 4);
    // (the marker report: a point ahead of the UI camera, now and then behind it or at it)
    report_found = random_chance(0.92);
    const double depth = 0.3 * pow(10000, random_between(0, 1));
    report = (PcVrHudPoint){.layout = {(float)random_between(-640, 640), (float)random_between(-360, 360)},
                            .camera = {(float)(random_between(-0.8, 0.8) * depth),
                                       (float)(random_between(-0.5, 0.5) * depth), (float)-depth},
                            .offset = random_chance(0.3) ? 0 : (float)random_between(-0.3, 0.3),
                            .frame = random_chance(0.1) ? 0 : (uint64_t)random_between(1, 1e6),
                            .seq = (uint32_t)random_between(1, PC_VR_MARKER_SEQ_MAX)};
    const double report_kind = random_between(0, 1);
    if (report_kind < 0.03) report.camera[2] = (float)random_between(0, 50);
    else if (report_kind < 0.05) report.camera[2] = -0.0005f;
    // (the aim: fresh within 8 presents, or not)
    present_count = 100 + (uint64_t)random_between(0, 1e6);
    vr_aim_result.distance = random_chance(0.05) ? (random_chance(0.5) ? 0 : -1) : 0.25 * pow(1600, random_between(0, 1));
    vr_aim_result.frame = present_count - (uint64_t)random_between(0, 13);
    // (Link's distance, and the switches)
    focus_distance = random_chance(0.1) ? random_between(-1, 0) : random_between(0.5, 10);
    keep_image_up = random_chance(0.3);
    test_turn = random_chance(0.8) ? 0 : random_between(-3.1, 3.1);
    configuration.play.world_hud_upright = keep_image_up;
    configuration.play.world_hud_test_turn = test_turn;
    // (what the draw is)
    st.ubuf[0][0].addr = random_chance(0.99) ? (uint64_t)(uintptr_t)pane_constants_block : 0;
    vr_draw_role = PC_VR_PANE_NONE;
    vr_draw_marker = 0;
    const double draw_kind = random_between(0, 1);
    if (draw_kind < 0.67) {  // (a marker linked by pane; a reticle's pane)
        if (draw_kind < 0.42) {
            vr_draw_marker = (uint32_t)random_between(1, PC_VR_MARKER_SEQ_MAX);
            if (random_chance(0.1)) vr_draw_role = PC_VR_PANE_RETICLE;
        } else {
            vr_draw_role = PC_VR_PANE_RETICLE;
        }
        const double block_kind = random_between(0, 1);
        if (block_kind < 0.6) random_pane_block();
        else if (block_kind < 0.9) random_text_block();
        else {
            for (unsigned i = 0; i < 136; ++i) pane_constants_block[i] = (float)random_between(-3, 3);
            st.ubuf[0][0].size = (uint64_t)random_between(0, 600);
        }
    } else if (draw_kind < 0.8) {  // (the fade)
        vr_draw_role = PC_VR_PANE_FADE;
        if (random_chance(0.2)) vr_draw_marker = (uint32_t)random_between(1, PC_VR_MARKER_SEQ_MAX);
        random_fade_block();
    } else if (draw_kind < 0.85) {  // (a piece at the game's own fixed place)
        vr_draw_marker = PC_VR_MARKER_FLAT;
        random_pane_block();
    } else if (draw_kind < 0.9) {  // (no link and another role)
        vr_draw_role = (PcVrPaneRole)(unsigned)random_between(0, PC_VR_PANE_BOX + 1);
        if (vr_draw_role == PC_VR_PANE_RETICLE || vr_draw_role == PC_VR_PANE_FADE) vr_draw_role = PC_VR_PANE_BOX;
        random_pane_block();
    } else {  // (anything)
        vr_draw_marker = random_chance(0.5) ? 0 : (uint32_t)random_between(1, PC_VR_MARKER_FLAT + 1);
        vr_draw_role = (PcVrPaneRole)(unsigned)random_between(0, PC_VR_PANE_BOX + 1);
        for (unsigned i = 0; i < 136; ++i) pane_constants_block[i] = (float)random_between(-3, 3);
        st.ubuf[0][0].size = (uint64_t)random_between(0, 600);
    }
    perturb_block();
    // (the guest-to-device mapping and the pass)
    vp_sx = random_chance(0.5) ? 1 : (float)random_between(0.5, 3);
    vp_sy = random_chance(0.5) ? vp_sx : (float)random_between(0.5, 3);
    vp_ox = random_chance(0.5) ? 0 : (float)random_between(-100, 100);
    vp_oy = random_chance(0.5) ? 0 : (float)random_between(-100, 100);
    pass_w = (uint32_t)(target.width * vp_sx + random_between(-50, 50));
    pass_h = (uint32_t)(target.height * vp_sy + random_between(-50, 50));
}

// Runs the router's question on both (draw_: the fade first, then the marker), then the draw's hooks: the scissor
// with the scissor set before (sc), the fragment scale, the patched constants of a block of patch_size bytes.
static unsigned scissor_compared, rscale_compared, patch_compared, patch_disagreements, hook_disagreements;
static void compare_hooks(Rect sc, uint64_t patch_size) {
    dc.sc = sc;
    scissor_sets = 0;
    world_hud_scissor_before();
    const Rect old_sc = dc.sc;
    const unsigned old_sets = scissor_sets;
    dc.sc = sc;
    scissor_sets = 0;
    world_hud_scissor_now();
    hook_disagreements += memcmp(&old_sc, &dc.sc, sizeof old_sc) || old_sets != scissor_sets;
    scissor_compared += old_hud.active;
    float old_rscale[4] = {vp_sx, vp_sy, vp_ox, vp_oy}, new_rscale[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    world_hud_rscale_before(old_rscale);
    world_hud_rscale_now(new_rscale);
    hook_disagreements += memcmp(old_rscale, new_rscale, sizeof old_rscale) != 0;
    rscale_compared += old_hud.active;
    static float old_bound[0x10000 / 4], new_bound[0x10000 / 4];
    uint64_t old_size = 0, new_size = 0;
    const bool old_patched = world_hud_patch_before((uint64_t)(uintptr_t)patch_source, patch_size, old_bound, &old_size);
    const bool new_patched = world_hud_patch_now((uint64_t)(uintptr_t)patch_source, patch_size, new_bound, &new_size);
    patch_disagreements += old_patched != new_patched || old_size != new_size ||
                           (old_patched && memcmp(old_bound, new_bound, old_size));
    patch_compared += old_patched;
}

int main(void) {
    const float flat_fov[4] = {-0.7853982f, 0.7853982f, 0.7853982f, -0.7853982f};  // (about 45 degrees each way)
    const double no_turn[4] = {0, 0, 0, 1};
    const float calibration_origin[3] = {0, 1.6f, 0};

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
    present_count = vr_aim_result.frame = 500;
    focus_distance = 10;
    world_hud_off = 0;
    keep_image_up = 0;
    test_turn = 0;
    test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL}, &configuration);  // (the play default)
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
            const double anchor_x = viewport[0] + 500, anchor_y = 500;
            const double moved_x = move->offset[0] + move->linear[0] * anchor_x + move->linear[1] * anchor_y;
            const double moved_y = move->offset[1] + move->linear[2] * anchor_x + move->linear[3] * anchor_y;
            CHECK(fabs(moved_x - expected_x) < 1e-3 && fabs(moved_y - 500) < 1e-3);
            CHECK(fabs(moved_x - (viewport[0] + 500) - (eye ? -1.6 : 1.6)) < 1e-3);
            // (on clip coordinates: the half's centre, (0, 0, w 1), moves by that parallax over the FOV's half-width)
            CHECK(fabs(move->clip[2] - parallax / ((right_tangent - left_tangent) / 2)) < 1e-6 &&
                  fabs(move->clip[5]) < 1e-6);
            // (the HUD panel's size: one guest pixel of the panel at the eye's FOV, Link 10 m away as the point)
            const double panel_scale = tan_per_pixel * 1000 / (right_tangent - left_tangent);
            CHECK(fabs(move->flat_scale[0] / panel_scale - 1) < 1e-4 && fabs(move->flat_scale[1] / panel_scale - 1) < 1e-4);
            // (level: upright, not turned)
            CHECK(move->linear[1] == 0 && move->linear[2] == 0 && fabs(new_log.values[15]) < 1e-9);
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
        // (and a pane too large to be a marker's, or the flat piece, is not one: nothing counted)
        pane_block(pane_constants_block, 1000, 1, 0, 0, 0, 450, 40, -20, 20);
        CHECK(!world_hud_draw_now() && vr_world_hud.exact == 3);
        pane_block(pane_constants_block, 1000, 1, 0, 0, 0, 40, 40, -20, 20);
        vr_draw_marker = PC_VR_MARKER_FLAT;
        CHECK(!world_hud_draw_now() && vr_world_hud.exact == 3);
        vr_draw_marker = 7;
        CHECK(world_hud_draw_now() && vr_world_hud.exact == 4 && vr_world_hud.draws == 1 && vr_world_hud.frame_draws == 1);
    }

    // ---- By hand: the fade, in the left half with a Labo offset of 0.13: 1.3 times about the half's centre, which
    // on clip coordinates (the offset added after them) is x' = 1.3 x + 0.3 x 0.13 w, y' = 1.3 y. A pane that does
    // not cover the layer is no fade.
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

    // ---- By hand: the switches, from the configuration (runtime/pc_vr_config.c parses the fixture's settings), as
    // exec.c reads them. The reticle straight ahead in the left half, as above. SAVAGEWIND_PC_VR_WORLD_HUD: exactly "0"
    // turns the world-tied HUD off (nothing placed, nothing counted); unset, "1", "00", "0 " or empty leave it on.
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
        present_count = vr_aim_result.frame = 500;
        vr_draw_marker = 0;
        vr_draw_role = PC_VR_PANE_RETICLE;
        keep_image_up = 0;
        test_turn = 0;
        const struct { const char *value; bool off; } switches[] = {
            {NULL, false}, {"0", true}, {"1", false}, {"00", false}, {"0 ", false}, {"", false}};
        for (unsigned i = 0; i < sizeof switches / sizeof switches[0]; ++i) {
            test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1",
                                                   "SAVAGEWIND_PC_VR_WORLD_HUD", switches[i].value, NULL}, &configuration);
            memset(&vr_world_hud, 0, sizeof vr_world_hud);
            const bool placed = world_hud_draw_now();
            CHECK(configuration.play.world_hud == !switches[i].off && placed == !switches[i].off);
            CHECK(vr_world_hud.draws == !switches[i].off && vr_world_hud.unmatched == 0 && vr_world_hud.behind == 0);
        }
        test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL}, &configuration);
        // SAVAGEWIND_PC_VR_WORLD_HUD_ROLL: unset, the marker turns with the world's up as the eye sees it (a head
        // rolled 0.3 rad: the marker's roll in the image is 0.3 rad, 17.19 degrees); "0": upright in the image; another
        // number: that many degrees clockwise more (with the head level, the roll is that number); not a number, or
        // empty: 0, no test turn.
        double rolled[4];
        quaternion_about_axis(0, 0, 1, 0.3, rolled);
        const struct { const char *value; bool rolled_head; int keep; double turn_degrees, roll_degrees; } rolls[] = {
            {NULL, true, 0, 0, 0.3 * 180 / 3.14159265358979323846},
            {"0", true, 1, 0, 0},
            {"90", false, 0, 90, 90},
            {"-45", false, 0, -45, -45},
            {"abc", false, 0, 0, 0},
            {"", true, 0, 0, 0.3 * 180 / 3.14159265358979323846},
        };
        for (unsigned i = 0; i < sizeof rolls / sizeof rolls[0]; ++i) {
            test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1",
                                                   "SAVAGEWIND_PC_VR_WORLD_HUD_ROLL", rolls[i].value, NULL}, &configuration);
            set_eyes(no_turn, calibration_origin, rolls[i].rolled_head ? rolled : no_turn, 0.064, flat_fov, 10, 1);
            memset(&vr_world_hud, 0, sizeof vr_world_hud);
            CHECK(world_hud_draw_now());
            CHECK(configuration.play.world_hud_upright == rolls[i].keep &&
                  fabs(configuration.play.world_hud_test_turn - rolls[i].turn_degrees * 3.14159265358979323846 / 180) < 1e-12);
            CHECK(fabs(new_log.values[15] - rolls[i].roll_degrees) < 0.01);
        }
        test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL}, &configuration);
        set_eyes(no_turn, calibration_origin, no_turn, 0.064, flat_fov, 10, 1);
        keep_image_up = 0;
        test_turn = 0;

        // ---- By hand: a stale move with every field set (an earlier draw's). A refused draw (behind the UI camera)
        // leaves all of it, bit for bit, and the hooks ignore it while the draw is not moved; a placed draw replaces
        // every field.
        VrHudMove stale;
        random_move(&stale);
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
        bool every_field_replaced = placed->half_left != stale.half_left && placed->half_right != stale.half_right;
        for (unsigned i = 0; i < 4; ++i) every_field_replaced &= placed->linear[i] != stale.linear[i];
        for (unsigned i = 0; i < 6; ++i) every_field_replaced &= placed->clip[i] != stale.clip[i];
        for (unsigned i = 0; i < 2; ++i)
            every_field_replaced &= placed->offset[i] != stale.offset[i] && placed->flat_scale[i] != stale.flat_scale[i] &&
                                    placed->flat_offset[i] != stale.flat_offset[i];
        CHECK(every_field_replaced);
    }

    // ---- Against the code as it was, on random draws: the same answer, move, counters and log values for the
    // router's question, then the same scissor, fragment scale and patched constants, with the router's answer and
    // with the move forced on.
    for (unsigned i = 0; i < sizeof patch_source / sizeof patch_source[0]; ++i)
        patch_source[i] = (float)random_between(-1000, 1000);
    unsigned draws = 0, state_disagreements = 0, log_disagreements = 0, fade_disagreements = 0;
    unsigned placed_by_pane = 0, placed_reticles = 0, placed_fades = 0, placed_eyes[2] = {0}, placed_upright = 0,
             placed_turned = 0, placed_test_turn = 0, behind = 0, unmatched = 0, scissors_moved = 0;
    memset(&old_hud, 0, sizeof old_hud);
    memset(&vr_world_hud, 0, sizeof vr_world_hud);
    for (unsigned trial = 0; trial < 1000000; ++trial) {
        random_draw();
        // (both start from the same earlier move and counters; now and then a stale move with every field set, which
        // a refused draw must leave as it is)
        const bool stale_this_draw = random_chance(0.05);
        VrHudMove stale;
        if (stale_this_draw) {
            random_move(&stale);
            set_old_move(&stale);
            vr_world_hud.move = stale;
            ++stale_moves;
        }
        ++draws;
        const uint64_t behind_before = old_hud.behind, unmatched_before = old_hud.unmatched;
        memset(&old_log, 0, sizeof old_log);
        memset(&new_log, 0, sizeof new_log);
        old_fade_eye = new_fade_eye = -1;
        const bool old_fade = hud_fade_before();
        old_hud.active = old_fade || world_hud_draw_before();
        const bool new_fade = hud_fade_now();
        vr_world_hud.active = new_fade || world_hud_draw_now();
        state_disagreements += !same_state();
        // (a refused draw left the stale move as it was, every field, in both)
        stale_moves_kept += stale_this_draw && !old_hud.active && same_state() &&
                            !memcmp(&vr_world_hud.move, &stale, sizeof stale);
        fade_disagreements += old_fade != new_fade || old_fade_eye != new_fade_eye;
        log_disagreements += !same_log();
        behind += old_hud.behind != behind_before;
        unmatched += old_hud.unmatched != unmatched_before;
        if (old_hud.active && !old_fade) {
            placed_by_pane += old_log.how && !strcmp(old_log.how, "pane");
            placed_reticles += old_log.how && !strcmp(old_log.how, "reticle");
            ++placed_eyes[old_log.eye];
            placed_upright += keep_image_up != 0;
            placed_turned += !keep_image_up && test_turn == 0 && old_log.values[15] != 0;
            placed_test_turn += !keep_image_up && test_turn != 0;
        }
        placed_fades += old_fade;
        if (!same_state()) {  // (the hooks need the same move to compare)
            memset(&old_hud, 0, sizeof old_hud);
            memset(&vr_world_hud, 0, sizeof vr_world_hud);
            continue;
        }
        const Rect sc = random_chance(0.95) ? (Rect){{(int32_t)random_between(0, 100), (int32_t)random_between(0, 100)},
                                                     {(uint32_t)random_between(1, 3000), (uint32_t)random_between(1, 3000)}}
                                            : (Rect){{0, 0}, {0, (uint32_t)random_between(0, 2)}};
        const uint64_t patch_size = random_chance(0.01) ? (uint64_t)random_between(0x10000, sizeof patch_source)
                                                        : (uint64_t)random_between(0, 300);
        for (unsigned i = 0; i < 16; ++i) patch_source[i] = (float)random_between(-3, 3);
        compare_hooks(sc, patch_size);
        if (old_hud.active && dc.sc.extent.width && (st.scissor[0][2] < 16384 || st.scissor[0][3] < 16384) &&
            (st.scissor[0][2] < st.viewport[0][2] - 0.5 || st.scissor[0][3] < st.viewport[0][3] - 0.5))
            ++scissors_moved;
        // (the hooks with the move forced on: the router's answer aside, the same move gives the same results)
        const bool was_active = old_hud.active;
        old_hud.active = vr_world_hud.active = true;
        if (st.rt.color[0]) compare_hooks(sc, patch_size);
        old_hud.active = vr_world_hud.active = was_active;
    }
    CHECK(state_disagreements == 0 && fade_disagreements == 0 && log_disagreements == 0);
    CHECK(hook_disagreements == 0 && patch_disagreements == 0);
    CHECK(stale_moves > 40000 && stale_moves_kept > 20000);
    // (the random draws reach every way through: placed by pane, as reticles and as fades, in both eyes, upright,
    // turned and with the test turn; refused behind and unmatched; hooks on moved draws with moved scissors)
    CHECK(placed_by_pane > 50000 && placed_reticles > 30000 && placed_fades > 20000);
    CHECK(placed_eyes[0] > 30000 && placed_eyes[1] > 30000);
    CHECK(placed_upright > 10000 && placed_turned > 10000 && placed_test_turn > 5000);
    CHECK(behind > 10000 && unmatched > 10000 && scissors_moved > 10000);
    CHECK(scissor_compared > 100000 && rscale_compared > 100000 && patch_compared > 100000);
    printf("vr_world_hud random draws: %u, disagreements state %u, fade %u, log %u, hooks %u, patch %u; stale moves %u, "
           "kept whole by a refused draw %u\n", draws, state_disagreements, fade_disagreements, log_disagreements,
           hook_disagreements, patch_disagreements, stale_moves, stale_moves_kept);
    printf("vr_world_hud placed: by pane %u, reticles %u, fades %u; eyes %u, %u; upright %u, turned %u, test turn %u; "
           "behind %u, unmatched %u; moved scissors %u; hooks compared: scissor %u, rscale %u, patch %u\n",
           placed_by_pane, placed_reticles, placed_fades, placed_eyes[0], placed_eyes[1], placed_upright, placed_turned,
           placed_test_turn, behind, unmatched, scissors_moved, scissor_compared, rscale_compared, patch_compared);

    printf("vr_world_hud fixture: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
