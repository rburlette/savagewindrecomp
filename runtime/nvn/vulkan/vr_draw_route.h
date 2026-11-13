// PC VR renderer: where one 2D draw into the final image goes (D057, D072, D073, D087, D088, D091, D097, D099).
// Header-only: exec.c's draw_ asks it for every HUD draw, on the per-draw path, in its own translation unit.
//
// Responsible for: the per-draw route, the second of the PC VR code's two results (the first is the frame's kind at
// the present, vr_frame.h): for each draw a game UI screen records into the final image, whether it goes to the HUD's
// own image (as drawn, over black, or erasing), into its eye's half of the final image (a marker or reticle placed at
// its 3D point, or the fade over the whole view), or is left out, and why. Also the decision whether a frame without a
// scene is a quick menu's or a shop's over the held scene (vr_frame_over_held_scene), which decides whether the
// frame's 2D draws are HUD draws at all. Kept beside the route rather than in vr_frame.h: both are taken while the
// frame's draws execute, from the draws' tags and the frame so far, and feed the per-draw path; vr_frame.h classifies
// the finished frame at its present.
//
// Pure: plain values in, plain values out, so a fixture (tools/test_vr_draw_route.c) compares it with the code it came
// from on every combination of its inputs.
//
// Not owned here:
//  - the gate (exec.c vr_is_hud_draw), which runs for every draw and stays in exec.c as it was: a draw is a HUD draw
//    when the eye images are on, the HUD's image is usable, the draw's one colour target is the final image (known
//    as the eye placement's target, D092), the frame either placed its scene with the HUD panel on (the HUD setting)
//    or is over the held scene (vr_frame_over_held_scene's answer, latched), and a UI screen recorded the draw (its
//    screen tag, D088). Any other draw is VR_DRAW_FINAL_IMAGE without asking anything here;
//  - the placements (vr_world_hud.h: exec.c's vr_hud_fade and vr_world_hud_draw, which count, log and set the move)
//    and the backdrop's fate (exec.c vr_hud_backdrop_fate, which looks at the textures the draw samples);
//  - applying the route: exec.c's vr_route_hud_draw sets vr_draw_to_hud_image (begin_pass then opens a pass into the
//    HUD's image), vr_world_hud.active, the backdrop flags (vr_backdrop_pictures_black, read where textures are bound;
//    vr_backdrop_erase, read by vr_hud_blend), counts the left-out draws for the shots' log and the guard's log, and
//    logs the pane ([pc-vr-audit] SAVAGEWIND_LOG_PANES); draw_ counts a left-out draw as skipped and returns.
//
// The inputs, who sets them and when:
//  - over_held_scene: vr_eyes.frame.over_world. Latched at the frame's first possible HUD draw, while the frame has no
//    scene-final draw, placement or certificate yet, from vr_frame_over_held_scene (the held scene, the HUD setting,
//    the game's pause state, the shop's screen at that moment); cleared by a later scene-final draw (vr_after_draw: a
//    scene after all) and at each present.
//  - link: the draw's marker value, carried in its record (vr_draw_marker; runtime/pc_vr_hud.c pc_vr_marker_drawing at
//    record time, D088, D099): none, the game's own fixed HUD place (PC_VR_MARKER_FLAT), or a marker report.
//  - role: what the pane that recorded the draw is, by the game's own names (vr_draw_role; pc_vr_pane_role at record
//    time, adopted with D091): a reticle, the fade, a backdrop, or ordinary.
//  - screen_kind: the recording screen's kind by its layout name (pc_vr_screen_kind, D087, D097). Read at most once
//    per draw, and only when vr_draw_route_reads_screen says the route needs it (its first reading of a screen is
//    logged); otherwise passed as PC_VR_SCREEN_NONE, which the route then does not look at. The code this came from
//    could read it twice in one draw (once for each left-out test). pc_vr_screen_kind caches a screen's kind once its
//    name reads, but answers FLAT without caching while the name reads NULL, so those two reads could in principle
//    disagree if the game filled the name in between them; the single read gives the draw one consistent answer.
//  - the placements' answers (vr_draw_route_placed) and the backdrop's fate (vr_draw_route_backdrop), asked by exec.c
//    in that order and only when the route so far needs them.
//
// The order of precedence, in words:
//  1. A draw that is not a HUD draw goes to the final image as the game draws it.
//  2. Over the held scene, a draw with any marker value is left out: a piece tied to a report's world point, and also
//     a piece the game gave a fixed HUD place (PC_VR_MARKER_FLAT), which outside a held scene goes flat on the panel.
//     So are a reticle's pane and any draw of a world screen. The held eye images already show the pieces that were
//     placed at their world points.
//  3. Anywhere, a world screen's draw without a marker value is left out: its world point is not known (the guard).
//  4. Outside a held scene, the fade's placement is tried, then the marker's; a placed draw goes into its eye's half.
//  5. Everything else goes to the HUD's image, as the backdrop's fate says: as drawn, over black, erasing what is
//     under it, or (the game's picture of the world) left out.
//
// Rules by shape inside this decision, kept exactly and named for their replacement: the backdrop's fate applies only
// to a draw that samples a render target or a display image (exec.c vr_backdrop_source: the texture's flags & 9, the
// texture flag pattern of backlog item 11). The placements' own rules by shape are listed in vr_world_hud.h (item 11).
// The held scene is known from what the frame has drawn so far and the game's pause state (decided once, at the
// frame's first possible HUD draw).
#ifndef SAVAGEWIND_VR_DRAW_ROUTE_H
#define SAVAGEWIND_VR_DRAW_ROUTE_H

