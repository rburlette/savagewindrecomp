// Part of exec.c (D133): PC VR's side of the renderer: eye images, the HUD's routes, the world-tied HUD.
// Included where it stands by exec.c, so it compiles in exec.c's translation unit as before.

// =====================================================================================================================
// PC VR (D048 onward): the renderer's side of third-person VR. It finds the two eye images the game draws in its Labo
// VR mode, copies them for the headset, splits the HUD and menus off onto flat panels, draws the pieces the game ties
// to the world at their 3D points, and keeps the desktop window showing the left eye. "Labo" is the game's Nintendo
// Labo VR mode, whose two eye cameras PC VR reuses; the D numbers are decisions in the root repository's
// docs/DECISIONS.md. The vr_ names are PC VR's (its play configuration, runtime/pc_vr_config.h; log lines [pc-vr-...]).
//
// The modules, beside this file in runtime/nvn/vulkan/:
//  - vr_math.h        geometry on plain values: rotations, where an eye sees a point, the aim ray's hit, the letterbox;
//  - vr_scene_depth.h each eye's scene depth target, as its scene-final draw attached it;
//  - vr_frame.h       a frame's findings and its kind at the present (the per-frame result);
//  - vr_draw_route.h  where each 2D draw into the final image goes (the per-draw result), and the held-scene decision;
//  - vr_world_hud.h   the move of a world-tied HUD draw into its eye's half (markers, reticles, the fade);
//  - vr_eyes.c        the eye images: slots, the per-frame copy, the offer to the XR owner, the game reset;
//  - vr_aim.c         the crosshair's depth along the aim ray, sampled from the scene's depth;
//  - vr_mirror.c      the desktop window's copy of the left eye (D085);
//  - vr_shots.c       diagnostic PNG shots of the eye and HUD images.
// The two results: the per-draw route (vr_draw_route.h), asked by draw_ for every HUD draw; and the per-frame kind
// (vr_frame.h), decided by vr_eyes.c at each present from the findings the draws below set.
//
// What stays in exec.c's translation unit (this file, vr_exec_hud_image.inc, vr_exec_motion.inc,
// vr_exec_rune_target.inc and the call sites in exec.c), and why: the hubs that read the executor's private state
// (st, dc, the pass, the program and its
// textures, the guest-to-device mapping) and hand the modules plain values; the code on the per-draw path (the draw
// hooks, the route's hub, the world-tied HUD's hooks, the rune aim), which stays in this translation unit because the
// play build has no link-time optimisation, with the state it reads defined here so that its reads stay direct loads
// (vr_eyes.h says why); the HUD's own image and its pass (begin_pass, texture_info, vr_hud_blend); and the executor's
// PC VR policy (stereo_hold, the split passes).
//
// The Labo frame, in guest (Switch) pixels. The game renders each eye's scene into an 800x900 image (an "eye-shaped"
// image: aspect 0.87 to 0.91), then places both into the two 960x1080 halves of its 1920x1080 "final image", draws the
// HUD and menus over them, and last runs its lens pass, which draws the final image to the window through the Labo
// lens distortion. PC VR copies the two halves just before the lens pass (vr_capture_eyes).
//
// Upstream's short names, used throughout this file: st is the guest's current NVN state (render targets st.rt,
// viewports, uniform buffers st.ubuf); dc is the Vulkan state of the draw being recorded; pr is the draw's program
// (its shaders' interface: pr->fi is the fragment stage's, with its textures); r is the recorded command (CmdRec).
// =====================================================================================================================

// ------------------------------------------------------------------------------------------- diagnostic image shots
// Saving GPU images of the PC VR path as PNGs (SAVAGEWIND_PC_VR_SHOT, SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS): vr_shots.c.
// A shot needs no render pass open: after a draw, vr_shot_after_draw ends the pass first.
#include "vr_shots.h"

// ------------------------------------------------------------------------------------------------ VR eye images: state
// D048 (part of the PC VR play configuration): the eye-image producer, vr_eyes.c. Its state is public
// (vr_eyes.h) for the draw hooks below, which read it and set the frame's findings on the per-draw path, and is defined
// here, in their translation unit, so that those accesses stay direct loads and stores (vr_eyes.h says why).
#include "vr_eyes.h"
VrEyes vr_eyes;

// ----------------------------------------------------------------------------------------------- the crosshair's depth
// D073: where the aim ray meets the scene, so the crosshair can be drawn at that depth (vr_world_hud_draw).
//
// An eye's scene depth is the depth target the game's finishing draw of the eye layer (its scene-final draw, known by
// the game's tag) has attached (vr_scene_depth.h). The draw hooks below keep that state, vr_scene_depth, on the
// per-draw path. At the lens pass the aim module samples
// the last certified eye's scene depth along the aim ray and, a frame or two later, reads where the ray meets the
// scene (vr_aim.c, vr_aim.h). Its result, vr_aim_result, is defined here for the reticle and the rune aim, which read it
// on the draw and constant buffer paths: their reads stay direct (vr_eyes.h says why).
// Each eye's scene depth target and the last eye drawn (VrSceneDepth).
#include "vr_scene_depth.h"
static VrSceneDepth vr_scene_depth = {.last_eye = -1};
#include "vr_aim.h"
VrAimResult vr_aim_result;
// DLAA on each eye image at its placement (an experiment, vr_dlaa.h): scene_post's call, with the eye's scene depth.
#include "vr_dlaa.h"
// D073, D088: the world-tied HUD. When the HUD draw being recorded is a marker's, a reticle's or the fade's
// (vr_world_hud_draw, vr_hud_fade), how to move it into its eye's half: the model, the move and its geometry are
// vr_world_hud.h, the hub below (HUD markers in the world). The move is applied to the draw's vertices, in the
// projection matrix of nn::ui2d's vertex constants (vr_world_hud_patch), so the GPU clips the moved draw against the
// eye's half. (Moving the draw's viewport, as before, clipped it at the game's own screen edge, and could not turn it.)
// Its state, who sets and resets what: vr_world_hud.h. Defined here, in the translation unit of the per-draw hooks that
// read it, so those reads stay direct loads (vr_eyes.h says why).
#include "vr_world_hud.h"
static VrWorldHud vr_world_hud;

