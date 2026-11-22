// PC VR renderer: the world-tied HUD's placement (D073, D088). Header-only: exec.c includes it, and its per-draw hooks
// stay in exec.c's translation unit (the play build has no link-time optimisation).
//
// The model. The game ties some HUD pieces to points in the world: enemy marks and health bars, the Z-target cursor,
// item names, the stamina wheel (placed by the game's HUD helper, which runtime/pc_vr_hud.c reports as a marker: its
// layout position and its point in the UI camera's space) and the speech balloons. A HUD draw whose pane, or a parent,
// is bound to such a report (D088: the draw's marker link, exec.c vr_draw_marker) is drawn into its own eye's half of
// the final image instead of on the HUD's panel, moved so that the marker's point lands where that eye sees it, from
// the pose and FOV the eye was rendered with (its render certificate) through the D053 calibration and world scale.
// Each half goes to its own eye, so the piece is seen at its point's depth. An aiming reticle's pane (the bow's, a
// rune's, a thrown weapon's: the pane's role) is placed the same way on the aim ray, at the depth the scene has there
// (vr_aim.c's vr_aim_result). Each marker or reticle's panes scale together about its point, at the HUD panel's
// angular size times a factor that falls with distance. They form one card facing the point between the eyes.
// The fade over the whole
// layout (the black that clears after a load) is drawn over each eye's whole view the same way, enlarged about its
// half's centre and not moved.
//
// Spaces and units:
//  - Guest pixels: the game's final image, both halves side by side, from its top-left corner, y down. A HUD draw's
//    viewport (x, y, width, height) is its half.
//  - The HUD layer: nn::ui2d draws layout x (1280 wide, y up, 0 at the centre) at x / 640 of its half's NDC and y at
//    y / 360, with its 40-degree projection (the vertex constants c3: the matrix, then the pane's view rows); the
//    vertex shader then adds the half's Labo offset to x (the first word of the constants c13: the lens centre plus the
//    HUD's convergence, measured 0.13125 to 0.15417). Labo's HUD layer scales the whole layout 0.92 by 0.46 about its
//    centre after the helper placed a marker (VR_HUD_LAYER_X/Y).
//  - The UI camera's space: game units, x right, y up, -z ahead (a marker's camera point). The helper's horizontal
//    offset beside the point is NDC of the UI camera's view (fovy 50 degrees, 16:9), kept as an angle at the point's
//    depth (VR_UI_TAN_X). The certificate's units_per_meter turns game units into metres; the calibration's origin and
//    heading put the UI camera in LOCAL (vr_math.h has OpenXR's conventions). An eye's image: 0..1 across its FOV.
//
// The move (VrHudMove): the card's plane maps the game's HUD coordinates to each eye's clip space, including depth
// across the card for perspective. The fade and the diagnostic image turns use a flat move. exec.c applies it in
// three places of the draw's recording: the vertex constants c3, whose first matrix gives the clip position, get the
// move on clip coordinates (vr_hud_move_patch, on a copy exec.c binds), so the GPU clips the moved draw against the
// eye's half; the scissor becomes the half, or the game's own smaller scissor moved with the draw
// (vr_hud_move_scissor); and the scale and offset by which shaders turn the fragment's position back into a guest
// position use the scale at the anchor (vr_hud_move_rscale).
//
// The hub and the geometry. exec.c reads what only the executor knows and passes it in as values: the draw's tags (its
// marker link, its pane's role), its constants blocks (st.ubuf: c3 as a pointer to its floats, the Labo offset from
// c13), the viewport, the scissor, the target's size, the guest-to-device mapping and the pass's size, the eye
// certificates (after the certified-eye test, before any use of a certificate), the marker report
// (pc_vr_hud_point_by_seq) or the aim result, the focus distance (pc_vr_camera_focus_distance) and the environment
// switches. It keeps the counters and logs, the 64 KB block the patched constants are copied into and bound
// (ubo_span, vk_transient_copy), and setting the scissor (rec_CmdSetScissor). The functions here take plain values and
// return values: no executor state, no Vulkan, nothing read but what they are given, so tools/test_vr_world_hud.c
// checks them, and against the code they came from (exec.c at ca3cd86), without the renderer.
//
// Rules by shape that remain (D088 left them; replacing one wants the game's own word for it): nn::ui2d's blocks are
// told by their projection's constants (vr_hud_marker_block); a draw into one half is told by its viewport's width,
// and its eye by the viewport's centre (vr_hud_one_half, vr_hud_eye_of_half): the half the game itself draws that
// view into. HUD draws carry no render certificate to name the eye instead (they are recorded outside any eye layer's
// render scope: none of 16,384 placed draws had one, simulator run of 6cb77d7); a tag would need a game hook where the
// UI's per-view draw begins. The fade is known by its pane's role alone, and a marker's panes by the binding or the
// role, not by their size (backlog 9).
//
// Open, from the records, and not addressed here: the HUD's depth through the camera's zoom and the world scale (the
// UI camera's FOV is taken as fixed, VR_UI_TAN_X, and a marker's size follows Link's distance), and the depth of
// dialogue, whose boxes stay on the HUD's panel.
#ifndef SAVAGEWIND_VR_WORLD_HUD_H
#define SAVAGEWIND_VR_WORLD_HUD_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "../../hle/pc_vr_render_trace.h"
#include "../../pc_vr_hud.h"
#include "vr_math.h"