#include <stdbool.h>
#include <stdint.h>

#include "../../pc_vr_hud.h"

// Where a 2D draw into the final image goes, and what the player sees.
typedef enum {
    // Not a HUD draw (exec.c vr_is_hud_draw): drawn where the game draws it. In a scene frame: the scene and the game's
    // own composites; in a frame without a scene and not over the held scene: a menu page, shown on the menu panel
    // (D056, vr_frame.h).
    VR_DRAW_FINAL_IMAGE,
    // On the HUD's see-through panel at the game's forward, as the game draws it (D057): the flat HUD (the hearts,
    // the action prompt); outside a held scene, a piece the game gave a fixed HUD place (PC_VR_MARKER_FLAT) and a
    // piece tied to a world point that no placement took; over the held scene, the quick menu or the shop (D072).
    VR_DRAW_HUD,
    // On the HUD's panel, a box that shows the screen behind it (its pane's role, adopted with D091, together with the
    // texture test; D073 d838a01): its own shading over opaque black in place of the screen, so no flat picture of the
    // world floats on the panel.
    VR_DRAW_HUD_OVER_BLACK,
    // On the HUD's panel, a fade into the screen behind (the ends of the quick menu's item row, by their pane's role
    // together with the texture test; D072 a42f436): over black, and it erases the HUD under it by its alpha, so the
    // items fade into the world seen through the panel.
    VR_DRAW_HUD_ERASE,
    // Asked, not final: into its eye's half if a placement takes it (vr_draw_route_placed), else the HUD's image.
    VR_DRAW_EYE_HALF,
    // Into its own eye's half of the final image at its 3D point (D073, D088, D094): an enemy mark or health bar, the
    // Z-target cursor, an item name, the stamina wheel, a speech balloon, or an aiming reticle on the aim ray. Each
    // half goes to its own eye, so the player sees it at its point's depth.
    VR_DRAW_EYE_HALF_MARKER,
    // The fade over the whole layout (the black after a load), over each eye's whole view (D073).
    VR_DRAW_EYE_HALF_FADE,
    // Left out over the held scene (a quick menu, a shop): a piece tied to the world, which the held eye images, the
    // last scene frame's, already show (D097). The game goes on drawing those pieces; they used to land on the HUD's
    // panel as a second, flat copy (user, 2026-11-10 and 11-11). A world screen's draws carry no marker link there
    // from the hold's first presents (2026-11-11, pane log), so they are left out by their screen as well as by their
    // link or their reticle's role. Counted for the quick menu shots' log line.
    VR_DRAW_LEFT_OUT_HELD_WORLD,
    // Left out anywhere: a world screen's draw without a marker value (no pane up its parents has a binding, or the
    // game moved the piece without a known world point). Its world point is not known, and on the panel it would be a
    // flat copy at the wrong depth (the speech balloon flashed flat as a quick menu closed; user, 2026-11-11): the
    // guard 2707bb8 (D097), kept as a logged safety net by D099. A piece at the game's own fixed HUD place is drawn
    // flat as any HUD draw and not counted. Counted for the guard's log (exec.c vr_unlinked_world_present, by runs of
    // presents).
    VR_DRAW_LEFT_OUT_UNLINKED_WORLD,
    // Left out: the game's picture of the world behind a menu (D072), known by its pane's role together with the
    // texture test (the draw samples a render target or a display image: the rule by shape named above). The world
    // itself shows there instead: over the held scene the held eye images, in a scene frame the live scene.
    VR_DRAW_LEFT_OUT_BACKDROP,
} VrDrawRoute;