// The draw's tags, what the game said about the draw when it was recorded (runtime/nvn/nvn.h), set for every executed
// draw by draw() before draw_ runs, and read by the hooks of that draw.
// D088: the game UI screen that recorded the draw being executed (its tag, runtime/pc_vr_hud.c; 0: none). A draw goes
// into the world through its own pane's marker (vr_world_hud_draw), so a popup's text beside a marker stays with
// its box on the panel.
static uint32_t vr_draw_screen;
// D088: the marker report bound to the draw's pane, or to its nearest parent with a binding (runtime/pc_vr_hud.c
// pc_vr_marker_drawing); PC_VR_MARKER_FLAT: the game gave the piece a fixed HUD place; 0: no world point known.
static uint32_t vr_draw_marker;
// What the UI pane that recorded the draw is (runtime/pc_vr_hud.c pc_vr_pane_role): a reticle, the fade, a backdrop.
static PcVrPaneRole vr_draw_role;
// The draw's own render certificate (0: none): the eye layer it was recorded in, with that frame's pose.
static uint64_t vr_draw_certificate;
static bool vr_draw_rune_test;  // This draw is the game's rune target test (its position moved onto the aim ray).
// [pc-vr-audit] SAVAGEWIND_LOG_PANES: the number of the UI pane that recorded the draw (0: none, or logging off).
static uint32_t vr_draw_pane;

// vr_rotate: the pose maths the aim ray, the world-tied HUD markers and the rune aim share.
#include "vr_math.h"

// ------------------------------------------------------------------------- VR eye images: shots over the held scene
// This frame's world-tied HUD draws left out over the held scene (draw_), for the log line of the quick menu shots
// (SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS, vr_shots.c vr_quick_menu_shot). Counted here, a plain increment on the draw path;
// cleared at each present (vk_vr_present).
static unsigned vr_frame_world_left_out;

// ----------------------------------------------------------------- VR eye images: world pieces without a marker link
// A world screen's HUD draw (pc_vr_screen_kind) without a marker link (marker 0: no pane up its parents has a binding,
// or the game moved the piece without a known world point; a fixed HUD place's draws are drawn flat, not counted
// here) is left out of the HUD's image (draw_): its
// world point is not known, and on the panel it would be a flat copy at the wrong depth (the speech balloon flashed
// flat as a quick menu closed; user, 2026-11-11). Each run of game presents with such draws outside a held scene is
// logged once it ends, the first VR_UNLINKED_WORLD_LINES runs: how many draws over how many presents, the run's first
// present, and how many presents after the last one over a held scene it began.
#define VR_UNLINKED_WORLD_LINES 16
static struct {
    uint64_t frame_draws;        // this frame's such draws (draw_)
    uint64_t run_draws, run_presents, run_first_present;  // the current run (run_presents 0: none)
    uint64_t last_held_present;  // the last game present over a held scene (0: none yet)
    unsigned lines;              // the runs logged
} vr_unlinked_world;

// At each game present (vk_vr_present), with whether its frame was over the held scene.
static void vr_unlinked_world_present(uint64_t game_present_id, bool over_world) {
    if (over_world) vr_unlinked_world.last_held_present = game_present_id;
    const uint64_t draws = vr_unlinked_world.frame_draws;
    vr_unlinked_world.frame_draws = 0;
    if (draws) {
        if (!vr_unlinked_world.run_presents) {
            vr_unlinked_world.run_first_present = game_present_id;
            vr_unlinked_world.run_draws = 0;
        }
        ++vr_unlinked_world.run_presents;
        vr_unlinked_world.run_draws += draws;
        return;
    }
    if (!vr_unlinked_world.run_presents) return;
    if (vr_unlinked_world.lines < VR_UNLINKED_WORLD_LINES) {
        ++vr_unlinked_world.lines;
        char after_held[24] = "none";
        if (vr_unlinked_world.last_held_present && vr_unlinked_world.last_held_present < vr_unlinked_world.run_first_present)
            snprintf(after_held, sizeof after_held, "%llu", (unsigned long long)(vr_unlinked_world.run_first_present -
                                                                                 vr_unlinked_world.last_held_present));
        LOG("[pc-vr-eyes] world pieces without a marker link left out: draws=%llu presents=%llu first_present=%llu "
            "presents_after_held_scene=%s (1: the first present after it; line %u of %u)\n",
            (unsigned long long)vr_unlinked_world.run_draws, (unsigned long long)vr_unlinked_world.run_presents,
            (unsigned long long)vr_unlinked_world.run_first_present, after_held, vr_unlinked_world.lines,
            VR_UNLINKED_WORLD_LINES);
    }
    vr_unlinked_world.run_presents = 0;
}