#define VR_WORLD_HUD_SIZE_MIN 0.4      // a marker's size against its size at Link's distance: at least
#define VR_WORLD_HUD_SIZE_MAX 1.5      // and at most
#define VR_AIM_SIZE_MIN 0.6            // the crosshair's least size (it must stay big enough to aim with)
#define VR_WORLD_HUD_PANE_LARGE 400.0f // layout units: a placed pane larger is logged (the size cap before backlog 9)
#define VR_UI_TAN_X 0.82904            // the UI camera's horizontal half-FOV as a tangent: tan(25 degrees) * 16 / 9
#define VR_HUD_LAYER_X 0.92f           // Labo's HUD layer: the layout's scale (square in the half's pixels)
#define VR_HUD_LAYER_Y 0.46f
#define VR_FADE_GROW 1.3f              // the fade's enlargement about its half's centre (so the Labo offset leaves no strip)

// The move of one HUD draw, in guest pixels of the final image.
typedef struct {
    float linear[4], offset[2];  // pixel mapping, tangent to the card at its anchor for a perspective draw
    // The same move on the vertex stage's clip coordinates: x' = clip[0] x + clip[1] y + clip[2] w,
    // y' = clip[3] x + clip[4] y + clip[5] w.
    float clip[6];
    float clip_w[3];             // card depth from the original clip x, y and w
    bool card;
    double post_offset;          // the shader's horizontal NDC offset, for the card's scissor
    // The move without its turn, as a scale and an offset per axis, for shaders that read the fragment's position.
    float flat_scale[2], flat_offset[2];
    float half_left, half_right;  // the eye's half of the final image, which the scissor stays inside
} VrHudMove;

// The world-tied HUD's state, one for the process. Defined in exec.c (vr_world_hud), the translation unit of the
// per-draw hooks that read it, so those reads stay direct loads (vr_eyes.h says why); serialised by gpu_lock, as all
// draw recording is.
//  - active: whether the draw being recorded is moved. Set at the draw's routing (vr_draw_route.h): false by exec.c
//    draw_ for a draw that is not a HUD draw (vr_is_hud_draw), and by exec.c vr_route_hud_draw for a HUD draw that
//    reaches the placements, true when vr_hud_fade or vr_world_hud_draw placed it. A HUD draw left out before the
//    placements (over the held scene, or by the guard) and a draw draw_ returns from before its routing leave it as
//    it was; none of them reaches the hooks. Read by the draw's hooks (vr_world_hud_patch, _scissor, _rscale), which
//    all run after the routing in draw_, and by draw_ itself. Known property: nothing else clears it, not a frame's
//    end, a pass or a game reset; it stays as the last routed draw left it until the next.
//  - move: written only by a placement that succeeds (vr_hud_move_place, through vr_hud_marker_move or
//    vr_hud_fade_move), just before active is set true; a refused draw leaves the last one there, unused.
//  - frame_draws: markers placed since the last present; reset at each game present (exec.c vk_vr_present) and at a
//    game reset (vk_exec_reset); the "shot frame" log line of an eye capture reads it (exec.c vr_capture_eyes).
//  - draws, exact (linked by pane), unmatched (no report and no fresh aim), behind (behind the UI camera or the eye),
//    logged: for the process, never reset; for the logs only.
typedef struct {
    bool active;
    VrHudMove move;
    uint64_t draws, unmatched, behind, logged, frame_draws, exact;
} VrWorldHud;