// The draw's marker value (vr_draw_marker).
typedef enum {
    VR_DRAW_LINK_NONE,    // 0: no pane up its parents has a binding, or the game moved it without a known world point
    VR_DRAW_LINK_FLAT,    // PC_VR_MARKER_FLAT: the game gave the piece a fixed HUD place
    VR_DRAW_LINK_REPORT,  // a marker report's number: the piece is tied to that report's world point
} VrDrawLink;

static inline VrDrawLink vr_draw_link(uint32_t marker) {
    return !marker ? VR_DRAW_LINK_NONE : marker == PC_VR_MARKER_FLAT ? VR_DRAW_LINK_FLAT : VR_DRAW_LINK_REPORT;
}

// What the backdrop's fate says about a draw to the HUD's image (exec.c vr_hud_backdrop_fate; the numbers are its own).
typedef enum {
    VR_BACKDROP_NONE = 0,     // not a backdrop's draw, or SAVAGEWIND_PC_VR_HUD_BACKDROP=1: as drawn
    VR_BACKDROP_BLACK = 1,    // a box: over black
    VR_BACKDROP_LEFT_OUT = 2, // the game's picture of the world: left out
    VR_BACKDROP_ERASE = 3,    // a fade into the screen behind: erasing
} VrBackdropFate;

// The fate a backdrop pane's role asks for (adopted with D091), before the switch and the texture test.
static inline VrBackdropFate vr_backdrop_fate_of_role(PcVrPaneRole role) {
    return role == PC_VR_PANE_BOX ? VR_BACKDROP_BLACK : role == PC_VR_PANE_WORLD_PICTURE ? VR_BACKDROP_LEFT_OUT :
        role == PC_VR_PANE_EDGE_MASK ? VR_BACKDROP_ERASE : VR_BACKDROP_NONE;
}

// A HUD draw's tags and the frame's finding the route reads.
typedef struct {
    bool over_held_scene;        // vr_eyes.frame.over_world
    VrDrawLink link;             // vr_draw_link(vr_draw_marker)
    PcVrPaneRole role;           // vr_draw_role
    PcVrScreenKind screen_kind;  // pc_vr_screen_kind(vr_draw_screen) when vr_draw_route_reads_screen, else NONE
} VrDrawTags;

// Whether the route reads the screen's kind: for a draw without a marker value, unless it is a reticle's over the held
// scene (left out by its role first).
static inline bool vr_draw_route_reads_screen(bool over_held_scene, VrDrawLink link, PcVrPaneRole role) {
    return link == VR_DRAW_LINK_NONE && !(over_held_scene && role == PC_VR_PANE_RETICLE);
}

// The route a HUD draw asks for (steps 2 to 4 of the precedence above): left out over the held scene, left out by the
// guard, into its eye's half if a placement takes it (VR_DRAW_EYE_HALF: outside a held scene), or the HUD's image
// (VR_DRAW_HUD: over the held scene, where nothing is placed).
static inline VrDrawRoute vr_draw_route_ask(const VrDrawTags *tags) {
    if (tags->over_held_scene && (tags->link != VR_DRAW_LINK_NONE || tags->role == PC_VR_PANE_RETICLE ||
                                  tags->screen_kind == PC_VR_SCREEN_WORLD))
        return VR_DRAW_LEFT_OUT_HELD_WORLD;
    if (tags->link == VR_DRAW_LINK_NONE && tags->screen_kind == PC_VR_SCREEN_WORLD) return VR_DRAW_LEFT_OUT_UNLINKED_WORLD;
    return tags->over_held_scene ? VR_DRAW_HUD : VR_DRAW_EYE_HALF;
}

// What a route becomes after the placements (step 4): from VR_DRAW_EYE_HALF, the fade's eye half when the fade's
// placement took it, the marker's when the marker's did (exec.c tries the marker's only when the fade's was refused),
// else the HUD's image. Any other route is unchanged.
static inline VrDrawRoute vr_draw_route_placed(VrDrawRoute route, bool fade_placed, bool marker_placed) {
    if (route != VR_DRAW_EYE_HALF) return route;
    return fade_placed ? VR_DRAW_EYE_HALF_FADE : marker_placed ? VR_DRAW_EYE_HALF_MARKER : VR_DRAW_HUD;
}

// What a route to the HUD's image becomes by the backdrop's fate (step 5). Any other route is unchanged.
static inline VrDrawRoute vr_draw_route_backdrop(VrDrawRoute route, VrBackdropFate fate) {
    if (route != VR_DRAW_HUD) return route;
    return fate == VR_BACKDROP_BLACK ? VR_DRAW_HUD_OVER_BLACK : fate == VR_BACKDROP_LEFT_OUT ? VR_DRAW_LEFT_OUT_BACKDROP :
        fate == VR_BACKDROP_ERASE ? VR_DRAW_HUD_ERASE : VR_DRAW_HUD;
}