// ----------------------------------------------------------------------------------- VR eye images: the per-frame copy
// At the lens pass, which reads the finished final image (combined): copies both halves into a free slot's eye images,
// and the HUD's own image into its HUD image, once per frame (vr_eyes.c), then the frame's shots and the crosshair's
// depth sample. The copies complete at vr_eyes.ready_seq.
static void vr_capture_eyes(TexBackend *combined) {
    VrEyeSlot *slot = vr_eyes_capture_claim(combined);
    if (!slot) return;
    vk_end_pass();
    if (!vr_eyes_capture_fill(slot, combined, vr_hud_image.image, vr_hud_image.drawn, vr_hud_image.width,
                              vr_hud_image.height))
        return;
    const uint32_t half_width = combined->width / 2, height = combined->height;
    // Shots of what the XR owner will copy (the final image itself, "combined", stops at 4096 wide). (No pass is open:
    // ended above.)
    vr_shot_slot_eyes(slot->image, half_width, height);
    // D073: the crosshair's depth, from the last eye whose scene was drawn, when it is certified this frame (vr_aim.c).
    // (A shot above submits the frame's commands and waits for the GPU, so the command buffer is asked for again:
    // recording into the submitted one crashed in the driver on every shot frame.)
    const int aim_eye = vr_scene_depth.last_eye >= 0 && (vr_eyes.frame.certified >> vr_scene_depth.last_eye & 1) ?
        vr_scene_depth.last_eye : -1;
    vr_aim_sample(vk_cmd(), aim_eye, aim_eye >= 0 ? &vr_eyes.frame.eyes[aim_eye] : NULL, vr_scene_depth.depth);
    if (vr_shot_due("combined"))
        LOG("[pc-vr-world-hud] shot frame: world draws %llu, aim distance %.2f from present %llu (now %llu), aim rays without a hit %llu\n",
            (unsigned long long)vr_world_hud.frame_draws, vr_aim_result.distance, (unsigned long long)vr_aim_result.frame,
            (unsigned long long)renderer_present_count(), (unsigned long long)vr_aim_result.misses);
    vr_shot(combined, "combined", false);
    if (slot->hud) vr_shot_slot_hud(slot->image, half_width, height);
    if (slot->hud && vr_shot_due("hud-full"))  // (diagnostic: both halves, unflipped like the combined shot)
        vr_shot_hud_full(vr_hud_image.image, combined);
    vr_quick_menu_shot(slot->image, vr_eyes.frame.over_world, slot->hud, half_width, height,
                       vr_frame_world_left_out);  // (SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS)
    vr_eyes_capture_done();
}

// ---------------------------------------------------------------------------------- VR eye images: which draw is which
// Whether a texture is this session's final image: the image the game places its eyes into, which the lens pass
// samples (vr_eyes.final_backend, set by those draws: the eye placement's target, vr_after_draw, and what the game's
// display draw samples, vr_display_draw_source). Per draw (vr_is_hud_draw), and for the diagnostic logs.
static bool vr_is_final_image(const NvnTexture *texture) {
    return texture && texture->backend && texture->backend == vr_eyes.final_backend;
}

// The quick menus over the world (D-pad: weapons, shields, bows, runes; D072). While one opens the game draws the
// scene, then only its own picture of the world (vr_hud_backdrop_fate) with the menu. Those frames would go to the
// headset as a menu panel (D056): a flat screen in place of the world. Instead, in a frame without a scene while the
// game is in a quick menu (pause state 4), its 2D content goes into the HUD's image as in a scene frame, and the
// headset shows it on the HUD's panel over the last scene pair, which the XR owner keeps showing (over_scene). Only
// after an accepted scene pair (a menu panel pair replaces the eyes' images). The pause menu's pages stay menu
// panels (D056, D068).
// Decided at the frame's first draw into the final image, before any eye was certified (vr_frame_over_held_scene,
// vr_draw_route.h), and latched here: asked per draw into the final image while the frame has placed no scene
// (vr_is_hud_draw), it reads its inputs once a frame.
#include "vr_draw_route.h"
static bool vr_quick_menu_over_world(void) {
    const bool scene_unseen =
        !(vr_eyes.frame.scene_seen || vr_eyes.frame.placement_seen || vr_eyes.frame.certified);
    if (!scene_unseen) return false;  // (the latch untouched)
    if (!vr_eyes.frame.over_world_known) {
        extern bool framepace_quick_ui(void);  // (pause state 4, hle/framepace.c)
        vr_eyes.frame.over_world_known = true;
        // (each input read only while the ones before it leave the answer open, in the decision's order; a shop's
        // list is the same case as a quick menu, but leaves the pause state alone: by its screen, runtime/pc_vr_hud.c)
        const bool scene_held = vr_eyes.scene_held != 0;
        const bool hud_panel_on = scene_held && pc_vr_panel_depth(PC_VR_PANEL_HUD);
        const bool quick_menu = hud_panel_on && framepace_quick_ui();
        const bool shop_open = hud_panel_on && !quick_menu && pc_vr_shop_open();
        vr_eyes.frame.over_world =
            vr_frame_over_held_scene(scene_unseen, scene_held, hud_panel_on, quick_menu, shop_open);
    }
    return vr_eyes.frame.over_world;
}

// D057: whether this draw is the gameplay HUD and goes into the HUD's own image (begin_pass) instead of the final
// image: a draw one of the game's UI screens recorded (vr_draw_screen, runtime/pc_vr_hud.c), in a frame whose scene
// was placed into the final image or in a quick menu's frame over the held scene. The scene placement and the game's
// own composites are no screen's draws and stay with the scene. Per draw (draw_): the cheap test that keeps every
// other draw off the HUD's routing.
static bool vr_is_hud_draw(void) {
    if (vr_eyes.state <= 0 || vr_hud_image.state < 0 || st.rt.ncolor != 1 || !vr_is_final_image(st.rt.color[0]))
        return false;
    if (vr_eyes.frame.placement_seen ? !pc_vr_panel_depth(PC_VR_PANEL_HUD) : !vr_quick_menu_over_world()) return false;
    return vr_draw_screen != 0;
}

// ---------------------------------------------------------------------------- VR eye images: HUD boxes over the screen
// A HUD draw that shows the screen behind it: a dialog's box shows the world through itself that way, and a menu lays
// the game's picture of the world behind itself. On the HUD's panel that is a flat picture of the eyes' views, out of
// place against the world seen around and through the panel. The pane that draws it says what it is (vr_draw_role,
// runtime/pc_vr_hud.c pc_vr_pane_role), for the draws of that pane that sample such a picture.

// A texture a backdrop pane shows the screen through: a render target or a display image (flags & 9), which is
// where the game keeps its pictures of the screen. The pane's own art is ordinary textures. (A rule by the texture's
// flags, not by who drew it: backlog item 11.) Per texture of a backdrop's draw (vr_hud_backdrop_fate, texture_info).
static bool vr_backdrop_source(const NvnTexture *texture) { return texture && (texture->flags & 9); }