// Sets up the move for a draw into the half with this viewport (guest pixels): scaled by scale about anchor, turned so
// that the draw's up becomes the unit direction up (y down the image: 0, -1 leaves it upright), and moved so anchor
// lands at eye. labo_offset is what the vertex shader adds to x after the projection, in the half's NDC.
static inline void vr_hud_move_place(VrHudMove *move, const float *viewport, double labo_offset, const double anchor[2],
                                     const double eye[2], const double scale[2], const double up[2]) {
    move->card = false;
    move->post_offset = labo_offset;
    move->clip_w[0] = move->clip_w[1] = 0;
    move->clip_w[2] = 1;
    // In pixels: the draw's right (1, 0) goes to (-up_y, up_x) and its down (0, 1) to (-up_x, -up_y), each scaled.
    const double linear[4] = {-up[1] * scale[0], -up[0] * scale[1], up[0] * scale[0], -up[1] * scale[1]};
    const double offset[2] = {eye[0] - (linear[0] * anchor[0] + linear[1] * anchor[1]),
                              eye[1] - (linear[2] * anchor[0] + linear[3] * anchor[1])};
    for (unsigned i = 0; i < 4; ++i) move->linear[i] = (float)linear[i];
    move->offset[0] = (float)offset[0];
    move->offset[1] = (float)offset[1];
    // On NDC: a pixel is centre + (half_w x, -half_h y), so the linear part is conjugated by that and the offset
    // follows. The shader adds labo_offset to x after the matrix: the move applies to the position with it.
    const double half_w = viewport[2] * 0.5, half_h = viewport[3] * 0.5;
    const double centre[2] = {viewport[0] + half_w, viewport[1] + half_h};
    const double ndc[4] = {linear[0], linear[1] * -half_h / half_w, linear[2] * half_w / -half_h, linear[3]};
    const double shift[2] = {(linear[0] * centre[0] + linear[1] * centre[1] + offset[0] - centre[0]) / half_w,
                             (linear[2] * centre[0] + linear[3] * centre[1] + offset[1] - centre[1]) / -half_h};
    move->clip[0] = (float)ndc[0];
    move->clip[1] = (float)ndc[1];
    move->clip[2] = (float)(ndc[0] * labo_offset + shift[0] - labo_offset);
    move->clip[3] = (float)ndc[2];
    move->clip[4] = (float)ndc[3];
    move->clip[5] = (float)(ndc[2] * labo_offset + shift[1]);
    move->flat_scale[0] = (float)scale[0];
    move->flat_scale[1] = (float)scale[1];
    move->flat_offset[0] = (float)(eye[0] - scale[0] * anchor[0]);
    move->flat_offset[1] = (float)(eye[1] - scale[1] * anchor[1]);
    move->half_left = viewport[0];
    move->half_right = viewport[0] + viewport[2];
}

// The move on a copy of a moved draw's vertex constants c3 (nn::ui2d's block, at least 64 bytes: its first matrix
// gives the clip position). A card also supplies perspective w to the GPU for clipping and texture interpolation.
static inline void vr_hud_move_patch(const VrHudMove *move, float *block) {
    const float *clip = move->clip;
    for (unsigned column = 0; column < 4; ++column) {
        const float row_x = block[column], row_y = block[4 + column], row_w = block[12 + column];
        block[column] = clip[0] * row_x + clip[1] * row_y + clip[2] * row_w;
        block[4 + column] = clip[3] * row_x + clip[4] * row_y + clip[5] * row_w;
        if (move->card) {
            const float weight = move->clip_w[0] * row_x + move->clip_w[1] * row_y + move->clip_w[2] * row_w;
            block[12 + column] = weight;
            // UI draws have no depth test. Keep their vertices inside both possible clip-depth ranges.
            block[8 + column] = 0.5f * weight;
        }
    }
}

static inline void vr_hud_include_corner(const float *viewport, double post_offset, const double clip[3],
                                         double bounds[4]) {
    const double pixel_x = viewport[0] + (1 + clip[0] / clip[2] + post_offset) * 0.5 * viewport[2];
    const double pixel_y = viewport[1] + (1 - clip[1] / clip[2]) * 0.5 * viewport[3];
    bounds[0] = fmin(bounds[0], pixel_x);
    bounds[1] = fmin(bounds[1], pixel_y);
    bounds[2] = fmax(bounds[2], pixel_x);
    bounds[3] = fmax(bounds[3], pixel_y);
}