// The draw is not executed (draw_ counts it skipped).
static inline bool vr_draw_route_left_out(VrDrawRoute route) {
    return route == VR_DRAW_LEFT_OUT_HELD_WORLD || route == VR_DRAW_LEFT_OUT_UNLINKED_WORLD ||
        route == VR_DRAW_LEFT_OUT_BACKDROP;
}

// The draw is moved into its eye's half (vr_world_hud.active).
static inline bool vr_draw_route_moved(VrDrawRoute route) {
    return route == VR_DRAW_EYE_HALF_FADE || route == VR_DRAW_EYE_HALF_MARKER;
}

// The draw goes to the HUD's image (vr_draw_to_hud_image), and how: over black (vr_backdrop_pictures_black), erasing
// (vr_backdrop_erase).
static inline bool vr_draw_route_to_hud_image(VrDrawRoute route) {
    return route == VR_DRAW_HUD || route == VR_DRAW_HUD_OVER_BLACK || route == VR_DRAW_HUD_ERASE;
}
static inline bool vr_draw_route_over_black(VrDrawRoute route) {
    return route == VR_DRAW_HUD_OVER_BLACK || route == VR_DRAW_HUD_ERASE;
}
static inline bool vr_draw_route_erases(VrDrawRoute route) { return route == VR_DRAW_HUD_ERASE; }

// [pc-vr-audit] SAVAGEWIND_LOG_PANES (runtime/pc_vr_hud.c pc_vr_pane_log_kind): the kind a route is logged as, and its
// name. Keep these numbers and strings: the pane log keeps one bit per kind, and the play script's summaries read the
// names. ("crosshair" is a placed marker of any kind, for the log's history.)
static inline unsigned vr_draw_route_log_kind(VrDrawRoute route) {
    switch (route) {
    case VR_DRAW_EYE_HALF_FADE: return 1;
    case VR_DRAW_EYE_HALF_MARKER: return 2;
    case VR_DRAW_HUD_OVER_BLACK: return 3;
    case VR_DRAW_LEFT_OUT_BACKDROP: return 4;
    case VR_DRAW_HUD_ERASE: return 5;
    case VR_DRAW_LEFT_OUT_HELD_WORLD: return 6;
    case VR_DRAW_LEFT_OUT_UNLINKED_WORLD: return 7;
    default: return 0;  // (the HUD's image as drawn)
    }
}
static inline const char *vr_draw_route_log_name(VrDrawRoute route) {
    switch (route) {
    case VR_DRAW_EYE_HALF_FADE: return "fade";
    case VR_DRAW_EYE_HALF_MARKER: return "crosshair";
    case VR_DRAW_HUD_OVER_BLACK: return "backdrop-black";
    case VR_DRAW_LEFT_OUT_BACKDROP: return "backdrop-left-out";
    case VR_DRAW_HUD_ERASE: return "backdrop-erase";
    case VR_DRAW_LEFT_OUT_HELD_WORLD: return "held-world-left-out";
    case VR_DRAW_LEFT_OUT_UNLINKED_WORLD: return "world-unlinked-left-out";
    default: return "hud";
    }
}

// D072: whether a frame is a quick menu's or a shop's over the held scene, so that its 2D draws go to the HUD's image
// over the last scene pair instead of becoming a menu panel. The frame must not have drawn a scene so far
// (scene_unseen: no scene-final draw, placement or certificate yet); a scene must be held (an accepted scene pair: a
// menu panel pair replaces the eyes' images); the HUD panel must be on (the HUD setting); and the game must be in a
// quick menu (its pause state 4, hle/framepace.c) or a shop's list must be up (its screen ShopBG_00, which leaves the
// pause state alone). The pause menu's pages stay menu panels (D056, D068). exec.c's vr_quick_menu_over_world asks
// once a frame, at its first possible HUD draw, and latches the answer (vr_eyes.frame.over_world). It reads each input
// only while the ones before it leave the answer open, in this order (the setting takes a lock, the shop's screen
// walks the screen list), as the expression this came from did; an input not read is passed as false.
static inline bool vr_frame_over_held_scene(bool scene_unseen, bool scene_held, bool hud_panel_on, bool quick_menu,
                                            bool shop_open) {
    return scene_unseen && scene_held && hud_panel_on && (quick_menu || shop_open);
}

#endif