// What becomes of a HUD draw (VrBackdropFate, vr_draw_route.h), asked per HUD draw that goes to the HUD's image:
//   0: no backdrop; drawn as it is.
//   1: a box (PC_VR_PANE_BOX): drawn with opaque black in place of the screen (vr_backdrop_pictures_black, read where
//      its textures are bound). The box keeps its own shading and translucency over the world seen through the panel,
//      without the flat picture of the screen.
//   2: the game's picture of the world behind a menu (PC_VR_PANE_WORLD_PICTURE): not drawn. The world itself shows
//      there.
//   3: a fade into the screen behind (PC_VR_PANE_EDGE_MASK: the ends of the quick menu's item row, which show the
//      screen over the items scrolling out). Its picture of the screen is black too (vr_backdrop_pictures_black), and
//      it erases the HUD under it by its alpha (vr_backdrop_erase, vr_hud_blend), so the items fade into the world
//      seen through the panel instead of into a black box.
// The role asks for the fate (vr_backdrop_fate_of_role); a draw that samples no picture of the screen keeps 0.
// SAVAGEWIND_PC_VR_HUD_BACKDROP=1: drawn as the game draws it.
// The two flags, set per HUD draw by its route (vr_route_hud_draw; cleared first for every draw): the draw samples its
// pictures of the screen as opaque black (fates 1 and 3), and it erases the HUD under it (fate 3).
static bool vr_backdrop_pictures_black, vr_backdrop_erase;
static VrBackdropFate vr_hud_backdrop_fate(Prog *program) {
    const VrBackdropFate fate = vr_backdrop_fate_of_role(vr_draw_role);
    if (!fate) return VR_BACKDROP_NONE;
    for (int i = 0; i < program->fi->ntex; i++)
        if (vr_backdrop_source(shader_texture_object(1, &program->fi->tex[i]))) return fate;
    return VR_BACKDROP_NONE;
}

// D057: the HUD image keeps premultiplied colour with coverage in alpha, so the XR owner can lay it over the scene.
// This adjusts the alpha blend of each draw into it (s->blend[0], NVN's codes: [1] colour source factor, [2] colour
// destination factor, [3] alpha source, [4] alpha destination, [5] colour op, [6] alpha op; factors 1 ZERO, 2 ONE,
// 6 ONE_MINUS_SRC_ALPHA, 9 DST_COLOR; op 1 ADD):
//  - over-blending accumulates coverage: alpha ONE / ONE_MINUS_SRC_ALPHA;
//  - additive blending (colour destination factor ONE) leaves coverage alone: alpha ZERO / ONE;
//  - a multiplying draw (colour source factor DST_COLOR: a box that darkens what is under it) leaves coverage alone
//    too. It scales the HUD drawn before it as on the screen, and over the image's empty parts, where the world shows
//    through, it adds nothing. Over-blended, it was black there at its own alpha: the black rectangle over the button
//    prompt during magnesis (D073). BetterVR's hook_FixUIBlending keeps the destination's alpha for this blend too;
//  - a fade into the screen behind (vr_hud_backdrop_fate's 3) scales colour and coverage under it by one minus
//    its alpha (ZERO / ONE_MINUS_SRC_ALPHA for both): where it is opaque the world shows through the panel;
//  - unblended draws write the shader's alpha. Alpha is written wherever colour is.
// Per draw in a pass into the HUD image, on its pipeline key (draw_).
static void vr_hud_blend(PipeState *state) {
    if (state->write_mask[0] & 7) state->write_mask[0] |= 8;
    uint8_t *blend = state->blend[0];
    if (vr_backdrop_erase && (state->write_mask[0] & 8)) {
        state->blend_on |= 1;
        blend[1] = blend[3] = 1;
        blend[2] = blend[4] = 6;
        blend[5] = blend[6] = 1;
        return;
    }
    if (!(state->blend_on & 1)) return;
    const bool additive = blend[2] == 2, multiply = blend[1] == 9;
    blend[3] = additive || multiply ? 1 : 2;
    blend[4] = additive || multiply ? 2 : 6;
    blend[6] = 1;
}

// -------------------------------------------------------------------------------------- VR eye images: the layer draws
// The eye's scene-final draw and its placement are known by the game code that issues them (nvn_layer_draw).
// Whether a draw's tag (nvn_layer_draw) says it is this kind of draw of an eye's layer, or of a layer that may be one
// (PC_VR_LAYER_DRAW_MAYBE_EYE: after a load, before any eye layer is known). Per draw (vr_after_draw).
static bool vr_eye_layer_draw(uint32_t layer_draw, PcVrLayerDraw kind) {
    return (layer_draw & PC_VR_LAYER_DRAW_KIND) == (uint32_t)kind &&
        (layer_draw & (PC_VR_LAYER_DRAW_EYE | PC_VR_LAYER_DRAW_MAYBE_EYE));
}

// ----------------------------------------------------------------------------------- VR eye images: the display draw
// The game's draw of its final image to the display, into the image it presents: Labo's lens pass, or its plain copy
// before VR mode is on. Known by the game code that issues it (PC_VR_LAYER_DRAW_DISPLAY). Returns the final image:
// the render target the draw samples (the lens pass also samples a lookup texture, which the game did not create
// as a render target or display image: flags & 9). NULL: the draw is not that one. Per draw into one colour target:
// before it is recorded (vr_window_mirror) and after (vr_after_draw's job 3).
static NvnTexture *vr_display_draw_source(Prog *program, uint32_t layer_draw) {
    NvnTexture *target = st.rt.ncolor == 1 ? st.rt.color[0] : NULL;
    if ((layer_draw & PC_VR_LAYER_DRAW_KIND) != PC_VR_LAYER_DRAW_DISPLAY || !target ||
        !renderer_texture_presented(target))
        return NULL;
    NvnTexture *source = NULL;
    for (int i = 0; i < program->fi->ntex && !source; ++i) {
        const NvnTexEntry *entry = shader_texture_entry(1, &program->fi->tex[i]);
        NvnTexture *texture = entry ? entry->tex : NULL;
        if (texture && texture != target && texture->backend && (texture->flags & 9)) source = texture;
    }
    static uint64_t draws;  // (the total, logged now and then)
    if (source && ++draws % 4000 == 1)
        LOG("[pc-vr-eyes] display draws by their game code: %llu\n", (unsigned long long)draws);
    return source;
}