// The card's smaller game scissor, in order around its perimeter. Clip its edges to the eye plane before dividing
// by w, so a corner behind the eye cannot fold across and cut off the visible part.
static inline void vr_hud_card_scissor(const VrHudMove *move, const float *viewport, double left, double top,
                                       double right, double bottom, double bounds[4]) {
    double corners[4][3];
    for (unsigned corner = 0; corner < 4; ++corner) {
        const double pixel_x = corner == 1 || corner == 2 ? right : left;
        const double pixel_y = corner >= 2 ? bottom : top;
        const double ndc_x = 2 * (pixel_x - viewport[0]) / viewport[2] - 1 - move->post_offset;
        const double ndc_y = 1 - 2 * (pixel_y - viewport[1]) / viewport[3];
        corners[corner][0] = move->clip[0] * ndc_x + move->clip[1] * ndc_y + move->clip[2];
        corners[corner][1] = move->clip[3] * ndc_x + move->clip[4] * ndc_y + move->clip[5];
        corners[corner][2] = move->clip_w[0] * ndc_x + move->clip_w[1] * ndc_y + move->clip_w[2];
    }
    // w is 1 at the marker's anchor. Project each visible endpoint and each crossing once.
    const double min_weight = 1e-9;
    for (unsigned corner = 0; corner < 4; ++corner) {
        const double *start = corners[corner], *end = corners[(corner + 1) % 4];
        const bool start_ahead = start[2] >= min_weight, end_ahead = end[2] >= min_weight;
        if (start_ahead) vr_hud_include_corner(viewport, move->post_offset, start, bounds);
        if (start_ahead != end_ahead) {
            const double along = (min_weight - start[2]) / (end[2] - start[2]);
            const double crossing[3] = {start[0] + along * (end[0] - start[0]),
                                        start[1] + along * (end[1] - start[1]), min_weight};
            vr_hud_include_corner(viewport, move->post_offset, crossing, bounds);
        }
    }
}

// The scissor of a moved draw, in the pass's device pixels: the eye's half (the whole target's height), or, when the
// game's own scissor is smaller than the draw's viewport (a clip of the pane's contents), the box around that
// scissor's moved corners, kept inside the half. viewport: the draw's (guest pixels); game_scissor: x, y, width,
// height (16384 or more wide or tall: none in that direction); target_height: the final image's (guest pixels);
// device_scale and device_offset: the guest-to-device mapping (exec.c vp_sx, vp_sy, vp_ox, vp_oy); pass_width and
// pass_height: the pass's. Writes left, top, right and bottom, clamped to the pass, right not left of left and bottom
// not above top.
static inline void vr_hud_move_scissor(const VrHudMove *move, const float *viewport, const int32_t *game_scissor,
                                       uint32_t target_height, float device_scale_x, float device_scale_y,
                                       float device_offset_x, float device_offset_y, uint32_t pass_width,
                                       uint32_t pass_height, int pixels[4]) {
    // In guest pixels.
    double left = move->half_left, right = move->half_right, top = 0, bottom = target_height;
    const double game_left = game_scissor[2] >= 16384 ? viewport[0] : game_scissor[0];
    const double game_right = game_scissor[2] >= 16384 ? viewport[0] + viewport[2] : game_left + game_scissor[2];
    const double game_top = game_scissor[3] >= 16384 ? viewport[1] : game_scissor[1];
    const double game_bottom = game_scissor[3] >= 16384 ? viewport[1] + viewport[3] : game_top + game_scissor[3];
    if (game_left > viewport[0] + 0.5 || game_right < viewport[0] + viewport[2] - 0.5 || game_top > viewport[1] + 0.5 ||
        game_bottom < viewport[1] + viewport[3] - 0.5) {
        double bounds[4] = {1e30, 1e30, -1e30, -1e30};
        if (move->card) {
            vr_hud_card_scissor(move, viewport, game_left, game_top, game_right, game_bottom, bounds);
            // A scissor entirely behind the eye or off its near edge is empty at that edge.
            bounds[0] = fmin(right, bounds[0]);
            bounds[1] = fmin(bottom, bounds[1]);
            bounds[2] = fmax(left, bounds[2]);
            bounds[3] = fmax(top, bounds[3]);
        } else for (unsigned corner = 0; corner < 4; ++corner) {
            const double corner_x = corner & 1 ? game_right : game_left, corner_y = corner & 2 ? game_bottom : game_top;
            const double moved_x = move->offset[0] + move->linear[0] * corner_x + move->linear[1] * corner_y;
            const double moved_y = move->offset[1] + move->linear[2] * corner_x + move->linear[3] * corner_y;
            bounds[0] = fmin(bounds[0], moved_x);
            bounds[1] = fmin(bounds[1], moved_y);
            bounds[2] = fmax(bounds[2], moved_x);
            bounds[3] = fmax(bounds[3], moved_y);
        }
        left = fmax(left, bounds[0]);
        top = fmax(top, bounds[1]);
        right = fmin(right, bounds[2]);
        bottom = fmin(bottom, bounds[3]);
    }
    // In the pass's device pixels, clamped to the pass.
    int left_px = (int)floor(left * device_scale_x + device_offset_x);
    int right_px = (int)ceil(right * device_scale_x + device_offset_x);
    int top_px = (int)floor(top * device_scale_y + device_offset_y);
    int bottom_px = (int)ceil(bottom * device_scale_y + device_offset_y);
    if (left_px < 0) left_px = 0;
    if (top_px < 0) top_px = 0;
    if (right_px > (int)pass_width) right_px = (int)pass_width;
    if (bottom_px > (int)pass_height) bottom_px = (int)pass_height;
    if (right_px < left_px) right_px = left_px;
    if (bottom_px < top_px) bottom_px = top_px;
    pixels[0] = left_px;
    pixels[1] = top_px;
    pixels[2] = right_px;
    pixels[3] = bottom_px;
}

