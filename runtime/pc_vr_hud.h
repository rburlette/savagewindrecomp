// PC VR world-tied HUD (D073, D088): where the game places HUD pieces over the world, so the renderer
// (runtime/nvn/vulkan/exec.c) can draw each one at its 3D point in both eyes instead of on the HUD's flat panel.
// How it works: runtime/pc_vr_hud.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

// One marker placed by the HUD's world-to-screen helper (or one speech balloon).
typedef struct {
    // Its position in the HUD layout: units of the 1280x720 layout, y up. (Labo's HUD layer later scales the whole
    // layout: VR_HUD_LAYER_X/Y, runtime/nvn/vulkan/vr_world_hud.h.)
    float layout[2];
    // The world point it marks, in the UI camera's space: game units, x right, y up, -z ahead.
    float camera[3];
    // The horizontal offset the helper added to that point's projection, in NDC of the UI camera's view (the stamina
    // wheel sits beside Link).
    float offset;
    // renderer_present_count() when it was placed.
    uint64_t frame;
    // This report's sequence number, never 0. A draw linked to the marker by its pane carries it.
    uint32_t seq;
} PcVrHudPoint;

// Called by patched game code on the game thread (tools/game_sites.py pcvr_hud_point, tools/patches.py _hud_point)
// after the helper ran: layout and camera are guest addresses of the helper's position and camera-space point.
void rt_hud_point(Cpu *cpu, uint64_t layout, uint64_t camera);

// D088: a speech balloon's placement, reported like the helper's (tools/patches.py _balloon_point). It also binds the
// balloon's pane to that report (below).
void rt_balloon_point(Cpu *cpu);

// The game's stores of the helper's result into a piece's pane (runtime/pc_vr_hud.c, the marker links). Patched game
// code (tools/game_sites.py pcvr_marker_*, tools/patches.py _marker_*) calls these on the game thread: pane or
// component (whose pane is [[component + 8] + 0x18]) a guest address, source the guest address of the two floats the
// store reads, site the store's number (1 to 11). A store binds the pane to the helper's last report on that thread
// when the helper wrote it to source; otherwise the pane's world point is unknown. A release (the piece is given a
// fixed HUD place) makes the pane flat. These bindings decide each draw's marker (pc_vr_marker_drawing).
void rt_marker_pane_bind(uint64_t pane, uint64_t source, uint32_t site);
void rt_marker_component_bind(uint64_t component, uint64_t source, uint32_t site);
void rt_marker_component_release(uint64_t component, uint32_t site);

// The marker report numbered `seq`: from the ring, or else the copy a pane's binding keeps (its report and the one
// before it), however old. False when neither has it (or seq is not a report's number).
bool pc_vr_hud_point_by_seq(uint32_t seq, PcVrHudPoint *out);

// D088: the UI pane being drawn on this thread. Patched game code (tools/patches.py _pane_draw) calls begin around a
// pane's DrawSelf, and around a child's whole Draw (a capture pane overrides Draw). begin returns the pane drawn
// before, which end restores.
uint64_t rt_pane_draw_begin(uint64_t pane);
void rt_pane_draw_end(uint64_t previous);

// For a draw being recorded in a screen (screen_tag), by the binding of the nearest pane up its parents (the pane being
// drawn and 15 parents) that has one: the sequence number of the report bound to it (the game stored that report's
// position into it), PC_VR_MARKER_FLAT when the game gave the piece a fixed HUD place, 0 when the game moved it without
// a known world point (a store that could not bind) or none of those panes has a binding. No age limits a binding: the
// game draws a piece where it last put it.
#define PC_VR_MARKER_FLAT 0x7fffffffu     // drawn flat, on the HUD's panel: where the game itself placed it
#define PC_VR_MARKER_SEQ_MAX 0x7ffffffeu  // reports are numbered 1 to this, then from 1 again
uint32_t pc_vr_marker_drawing(uint32_t screen_tag);

// D088: the UI screen each draw comes from. Patched game code reports each screen as its screen loop draws it, on
// the drawing thread (tools/patches.py _screen_draw). begin returns the screen drawn before, which end restores. When a
// world screen (MainScreen3D_00, Message3D_00) comes with another object than before, every pane binding is cleared:
// a pane does not outlive its screen.
uint32_t rt_screen_draw_begin(uint64_t screen, uint32_t id);
void rt_screen_draw_end(uint32_t previous);

// The screen being drawn on this thread, as the tag a draw carries: its id + 1. 0 outside a screen's draw, or
// outside PC VR.
uint32_t pc_vr_screen_drawing(void);