// -------------------------------------------------------------------------------------- VR eye images: after each draw
// A shot taken after a draw (an eye's scene-final draw: vr_after_draw's job 1), while the draw's render pass is still
// open. A shot needs no pass open (vr_shots.h), so the pass ends here, counted at this line in the pass-end
// statistics, exactly when the shot will save an image: one is due for name and the image can be saved.
static void vr_shot_after_draw(TexBackend *image, const char *name) {
    if (vr_shot_due(name) && vr_shot_image_savable(image)) vk_end_pass();
    vr_shot(image, name, false);
}

// Per draw, after it is recorded (draw_, unless an empty draw was omitted): the eye-image producer's three jobs, in
// this order:
//  1. an eye's scene-final draw records its certificate (which eye, which pose), its finished image and its scene
//     depth target (D073: the depth target that draw has attached, vr_scene_depth.h);
//  2. the placement of an eye's image into the final image: the frame's later 2D draws are its HUD, its target is
//     the final image, and it is checked to show that eye's image in that eye's half;
//  3. the game's first draw of its final image to the display (the lens pass, when it is drawn) starts the copy.
// Only for a draw into one colour target with the eye images on, by what the game says the draw is (nvn_layer_draw,
// with its own certificate).
static void vr_after_draw(Prog *program, const CmdRec *command) {
    // ---- jobs 1 to 3: what the game says the draw is
    if (vr_eyes.state < 0 || !xr_runtime_eye_images_enabled() || st.rt.ncolor != 1 || !st.rt.color[0]) return;
    NvnTexture *target = st.rt.color[0];
    // What the game says this draw is (nvn_layer_draw): the finishing draw of an eye's layer, or the placement of an
    // eye's finished image. Its eye and pose are its own certificate's.
    const uint32_t layer_draw = nvn_layer_draw(command);
    const bool eye_final = vr_eye_layer_draw(layer_draw, PC_VR_LAYER_DRAW_FINAL);
    const bool eye_placement = vr_eye_layer_draw(layer_draw, PC_VR_LAYER_DRAW_PLACEMENT);
    static uint64_t eye_finals, eye_placements;  // (the totals, logged now and then)
    eye_finals += eye_final;
    eye_placements += eye_placement;
    if ((eye_final || eye_placement) && (eye_finals + eye_placements) % 2000 == 1)
        LOG("[pc-vr-eyes] layer draws by their game code: %llu eye finals, %llu eye placements\n",
            (unsigned long long)eye_finals, (unsigned long long)eye_placements);
    PcVrRenderCertificate certificate;
    const bool certified = (eye_final || eye_placement) && vr_draw_certificate &&
        pc_vr_render_trace_resolve(vr_draw_certificate, &certificate);
    // ---- job 1: an eye's scene-final draw
    if (eye_final) {
        vr_eyes.frame.scene_seen = true;
        if (!(layer_draw & PC_VR_LAYER_DRAW_UNBOUND)) vr_eyes.frame.scene_bound_seen = true;
        vr_eyes.frame.over_world = false;  // (a scene after all: vr_quick_menu_over_world)
        // (for the shots: the image it writes, and the HDR scene it reads, the texture of the same size)
        TexBackend *output = target->backend, *input = NULL;
        for (int i = 0; i < program->fi->ntex && !input; ++i) {
            const NvnTexEntry *entry = shader_texture_entry(1, &program->fi->tex[i]);
            if (entry && entry->tex && entry->tex != target && entry->tex->width == target->width &&
                entry->tex->height == target->height)
                input = entry->tex->backend;
        }
        if (certified) {
            const unsigned eye = certificate.native_index;
            vr_eyes.frame.eyes[eye] = certificate;
            vr_eyes.frame.certified |= 1u << eye;
            // (D073) The eye's scene depth: the depth target the game's finishing draw of the eye layer has attached.
            vr_scene_depth.depth[eye] = st.rt.depth ? st.rt.depth->backend : NULL;
            {  // (the target named whenever an eye's changes, the first 16 times: what the game's draw attached)
                static const TexBackend *named_depth[2];
                static unsigned depth_changes;
                const TexBackend *depth_target = vr_scene_depth.depth[eye];
                if (depth_target && depth_target != named_depth[eye]) {
                    named_depth[eye] = depth_target;
                    if (depth_changes < 16) {
                        ++depth_changes;
                        LOG("[pc-vr-eyes] eye %u scene depth target: %ux%u format %d present=%llu (change %u of at most 16 "
                            "logged)\n", eye, depth_target->width, depth_target->height, (int)depth_target->format,
                            (unsigned long long)renderer_present_count(), depth_changes);
                    }
                }
            }
            if (!vr_scene_depth.depth[eye]) {  // (the first 8 times)
                static unsigned logged;
                if (logged < 8) {
                    ++logged;
                    LOG("[pc-vr-eyes] eye %u scene-final draw has no depth target present=%llu (line %u of 8)\n", eye,
                        (unsigned long long)renderer_present_count(), logged);
                }
            }
            vr_scene_depth.last_eye = (int)eye;
            if (input) vr_shot_after_draw(input, eye ? "hdr-eye1" : "hdr-eye0");
            if (output) vr_shot_after_draw(output, eye ? "eye1" : "eye0");
        } else if (output) {
            // No certificate (the headset asleep or unavailable): the shots still work, naming the eyes only by
            // the order of their outputs (vr_shot_scene_side). Nothing here counts as an eye's identity or admits an
            // image.
            const int side = vr_shot_scene_side(output->serial);
            if (side >= 0) {
                if (input) vr_shot_after_draw(input, side ? "hdr-scene1" : "hdr-scene0");
                vr_shot_after_draw(output, side ? "scene1" : "scene0");
            }
        }
        // (the eye scene's motion vectors: vr_exec_motion.inc)
        vr_eye_motion_scene_finished(certified ? (int)certificate.native_index : -1,
                                     certified ? vr_scene_depth.depth[certificate.native_index] : NULL);
        return;
    }
    // ---- job 2: the placement of an eye's image into the final image
    if (eye_placement) {
        vr_eyes.frame.placement_seen = true;  // (later 2D draws: D057's HUD)
        // The image the game places its eyes into is its final image.
        if (target->backend && target->backend != vr_eyes.final_backend) {
            static unsigned logged;
            if (logged < 8) {
                ++logged;
                LOG("[pc-vr-eyes] the final image is the eye placement's target (%ux%u, texture %llu), %s present=%llu\n",
                    target->width, target->height, (unsigned long long)target->debug_id,
                    vr_eyes.final_backend ? "not the image the lens pass showed" : "before any lens pass",
                    (unsigned long long)renderer_present_count());
            }
            vr_eyes.final_backend = target->backend;
        }
        // For an eye certified this frame, its placement (its own certificate says which eye): placed, in that eye's
        // half; the other half is the tripwire (VrFrameFindings misplaced).
        if (certified && (vr_eyes.frame.certified >> certificate.native_index & 1)) {
            const unsigned eye = certificate.native_index, half = st.viewport[0][0] * 2 >= target->width ? 1 : 0;
            if (eye == half) vr_eyes.frame.placed |= 1u << eye;
            else vr_eyes.frame.misplaced = true;
        }
        return;
    }
    // ---- job 3: the display draw
    // The lens pass (when it is drawn: vr_window_mirror): the game's first draw of its final image to the display.
    if (vr_eyes.filling) return;
    const NvnTexture *final_image = vr_display_draw_source(program, layer_draw);
    if (final_image) {
        vr_eyes.final_backend = final_image->backend;
        vr_capture_eyes(final_image->backend);
    }
}