// For shaders that read the fragment's position: they turn it back into a guest position as (position - o) / s with
// rscale = {s_x, s_y, o_x, o_y}. Folding the move into rscale makes them see the position the game drew at (without
// the move's turn or perspective, which a scale and an offset cannot hold).
static inline void vr_hud_move_rscale(const VrHudMove *move, float rscale[4]) {
    rscale[2] += rscale[0] * move->flat_offset[0];
    rscale[3] += rscale[1] * move->flat_offset[1];
    rscale[0] *= move->flat_scale[0];
    rscale[1] *= move->flat_scale[1];
}

// Whether a HUD draw's viewport (guest pixels) covers one half of the final image: as wide as half the target, within
// a pixel (half_width: the target's width times 0.5), and not empty.
static inline bool vr_hud_one_half(const float *viewport, float half_width) {
    return !(fabsf(viewport[2] - half_width) > 1 || !(viewport[3] > 0));
}

// Which eye's half that viewport is, by its centre: 0 the left, 1 the right.
static inline unsigned vr_hud_eye_of_half(const float *viewport, float half_width) {
    return viewport[0] + viewport[2] * 0.5f >= half_width;
}

// Whether a HUD draw's vertex constants c3 (block, block_size bytes, at least 64) are nn::ui2d's for a piece that can
// be a marker's: a pane of the HUD's 40-degree 16:9 layer (its projection's constants, then the pane's view rows and
// its rectangle), or a text's 16:9 matrix (the projection and its view in one, flat, maybe turned: the arrow count).
// (A full-screen pass that tests Link's depth for his silhouette also has a matrix there.) Writes origin, where the
// pane's (or the text's) origin is in the layer's NDC before the Labo offset, and pane_size, the pane's larger side in
// layout units (0: a text), both for the log, when the block is one of them; a pane whose origin is not ahead is none.
// Which draws are a marker's is the binding's word (vr_draw_marker) or the pane's role, not the pane's size.
static inline bool vr_hud_marker_block(const float *block, uint64_t block_size, float origin[2], float *pane_size) {
    const bool flat = block[0] > 0 && fabsf(fabsf(block[5]) / block[0] - 16.0f / 9.0f) < 0.002f;
    float size = 0;  // the pane's larger side in layout units (0: a text)
    if (flat && block[5] > 0 && fabsf(block[5] - 2.74748f) < 0.001f && block[14] == -1 && block[15] == 0 &&
        !block[1] && !block[2] && !block[3] && !block[4] && !block[6] && !block[7] && !block[8] && !block[9] &&
        !block[12] && !block[13] && block_size >= 34 * 16) {
        const float *model_view = block + 16, origin_depth = -model_view[11];
        if (!(origin_depth > 0)) return false;
        origin[0] = block[0] * model_view[3] / origin_depth;
        origin[1] = block[5] * model_view[7] / origin_depth;
        const float *rect = block + 33 * 4;  // (the pane's rectangle: width, height, left, top)
        size = fmaxf(fabsf(rect[0]) * hypotf(model_view[0], model_view[4]),
                     fabsf(rect[1]) * hypotf(model_view[1], model_view[5]));
    } else if (block_size == 128 && block[14] == -1 && block[15] > 0 && !block[2] && !block[6] && !block[8] &&
               !block[9] && !block[12] && !block[13]) {
        origin[0] = block[3] / block[15];
        origin[1] = block[7] / block[15];
    } else return false;
    *pane_size = size;
    return true;
}