// What kind of screen a tagged draw came from, by the screen's layout name (pc_vr_hud.c):
//   PC_VR_SCREEN_WORLD  a screen whose pieces the game ties to points in the world: MainScreen3D_00 (enemy marks and
//                       health bars, the Z-target cursor, item names, the stamina wheel) and Message3D_00 (speech
//                       balloons). Its draws are left out (exec.c draw_) over a held scene, linked or not (the
//                       held eye images already show those pieces), and anywhere when their marker is 0 (their world
//                       point is not known: flat on the panel they would be at the wrong depth). A draw whose pane
//                       the game gave a fixed HUD place (PC_VR_MARKER_FLAT) is drawn flat;
//   PC_VR_SCREEN_HUD    the flat HUD (MainScreen_00), with the aiming reticles;
//   PC_VR_SCREEN_FLAT   any other screen.
typedef enum { PC_VR_SCREEN_NONE, PC_VR_SCREEN_FLAT, PC_VR_SCREEN_HUD, PC_VR_SCREEN_WORLD } PcVrScreenKind;
PcVrScreenKind pc_vr_screen_kind(uint32_t tag);

// The tagged screen's layout name, or NULL (for logs).
const char *pc_vr_screen_name(uint32_t tag);

// Whether a shop's list is up (the screen ShopBG_00, from opening to closing). The game then stops drawing the scene
// and shows its own picture of the world, as in a quick menu (exec.c vr_menu_over_world).
bool pc_vr_shop_open(void);

// SAVAGEWIND_LOG_SCREENS: logs each screen's state by name as it changes. Called once per presented frame.
void pc_vr_screens_log(void);

// The UI's screen parallax: patched game code (tools/game_sites.py pcvr_ui_parallax, tools/patches.py _ui_parallax)
// calls this with the amount the game is about to store in a view (a float's bits) and stores what it returns: zero
// in PC VR, whose UI is on a panel of its own, so the game's UI layout, texture crops and lens mask all work without
// Labo's shift toward the nose. SAVAGEWIND_PC_VR_UI_PARALLAX=1: the game's own amount.
uint32_t rt_ui_parallax(uint32_t amount_bits);
// Whether the game's UI is drawn without that parallax: the patch has run and stored zero. The screen then lies about
// the centre of each eye's half of the final image, not about Labo's lens centre.
bool pc_vr_ui_centred(void);

// D078: the rune target test (stasis, magnesis). Patched game code (tools/game_sites.py pcvr_rune_test,
// tools/patches.py _rune_test) calls this on the game thread each frame it sets the test up: constants is the guest
// address of the constants block the test's draw will bind, position the guest address of the screen position the
// game is about to write into it (two floats, 0 to 1).
void rt_rune_test(uint64_t constants, uint64_t position);

// Whether the constants block at this guest address is the rune target test's, set up in the last few presented
// frames. The renderer asks for each fragment constant block it binds (exec.c vr_rune_aim_patch).
bool pc_vr_rune_test_constants(uint64_t address);

// What a UI pane is to PC VR, by the game's own names for the pane and its screen (runtime/pc_vr_hud.c
// pc_vr_pane_role). Each draw the pane records carries it, so the renderer knows these draws by who drew them and
// not by their size or place.
typedef enum {
    PC_VR_PANE_NONE,           // an ordinary pane: on the HUD panel, or on its marker's point
    PC_VR_PANE_RETICLE,        // part of an aiming reticle (bow, rune, throw): on the aim ray, at the scene's depth
    PC_VR_PANE_FADE,           // the fade over the whole screen: over each eye's whole view
    PC_VR_PANE_WORLD_PICTURE,  // the game's picture of the world behind a menu: left out, the world itself shows
    PC_VR_PANE_EDGE_MASK,      // fades a list's end into that picture: erases the HUD under it instead
    PC_VR_PANE_BOX,            // a box that shows the screen behind it, blurred: black in the picture's place
} PcVrPaneRole;

// The role of the pane being drawn on this thread, in the screen with this tag. Called for every recorded UI draw.
PcVrPaneRole pc_vr_pane_role(uint32_t screen_tag);

// [pc-vr-audit] SAVAGEWIND_LOG_PANES=1 (runtime/pc_vr_hud.c): the UI pane behind each draw the renderer classifies.
// A recorded draw without a marker link carries PC_VR_PANE_LOG_TAG | the pane's number (pc_vr_pane_log_drawing, at
// record time) in the marker's place; the renderer reports how it treated the draw (pc_vr_pane_log_kind).
#define PC_VR_PANE_LOG_TAG 0x80000000u
bool pc_vr_pane_log_on(void);
uint32_t pc_vr_pane_log_drawing(uint32_t screen_tag);
void pc_vr_pane_log_kind(uint32_t id, unsigned kind, const char *kind_name);
const char *pc_vr_pane_log_name(uint32_t id);  // the numbered pane's name, "-" when there is none

#ifdef __cplusplus
}
#endif