// ----------------------------------------------------------------------------------- VR eye images: the desktop window
// D085: the window shows the left eye, letterboxed, instead of the game's Labo lens pass (vr_mirror.c, vr_mirror.h).
#include "vr_mirror.h"
#include "vr_lens_mask.h"  // (D114: the lens polygon as the headset's visible area, in draw)

// Per draw, before it is recorded (draw_). The tests that find the display draw stay here, inline and in their order;
// the mirror module is called only for that draw, which also starts the frame's copy of the eyes when the lens pass
// is not drawn. True: the draw was the lens pass, and must not be recorded.
static bool vr_window_mirror(Prog *program, uint32_t layer_draw) {
    if (vr_eyes.state < 0 || !xr_runtime_eye_images_enabled() || st.rt.ncolor != 1 || !st.rt.color[0] ||
        !program->fi->ntex)
        return false;
    NvnTexture *presented_texture = st.rt.color[0];
    const NvnTexture *display_source = vr_display_draw_source(program, layer_draw);
    TexBackend *presented = presented_texture->backend, *final_image = display_source ? display_source->backend : NULL;
    if (!final_image || !presented || !presented->image || presented->samples != 1 ||
        (presented->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!vr_mirror_takes(presented, final_image)) return false;
    if (!vr_eyes.filling) {
        vr_eyes.final_backend = final_image;
        vr_capture_eyes(final_image);
    }
    VrLetterbox box;  // (where the left half goes in the presented image)
    if (!vr_mirror_due(presented, final_image, &box)) return true;  // (copied this frame already, or nothing to copy)
    vk_end_pass();
    vr_mirror_copy(presented, final_image, &box);
    return true;
}

// ----------------------------------------------------------------------------- VR eye images: HUD markers in the world
// D073, D088: HUD content over the world, drawn into each eye where its marker's point is, instead of on the HUD's
// panel. The model, the spaces and units, the move and its geometry: vr_world_hud.h. Here, the hub: what only the
// executor knows (the draw's tags and constants blocks, the viewport, the scissor, the target, the eye certificates,
// the marker reports, the aim result, the focus distance, the switches) goes in as values, the move comes back into
// vr_world_hud, and the per-draw hooks below apply it (the constants c3, the scissor, the fragment position's scale).
// SAVAGEWIND_PC_VR_WORLD_HUD=0: off.

// For the vertex stage's constants c3 of a draw vr_world_hud moves (nn::ui2d's block: its first matrix gives the clip
// position; rows 0 and 1 are x and y, row 3 is w): binds a copy with the move in that matrix (vr_hud_move_patch).
// Per vertex constant buffer c3 bound (cbuf_info); the moved flag is tested first. False: not patched.
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size);
static bool vr_world_hud_patch(uint64_t address, uint64_t size, VkDescriptorBufferInfo *out) {
    if (!vr_world_hud.active || size < 64) return false;
    static float block[0x10000 / 4];
    const uint64_t copied = size > sizeof block ? sizeof block : size;
    memcpy(block, (const void *)(uintptr_t)address, copied);
    vr_hud_move_patch(&vr_world_hud.move, block);
    *out = ubo_span(vk_transient_copy(block, copied, 256), copied);
    return true;
}