// A marker report as its panes are drawn: Labo's HUD layer scales the whole layout about its centre (VR_HUD_LAYER_X/Y,
// in every unscaled pane's matrix) after the helper placed the marker.
static inline void vr_hud_marker_on_layer(PcVrHudPoint *marker) {
    marker->layout[0] *= VR_HUD_LAYER_X;
    marker->layout[1] *= VR_HUD_LAYER_Y;
}

// A reticle's point, from the aim result: on the aim ray (the UI camera's forward), aim_distance game units out, about
// the layout's centre. Its frame of 0 marks it as the reticle (its least size is VR_AIM_SIZE_MIN).
static inline PcVrHudPoint vr_hud_reticle_point(double aim_distance) {
    return (PcVrHudPoint){.camera = {0, 0, -(float)aim_distance}};
}

// Where an eye sees a marker's point (vr_hud_marker_view).
typedef enum {
    VR_HUD_SEEN,    // ahead of the eye: the view below holds where
    VR_HUD_BEHIND,  // not ahead of the UI camera (1e-3 game units), or not ahead of the eye (1e-4 m): counted as behind
    VR_HUD_NO_FOV,  // the eye's FOV has no width or no height
} VrHudSight;

typedef struct {
    double depth;            // the point's depth ahead of the UI camera, game units
    double to_eye[4];        // the eye's orientation inverted: LOCAL to its view
    double fov_tangents[4];  // the eye's FOV as tangents: left, right, up, down
    double from_eye[3];      // the point less the eye's position, LOCAL metres
    double from_midpoint[3]; // the same point from the midpoint of the rendered eyes, for the shared card
    double view[3];          // the point in the eye's view, metres
    double image[2];         // where the eye sees it, 0..1 across its image (vr_eye_image_position)
} VrHudMarkerView;

// The marker's point in an eye's view: in metres from the UI camera (with the helper's offset beside it, an angle at
// the point's depth), then in LOCAL (D053: the calibration's origin plus its heading turning that), then relative to
// the eye and in the eye's view. eye_certificate is the certificate of eye `eye`, which the caller has checked is
// certified this frame, with a world scale (units_per_meter > 0) and a calibration (epoch). seen holds the view only
// when VR_HUD_SEEN comes back.
static inline VrHudSight vr_hud_marker_view(const PcVrHudPoint *marker, const PcVrRenderCertificate *eye_certificate,
                                            unsigned eye, VrHudMarkerView *seen) {
    const XrPoseSnapshot *pose = &eye_certificate->native_snapshot;
    const double depth = -marker->camera[2];
    seen->depth = depth;
    if (!(depth > 1e-3)) return VR_HUD_BEHIND;
    const double units_per_meter = eye_certificate->units_per_meter;
    const double point[3] = {(marker->camera[0] + marker->offset * VR_UI_TAN_X * depth) / units_per_meter,
                             marker->camera[1] / units_per_meter, marker->camera[2] / units_per_meter};
    const double heading[4] = {pose->calibration.heading_xyzw[0], pose->calibration.heading_xyzw[1],
                               pose->calibration.heading_xyzw[2], pose->calibration.heading_xyzw[3]};
    const XrEyeSnapshot *eye_pose = &pose->eyes[eye];
    seen->to_eye[0] = -eye_pose->orientation_xyzw[0];
    seen->to_eye[1] = -eye_pose->orientation_xyzw[1];
    seen->to_eye[2] = -eye_pose->orientation_xyzw[2];
    seen->to_eye[3] = eye_pose->orientation_xyzw[3];
    const double left = tan(eye_pose->fov_left_right_up_down[0]), right = tan(eye_pose->fov_left_right_up_down[1]);
    const double up = tan(eye_pose->fov_left_right_up_down[2]), down = tan(eye_pose->fov_left_right_up_down[3]);
    seen->fov_tangents[0] = left;
    seen->fov_tangents[1] = right;
    seen->fov_tangents[2] = up;
    seen->fov_tangents[3] = down;
    double local[3];
    vr_rotate(heading, point, local);
    for (unsigned i = 0; i < 3; ++i) {
        const double position = pose->calibration.origin_xyz[i] + local[i];
        seen->from_eye[i] = position - eye_pose->position_xyz[i];
        seen->from_midpoint[i] = position - 0.5 * ((double)pose->eyes[0].position_xyz[i] + pose->eyes[1].position_xyz[i]);
    }
    if (!vr_eye_image_position(seen->from_eye, eye_pose->orientation_xyzw, seen->fov_tangents, 1e-4, seen->view,
                               seen->image))
        return VR_HUD_BEHIND;
    if (!(right > left) || !(up > down)) return VR_HUD_NO_FOV;
    return VR_HUD_SEEN;
}