// Before begin_pass, for a draw vr_is_hud_draw sent to the HUD's image: whether it is a marker's. If it is,
// sets up vr_world_hud's move so the draw goes into its eye's half instead (vr_hud_marker_move: _patch, _scissor,
// _rscale), and returns true.
static bool vr_world_hud_draw(void) {
    if (!st.rt.color[0]) return false;
    // D088: where the draw goes. Exactly: the game stored a reported point into its pane, or one of its parents
    // (vr_draw_marker, the binding: the helper's markers, the speech balloons), or its pane belongs to an aiming
    // reticle (vr_draw_role). A piece at the game's own fixed HUD place (PC_VR_MARKER_FLAT) and everything else stay
    // on the panel.
    if (vr_draw_marker == PC_VR_MARKER_FLAT) return false;
    if (!vr_draw_marker && vr_draw_role != PC_VR_PANE_RETICLE) return false;
    const Range pane_constants = st.ubuf[0][0], offset_constants = st.ubuf[0][10];
    if (!pane_constants.addr || pane_constants.size < 64 || !offset_constants.addr || offset_constants.size < 4 ||
        st.ds[0])
        return false;  // (no depth test: ui2d's)
    // Only nn::ui2d's blocks, as hud_patch tells them, of a pane or a text.
    float origin[2];  // where the pane's (or the text's) origin is, in the layer's NDC before the Labo offset (the log)
    float pane_size;  // the pane's larger side in layout units (0: a text; the log)
    if (!vr_hud_marker_block((const float *)(uintptr_t)pane_constants.addr, pane_constants.size, origin, &pane_size))
        return false;
    const float *viewport = st.viewport[0], half_width = st.rt.color[0]->width * 0.5f;
    if (!vr_hud_one_half(viewport, half_width)) return false;  // (a draw over one half only)
    const unsigned eye = vr_hud_eye_of_half(viewport, half_width);
    if (!(vr_eyes.frame.certified >> eye & 1)) return false;
    const PcVrRenderCertificate *cert = &vr_eyes.frame.eyes[eye];
    const XrPoseSnapshot *pose = &cert->native_snapshot;
    if (!(cert->units_per_meter > 0) || !pose->calibration.epoch) return false;

    // Which point the draw belongs to (marker).
    PcVrHudPoint linked;
    const PcVrHudPoint *marker = NULL;
    if (vr_draw_marker && pc_vr_hud_point_by_seq(vr_draw_marker, &linked)) {
        // Linked by pane: the report bound to it, however old (the binding keeps a copy when the ring has moved on),
        // scaled to where the panes are drawn.
        vr_hud_marker_on_layer(&linked);
        marker = &linked;
        ++vr_world_hud.exact;
    } else if (vr_draw_role == PC_VR_PANE_RETICLE && vr_aim_result.distance > 0) {
        // A reticle's pane (the bow's, a rune's, a thrown weapon's): on the aim ray, at the depth the scene has there
        // (vr_aim.c), about the layout's centre. The last distance is held however old (D101; there is none only
        // before the first since the game reset), as the rune aim holds it: no window of presents (backlog 9).
        linked = vr_hud_reticle_point(vr_aim_result.distance);
        marker = &linked;
    }
    if (!marker) {
        ++vr_world_hud.unmatched;
        return false;
    }

    // The marker's point in the eye's view, and where in its image.
    VrHudMarkerView seen;
    const VrHudSight sight = vr_hud_marker_view(marker, cert, eye, &seen);
    if (sight == VR_HUD_BEHIND) {
        ++vr_world_hud.behind;
        return false;
    }
    if (sight != VR_HUD_SEEN) return false;  // (a FOV with no width or height)
    const double labo_offset = ((const float *)(uintptr_t)offset_constants.addr)[0];  // (this half's Labo offset, NDC)
    const double tan_per_pixel = VR_SCREEN_WIDTH / (2 * VR_SCREEN_HALF_W);  // (on the HUD panel)
    // Link's distance (the stamina wheel's), at which a marker has the HUD panel's size.
    const double focus = pc_vr_camera_focus_distance();
    // Project the marker's panes, or the reticle and its count on the aim ray, as one card for both eyes.
    VrHudMarkerPlacement placed;
    vr_hud_marker_move(&seen, marker, viewport, labo_offset, tan_per_pixel, focus, &vr_world_hud.move, &placed);
    ++vr_world_hud.draws;
    ++vr_world_hud.frame_draws;
    return true;
}

// The screen Fade's black pane (the black that clears after a load), known by its role (pc_vr_pane_role: the screen
// and pane names), is drawn into each eye's half of the final image, which is that eye's whole view, instead of into
// the HUD's image, whose panel shows only the screen's rectangle. Enlarged about the half's centre so the Labo offset
// leaves no strip; solid, so nothing else changes. Per HUD draw outside a held scene (vr_route_hud_draw), before the
// marker's placement; sets vr_world_hud's move when it takes the draw.
static bool vr_hud_fade(Prog *program) {
    (void)program;
    if (!st.rt.color[0] || vr_draw_role != PC_VR_PANE_FADE || st.ds[0]) return false;
    const float *viewport = st.viewport[0], half_width = st.rt.color[0]->width * 0.5f;
    if (!vr_hud_one_half(viewport, half_width)) return false;
    const Range offset_constants = st.ubuf[0][10];  // (this half's Labo offset, as vr_world_hud_draw reads it)
    const double labo_offset = offset_constants.addr && offset_constants.size >= 4 ?
        ((const float *)(uintptr_t)offset_constants.addr)[0] : 0;
    vr_hud_fade_move(&vr_world_hud.move, viewport, labo_offset);
    return true;
}

// After apply_scissor, for a draw vr_world_hud moves: the eye's half. The draw's viewport stays the game's, the half's,
// and its vertices are moved inside it. A scissor of the game's own smaller than that viewport (a clip of the pane's
// contents) moves with the draw (vr_hud_move_scissor). Set only when it differs from the one set. Per draw (draw_);
// the moved flag is tested first.
static void vr_world_hud_scissor(void) {
    if (!vr_world_hud.active || !dc.sc.extent.width || !dc.sc.extent.height) return;
    int pixels[4];  // left, top, right, bottom, in the pass's device pixels
    vr_hud_move_scissor(&vr_world_hud.move, st.viewport[0], st.scissor[0], st.rt.color[0]->height, drw.vp_sx,
                        drw.vp_sy, drw.vp_ox, drw.vp_oy, ps.w, ps.h, pixels);
    VkRect2D rect = {{pixels[0], pixels[1]}, {(uint32_t)(pixels[2] - pixels[0]), (uint32_t)(pixels[3] - pixels[1])}};
    if (!memcmp(&dc.sc, &rect, sizeof rect)) return;
    rec_CmdSetScissor(vk_cmd(), 0, 1, &rect);
    dc.sc = rect;
}

// For shaders that read the fragment's position (rscale: {s_x, s_y, o_x, o_y}, the guest-to-device mapping): the move
// without its turn folded in (vr_hud_move_rscale). Per draw (draw_, after the scissor); the moved flag is tested first.
static void vr_world_hud_rscale(float rscale[4]) {
    if (!vr_world_hud.active) return;
    vr_hud_move_rscale(&vr_world_hud.move, rscale);
}