// How a marker's draw was placed (for the log).
typedef struct {
    double anchor[2];    // where the game drew the marker's point (guest pixels of the half)
    double eye_at[2];    // where the eye sees it
    double scale[2];     // the move's scale per axis
    double image_up[2];  // the world's up at the point as the eye's image shows it (y down; 0, -1: upright)
} VrHudMarkerPlacement;

// One plane for both eyes. Its normal points at the midpoint of the eyes and its vertical follows gravity projected
// onto the plane. Each original guest pixel offsets the point on this plane by metres_per_pixel before projection.
static inline void vr_hud_project_card(const VrHudMarkerView *seen, const float *viewport, double labo_offset,
                                       double distance, double metres_per_pixel, VrHudMove *move,
                                       VrHudMarkerPlacement *placed) {
    const double *direction = seen->from_midpoint;
    const double horizontal = hypot(direction[0], direction[2]);
    // At a pole any horizontal direction is tangent. Pick LOCAL right independently of the eye's rotation.
    const double card_right[3] = {horizontal > 1e-9 ? -direction[2] / horizontal : 1, 0,
                                  horizontal > 1e-9 ? direction[0] / horizontal : 0};
    const double card_up[3] = {-card_right[2] * direction[1] / distance, horizontal / distance,
                                card_right[0] * direction[1] / distance};
    double view_right[3], view_up[3];
    vr_rotate(seen->to_eye, card_right, view_right);
    vr_rotate(seen->to_eye, card_up, view_up);

    const double half_width = 0.5 * viewport[2], half_height = 0.5 * viewport[3];
    const double anchor_x = (placed->anchor[0] - viewport[0]) / half_width - 1 - labo_offset;
    const double anchor_y = 1 - (placed->anchor[1] - viewport[1]) / half_height;
    const double left = seen->fov_tangents[0], right = seen->fov_tangents[1];
    const double up = seen->fov_tangents[2], down = seen->fov_tangents[3];
    const double eye_depth = -seen->view[2];
    // Columns taking (old clip x, y, w) to an eye-space point on the card. Use the same depth unit in every
    // column so w equals 1 at the anchor. c13 adds labo_offset after projection.
    double columns[3][3];
    for (unsigned axis = 0; axis < 3; ++axis) {
        columns[0][axis] = metres_per_pixel * half_width * view_right[axis] / eye_depth;
        columns[1][axis] = metres_per_pixel * half_height * view_up[axis] / eye_depth;
        columns[2][axis] = seen->view[axis] / eye_depth - anchor_x * columns[0][axis] - anchor_y * columns[1][axis];
    }
    for (unsigned column = 0; column < 3; ++column) {
        const double weight = -columns[column][2];
        move->clip[column] = (float)((2 * columns[column][0] - (right + left) * weight) / (right - left)
                                     - labo_offset * weight);
        move->clip[3 + column] = (float)((2 * columns[column][1] - (up + down) * weight) / (up - down));
        move->clip_w[column] = (float)weight;
    }
    move->card = true;
    move->post_offset = labo_offset;
    move->half_left = viewport[0];
    move->half_right = viewport[0] + viewport[2];
    // Pixel derivatives at the anchor, for the log and shaders that reconstruct guest fragment coordinates from
    // a scale and offset. The GPU gets the full projective transform above.
    const double eye_x = 2 * seen->image[0] - 1 - labo_offset, eye_y = 1 - 2 * seen->image[1];
    move->linear[0] = (float)(move->clip[0] - eye_x * move->clip_w[0]);
    move->linear[1] = (float)(-(half_width / half_height) * (move->clip[1] - eye_x * move->clip_w[1]));
    move->linear[2] = (float)(-(half_height / half_width) * (move->clip[3] - eye_y * move->clip_w[0]));
    move->linear[3] = (float)(move->clip[4] - eye_y * move->clip_w[1]);
    placed->scale[0] = hypot(move->linear[0], move->linear[2]);
    placed->scale[1] = hypot(move->linear[1], move->linear[3]);
    placed->image_up[0] = placed->scale[1] > 0 ? -move->linear[1] / placed->scale[1] : 0;
    placed->image_up[1] = placed->scale[1] > 0 ? -move->linear[3] / placed->scale[1] : -1;
    for (unsigned axis = 0; axis < 2; ++axis) {
        move->offset[axis] = (float)(placed->eye_at[axis] - move->linear[2 * axis] * placed->anchor[0]
                                     - move->linear[2 * axis + 1] * placed->anchor[1]);
        move->flat_scale[axis] = (float)placed->scale[axis];
        move->flat_offset[axis] = (float)(placed->eye_at[axis] - placed->scale[axis] * placed->anchor[axis]);
    }
}