// --------------------------------------------------------------------------- VR eye images: where a HUD draw goes
// D057, D072, D073, D088, D097: the route of a draw vr_is_hud_draw sent to the HUD's image (vr_draw_route.h: the
// routes, what the player sees, the precedence). Per HUD draw, from draw_ before its pass begins. The hub: gathers the
// draw's tags (its marker value, its pane's role, its screen's kind only when the route reads it) and the frame's
// finding, asks the route, tries the placements and asks the backdrop's fate when the route so far needs them, in that
// order, and applies the answer: vr_draw_to_hud_image, vr_world_hud.active, the backdrop flags, the counts of
// left-out draws, the pane log. Returns the route; draw_ counts a left-out draw as skipped and returns.
static VrDrawRoute vr_route_hud_draw(Prog *program) {
    const bool over_held_scene = vr_eyes.frame.over_world;
    const VrDrawLink link = vr_draw_link(vr_draw_marker);
    const PcVrScreenKind screen_kind = vr_draw_route_reads_screen(over_held_scene, link, vr_draw_role) ?
        pc_vr_screen_kind(vr_draw_screen) : PC_VR_SCREEN_NONE;
    VrDrawRoute route = vr_draw_route_ask(&(VrDrawTags){over_held_scene, link, vr_draw_role, screen_kind});
    if (route == VR_DRAW_LEFT_OUT_HELD_WORLD) {
        vr_draw_to_hud_image = false;
        ++vr_frame_world_left_out;  // (for SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS' log line)
        // [pc-vr-audit] SAVAGEWIND_LOG_PANES: a draw without a marker link left out over the held scene, by its
        // reticle's role or by its world screen (linked draws carry no pane number and are not logged)
        if (vr_draw_pane)
            pc_vr_pane_log_kind(vr_draw_pane, vr_draw_route_log_kind(route), vr_draw_route_log_name(route));
        return route;
    }
    if (route == VR_DRAW_LEFT_OUT_UNLINKED_WORLD) {
        vr_draw_to_hud_image = false;
        ++vr_unlinked_world.frame_draws;  // (the guard's log: vr_unlinked_world_present)
        if (vr_draw_pane)  // [pc-vr-audit]
            pc_vr_pane_log_kind(vr_draw_pane, vr_draw_route_log_kind(route), vr_draw_route_log_name(route));
        return route;
    }
    // Outside a held scene: into its eye's half when a placement takes it, the fade's first, then the marker's (each
    // counts and logs its own, and sets the move; over the held scene the eye images are not shown).
    const bool fade_placed = route == VR_DRAW_EYE_HALF && vr_hud_fade(program);
    const bool marker_placed = !fade_placed && route == VR_DRAW_EYE_HALF && vr_world_hud_draw();
    route = vr_draw_route_placed(route, fade_placed, marker_placed);
    vr_world_hud.active = vr_draw_route_moved(route);
    if (vr_draw_pane)  // [pc-vr-audit] SAVAGEWIND_LOG_PANES
        pc_vr_pane_log_kind(vr_draw_pane, vr_draw_route_log_kind(route), vr_draw_route_log_name(route));
    if (vr_world_hud.active) {
        vr_draw_to_hud_image = false;  // (D073: into its eye's half, at its marker's point; or a fade)
        return route;
    }
    // The HUD's image, by the backdrop's fate.
    route = vr_draw_route_backdrop(route, vr_hud_backdrop_fate(program));
    if (vr_draw_pane && route != VR_DRAW_HUD)  // [pc-vr-audit] SAVAGEWIND_LOG_PANES
        pc_vr_pane_log_kind(vr_draw_pane, vr_draw_route_log_kind(route), vr_draw_route_log_name(route));
    if (route == VR_DRAW_LEFT_OUT_BACKDROP) {  // (the game's picture of the world: left out)
        vr_draw_to_hud_image = false;
        return route;
    }
    vr_backdrop_pictures_black = vr_draw_route_over_black(route);  // (a fade's alpha, its ramp's, over opaque black)
    vr_backdrop_erase = vr_draw_route_erases(route);
    return route;
}

// ----------------------------------------------------------------------------- VR eye images: offering the frame to XR
// Called from backend_present, under gpu_lock, after the present's submission (so this frame's draws come before
// it in queue order). Takes this frame's findings, resets them for the next frame, classifies the frame
// (vr_frame_classify, vr_frame.h) and offers the filled slot to the XR owner thread as one of:
//   scene             both eyes certified by one camera traversal and each placed in its own half: the eye images,
//                     with the pose they were rendered for;
//   scene-hud         the same, with the HUD's image on its panel over them (D057);
//   menu-panel        no scene: the left half on the menu panel, anchored at the calibration (D056);
//   quick-over-scene  a quick menu over the held scene: the HUD's image on its panel over the last scene pair (D072).
// Anything else (an uncertified or misplaced scene, no pose for a menu) is not offered, and the slot is released.
// The producer's part is vr_eyes.c's (vr_eyes_present); around it this file resets its own per-frame VR state and
// counts the presents over the held scene for the quick menu shots (vr_shots.c). None of this file's resets touches the
// producer's state or submits, so they may come before the producer's call.
void vk_vr_present(uint64_t game_present_id) {
    pc_vr_screens_log();
    vr_dlaa_present();  // (a DLAA mode the '9' key asked for, from the next frame: vr_dlaa.h)
    const bool over_world = vr_eyes.frame.over_world;  // (this frame's, before vr_eyes_present resets the findings)
    vr_frame_world_left_out = 0;
    if (game_present_id) vr_unlinked_world_present(game_present_id, over_world);
    vr_scene_depth_reset(&vr_scene_depth);
    vr_world_hud.frame_draws = 0;
    // (SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS: the game frames over the held scene in a row, vr_quick_menu_shot)
    if (vr_eyes_present(game_present_id, submit_seq)) vr_quick_menu_shots_present(over_world);
    // (the late-frame trace: the pair is offered; this thread's counts so far, of which the frame's part is kept)
    pc_vr_frame_trace_offered(game_present_id, cum_draws, cum_submits, (uint64_t)atomic_load(&stat_compiles), cum_wait,
                              stat_upload_bytes, atomic_load(&g_vram_alloc_ns));
}