// The move of a marker's draw (seen: vr_hud_marker_view's VR_HUD_SEEN) into the half with this viewport. Where the
// game drew the marker's point: the layout position at x / 640 and y / 360 of the half's NDC, with the half's Labo
// offset (labo_offset, NDC). The size of one guest pixel of the HUD panel in the eye's image: tan_per_pixel (the
// panel's width in tangent per pixel of a half, exec.c's VR_SCREEN_WIDTH / (2 VR_SCREEN_HALF_W)) over the eye's FOV.
// A marker's panes scale together about its point (D088: each pane scaled about its own origin drew apart from its
// siblings, a balloon's background, centred, from its text, which starts at its left; user, 2026-11-09); the
// crosshair's pieces, one shape, are all sized about its centre. Its size, as an object's, falls with its distance:
// the panel's at Link's distance (focus, metres; 1 when not positive), within VR_WORLD_HUD_SIZE_MIN (the reticle:
// VR_AIM_SIZE_MIN) to VR_WORLD_HUD_SIZE_MAX of that, so a far marker stays readable. Project a card through the eye
// camera. The diagnostic overrides retain the old flat placement: keep_image_up leaves it upright and test_turn
// adds a clockwise image turn to the direction of gravity. Writes the move, and placed for the log.
static inline void vr_hud_marker_move(const VrHudMarkerView *seen, const PcVrHudPoint *marker, const float *viewport,
                                      double labo_offset, double tan_per_pixel, double focus, bool keep_image_up,
                                      double test_turn, VrHudMove *move, VrHudMarkerPlacement *placed) {
    const double left = seen->fov_tangents[0], right = seen->fov_tangents[1];
    const double up = seen->fov_tangents[2], down = seen->fov_tangents[3];
    const double *from_eye = seen->from_eye, *view = seen->view, *image = seen->image;
    const double point_x = viewport[0] + (marker->layout[0] / 640.0 + labo_offset + 1) * 0.5 * viewport[2];
    const double point_y = viewport[1] + (1 - marker->layout[1] / 360.0) * 0.5 * viewport[3];
    const double scale_x = tan_per_pixel * viewport[2] / (right - left);
    const double scale_y = tan_per_pixel * viewport[3] / (up - down);
    placed->anchor[0] = point_x;
    placed->anchor[1] = point_y;
    placed->eye_at[0] = viewport[0] + image[0] * viewport[2];
    placed->eye_at[1] = viewport[1] + image[1] * viewport[3];
    const bool project_card = !keep_image_up && test_turn == 0;
    const double *sizing_point = project_card ? seen->from_midpoint : from_eye;
    const double away = sqrt(sizing_point[0] * sizing_point[0] + sizing_point[1] * sizing_point[1] +
                             sizing_point[2] * sizing_point[2]);
    const double least = marker->frame ? VR_WORLD_HUD_SIZE_MIN : VR_AIM_SIZE_MIN;  // (frame 0: crosshair)
    const double grow = fmin(VR_WORLD_HUD_SIZE_MAX, fmax(least, (focus > 0 ? focus : 1) / away));
    if (project_card && away > 1e-9) {
        vr_hud_project_card(seen, viewport, labo_offset, away, tan_per_pixel * grow * away, move, placed);
        return;
    }
    double *image_up = placed->image_up;
    image_up[0] = 0;
    image_up[1] = -1;
    if (!keep_image_up) {
        const double local_up[3] = {0, 1, 0};
        double view_up[3];
        vr_rotate(seen->to_eye, local_up, view_up);
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
    // The whole marker scales and turns about its point, which lands where the eye sees it.
    placed->scale[0] = grow * scale_x;
    placed->scale[1] = grow * scale_y;
    vr_hud_move_place(move, viewport, labo_offset, placed->anchor, placed->eye_at, placed->scale, image_up);
}

// The fade's move into the half with this viewport: enlarged VR_FADE_GROW times about the half's centre, upright.
static inline void vr_hud_fade_move(VrHudMove *move, const float *viewport, double labo_offset) {
    const double centre[2] = {viewport[0] + viewport[2] * 0.5, viewport[1] + viewport[3] * 0.5};
    const double grow[2] = {VR_FADE_GROW, VR_FADE_GROW}, image_up[2] = {0, -1};
    vr_hud_move_place(move, viewport, labo_offset, centre, centre, grow, image_up);
}

#endif
