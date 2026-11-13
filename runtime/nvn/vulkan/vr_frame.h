// PC VR renderer: what one game frame turned out to be, for the headset (D048, D056, D057, D072).
//
// Responsible for: the facts the PC VR code in exec.c gathers while it executes one frame's draws (VrFrameFindings),
// and the decision, at the frame's present, of what the frame offers the XR owner (vr_frame_classify): the two eye
// images as a scene, the scene with the HUD on its panel, the HUD over the held scene (a quick menu or a shop), a menu
// panel, or nothing, with the reason. Pure: plain values in, plain values out, so a fixture (tools/test_vr_frame.c)
// pins every outcome without the renderer.
//
// Not owned here:
//  - setting the findings: exec.c's draw hooks do it (vr_after_draw for the eye layers' draws, vr_quick_menu_over_world
//    for the held scene), and say which draw sets which field below;
//  - the images: the slot being filled (vr_eyes.filling), the GPU submission that completes its copies
//    (vr_eyes.ready_seq) and the slots themselves stay in exec.c beside the code that hands them to the XR owner;
//  - state that outlives a frame: the held scene's generation (vr_eyes.scene_held), the eased HUD distance, the
//    settings and the pose query (exec.c reads them and passes the values in VrFrameInputs);
//  - building the panels (exec.c vr_panel) and the offer itself (xr_runtime_offer_eye_pair, runtime/xr/xr_vulkan.h).
//    The XR owner can still refuse a ready offer; that refusal is its answer, not a classification.
//
// How exec.c uses it at each present (vk_vr_present): copy the findings and reset them for the next frame
// (vr_frame_findings_reset); stop early for a frame that has nothing to offer (vr_frame_not_offered); gather the
// remaining inputs; classify; build the panel the kind needs, and where it cannot be built take vr_frame_without_panel's
// answer instead; offer. The two steps are separate because a panel can fail to build after the kind is known, and
// because the eased HUD distance changes only on the scene path, after its kind is decided and before its panel.
//
// Conventions:
//  - Eyes are numbered as the render certificates' native_index: 0 the left eye, 1 the right. A per-eye bit set holds
//    1u << eye, so both eyes is 3.
//  - The final image is the game's final composite: the left eye's scene in its left half, the right eye's in its
//    right half, then the HUD and menus drawn over them (before the Labo lens pass, which PC VR replaces). Its size
//    follows the window (exec.c's vr_capture_eyes reallocates its copies on a resize).
//  - A frame runs from one present (vk_vr_present) to the next; a UI re-presentation of the window has
//    game_present_id 0 and is not a game frame.
#ifndef SAVAGEWIND_VR_FRAME_H
#define SAVAGEWIND_VR_FRAME_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../../hle/pc_vr_render_trace.h"

// What executing one frame's draws established about its eye images and its HUD. Every field starts at zero at each
// present and at the game reset (vr_frame_findings_reset); the draws of the frame set them in the order they come.
typedef struct {
    // An eye's scene-final draw happened (the draw tagged PC_VR_LAYER_DRAW_FINAL of an eye layer, or of a layer that
    // may be an eye's after a load), whether or not it carried a certificate. Set by vr_after_draw. The game drew a
    // scene this frame, so the frame is offered as a scene or not at all: never as a menu panel.
    bool scene_seen;
    // Per-eye bits: that eye's scene-final draw carried its own render certificate, which resolved to that eye. Set by
    // vr_after_draw at the scene-final draw. Where a bit is set, eyes[] and scene_output[] hold this frame's values
    // for that eye; elsewhere they hold nothing.
    unsigned certified;
    // Per-eye bits: a certified eye's placement draw (PC_VR_LAYER_DRAW_PLACEMENT) drew that eye's own finished image
    // (scene_output) into that eye's half of the final image. Set by vr_after_draw. With both bits and both
    // certificates agreeing, the final image's halves are this frame's two eyes, each in its place.
    unsigned placed;
    // A certified eye's placement draw drew another image, or drew into the other eye's half. Set by vr_after_draw.
    // The halves cannot be trusted to be the eyes: the frame is rejected as misplaced.
    bool misplaced;
    // An eye's placement draw happened, certified or not (vr_after_draw). The scene is in the final image, so the
    // game's later 2D draws into it are its HUD (D057): exec.c's vr_is_hud_draw sends them to the HUD's own image.
    bool placement_seen;
    // A quick menu or a shop is open over the held scene (D072). Latched by vr_quick_menu_over_world at the frame's
    // first possible HUD draw, while no scene-final draw, placement or certificate has been seen yet, from the pause
    // state and the shop's screen at that moment; cleared again by a later scene-final draw (a scene after all). While
    // set, the frame's 2D content goes to the HUD's image, pieces tied to the world are left out (the held eye images
    // already show them), and the frame asks to be shown as the HUD over the held scene.
    bool over_world;
    // over_world has been decided this frame (it is decided once, at the first draw that asks).
    bool over_world_known;
    // Each certified eye's render certificate, from its scene-final draw (vr_after_draw): which camera traversal,
    // tracking frame and generation the eye was rendered for, the exact pose snapshot to submit it with, and the eye
    // projection's depth terms and world scale (the crosshair's depth and the HUD markers use them). Valid only where
    // certified has the eye's bit.
    PcVrRenderCertificate eyes[2];
    // Each certified eye's finished image (its scene-final draw's render target, by debug id), so that its placement
    // draw can be checked to sample this very image. Valid only where certified has the eye's bit.
    uint64_t scene_output[2];
} VrFrameFindings;

// Back to the state before any draw of a frame: every byte zero, as the zero-initialised vr_eyes starts. Called at each
// present after the findings were copied for classification, and at the game reset.
static inline void vr_frame_findings_reset(VrFrameFindings *findings) {
    memset(findings, 0, sizeof *findings);
}

// What a frame offers the XR owner.
typedef enum {
    VR_FRAME_SCENE,                // the two eye images on the projection layer, with the pose they were rendered for
    VR_FRAME_SCENE_WITH_HUD,       // the same, with the HUD's own image on its panel over them (D057)
    VR_FRAME_HUD_OVER_HELD_SCENE,  // a quick menu or shop: the HUD's image on its panel over the last scene pair (D072)
    VR_FRAME_MENU_PANEL,           // no scene: the left half on the menu panel, at the calibration pose (D053, D056)
    VR_FRAME_REJECTED,             // nothing is offered; VrFrameRejection says why
} VrFrameKind;

// Why a frame offers nothing.
typedef enum {
    VR_FRAME_NOT_REJECTED,
    // The eye-image producer is off, or no slot was filled this frame (no lens pass, or every slot busy): there is
    // nothing to offer and nothing to hand back.
    VR_FRAME_REJECTED_NOT_CAPTURED,
    // A UI re-presentation of the window, not a game frame: the filled slot is handed back unoffered.
    VR_FRAME_REJECTED_NOT_A_GAME_FRAME,
    // A scene was drawn but the eyes do not certify one rendering: an eye without a certificate, or certificates of
    // different traversals, generations, frames or pose snapshots.
    VR_FRAME_REJECTED_UNCERTIFIED,
    // A scene was drawn and certified, but an eye's image was not found placed in its own half.
    VR_FRAME_REJECTED_MISPLACED,
    // No scene, and no pose to anchor a menu panel: the pose query failed, its eyes lacked native views, or no
    // calibration anchor could be made from it.
    VR_FRAME_REJECTED_MENU_WITHOUT_POSE,
    // No scene, and the menu panel could not be built (its distance setting is 0, the images have no size yet, or the
    // calibration gives no panel pose).
    VR_FRAME_REJECTED_MENU_WITHOUT_PANEL,
} VrFrameRejection;

// A frame's classification: its kind, and for VR_FRAME_REJECTED the reason (VR_FRAME_NOT_REJECTED otherwise).
typedef struct {
    VrFrameKind kind;
    VrFrameRejection rejection;
} VrFrameOutcome;

// Everything the classification depends on, as plain values. exec.c fills them at the present; the pose fields only
// for a frame without a scene (the pose is not queried for a scene frame).
typedef struct {
    const VrFrameFindings *findings;  // this frame's findings, copied before the reset
    bool producer_armed;              // the eye-image producer is configured and on (vr_eyes.state > 0)
    bool slot_filled;                 // a slot was claimed and filled at this frame's lens pass (vr_eyes.filling)
    bool slot_has_hud;                // that slot also holds this frame's HUD image (the HUD was drawn this frame)
    uint64_t game_present_id;         // the present's game frame number; 0 for a UI re-presentation
    // The tracking generation of the last accepted scene pair, which the XR owner keeps showing while no scene is
    // drawn; 0 when a menu panel pair has replaced it, or after the game reset. A quick menu's HUD goes over it only
    // in that same generation (a recenter or tracking loss in between ends the hold).
    uint64_t scene_held;
    // The HUD panel's distance setting (pc_vr_panel_depth(PC_VR_PANEL_HUD), times the camera target's distance); 0:
    // the HUD panel is off, and the HUD stays in the eye images.
    double hud_panel_depth;
    // The HUD panel's eased distance in metres, as the last scene frame with a HUD left it (0: never placed). A quick
    // menu's HUD keeps that distance, so its panel stays where the HUD was while the menu opened over the scene.
    double hud_distance_m;
    bool pose_valid;         // the pose query returned a valid pose (frames without a scene only)
    bool pose_native_views;  // with both eyes' native views, positions and orientations, tracked
    bool anchor_built;       // and a calibration anchor (xr_calibration_anchor) was made from it
    uint64_t anchor_generation;  // that anchor's tracking generation, compared with scene_held
} VrFrameInputs;

// The early part of the classification: whether the frame has anything to offer at all. exec.c calls it before its
// per-frame side effects (the held-scene counters, the submission of the copies), as it decided this before them.
static inline VrFrameRejection vr_frame_not_offered(const VrFrameInputs *inputs) {
    if (!inputs->producer_armed || !inputs->slot_filled) return VR_FRAME_REJECTED_NOT_CAPTURED;
    if (!inputs->game_present_id) return VR_FRAME_REJECTED_NOT_A_GAME_FRAME;
    return VR_FRAME_NOT_REJECTED;
}

// The guard for a scene pair: both eyes certified and each placed in its own half, nothing misplaced, and both
// certificates from one camera traversal, one tracking generation, one tracking frame and one pose snapshot (compared
// byte for byte), so that the pair is one rendering and the snapshot is the pose to submit it with.
static inline bool vr_frame_eyes_agree(const VrFrameFindings *findings) {
    const PcVrRenderCertificate *left = &findings->eyes[0], *right = &findings->eyes[1];
    return findings->certified == 3 && findings->placed == 3 && !findings->misplaced &&
        left->traversal == right->traversal && left->generation == right->generation && left->frame == right->frame &&
        !memcmp(&left->native_snapshot, &right->native_snapshot, sizeof left->native_snapshot);
}

// The outcome of a frame that offers kind (any kind but VR_FRAME_REJECTED).
static inline VrFrameOutcome vr_frame_offers(VrFrameKind kind) {
    return (VrFrameOutcome){kind, VR_FRAME_NOT_REJECTED};
}

// The outcome of a frame that offers nothing, for this reason.
static inline VrFrameOutcome vr_frame_rejects(VrFrameRejection rejection) {
    return (VrFrameOutcome){VR_FRAME_REJECTED, rejection};
}

// The kind the frame asks for, assuming any panel it needs can be built (see vr_frame_without_panel):
//  - nothing to offer (vr_frame_not_offered): rejected with that reason;
//  - a scene was drawn: a scene when the eyes agree (vr_frame_eyes_agree), with the HUD when the slot holds this
//    frame's HUD and the HUD panel is on; else misplaced when an eye was misplaced or both are certified but not both
//    placed; else uncertified;
//  - no scene: without an anchored pose, rejected (menu without a pose); with one, the HUD over the held scene when
//    a quick menu or shop is open over it (over_world), the slot holds its HUD, the HUD has been placed before, the
//    HUD panel is on and the held scene is of the anchor's generation; else the menu panel.
static inline VrFrameOutcome vr_frame_classify(const VrFrameInputs *inputs) {
    const VrFrameRejection not_offered = vr_frame_not_offered(inputs);
    if (not_offered != VR_FRAME_NOT_REJECTED) return vr_frame_rejects(not_offered);
    const VrFrameFindings *findings = inputs->findings;
    if (findings->scene_seen) {
        if (vr_frame_eyes_agree(findings))
            return vr_frame_offers(inputs->slot_has_hud && inputs->hud_panel_depth != 0 ? VR_FRAME_SCENE_WITH_HUD
                                                                                        : VR_FRAME_SCENE);
        if (findings->misplaced || (findings->certified == 3 && findings->placed != 3))
            return vr_frame_rejects(VR_FRAME_REJECTED_MISPLACED);
        return vr_frame_rejects(VR_FRAME_REJECTED_UNCERTIFIED);
    }
    if (!(inputs->pose_valid && inputs->pose_native_views && inputs->anchor_built))
        return vr_frame_rejects(VR_FRAME_REJECTED_MENU_WITHOUT_POSE);
    if (findings->over_world && inputs->slot_has_hud && inputs->hud_distance_m > 0 && inputs->hud_panel_depth != 0 &&
        inputs->scene_held == inputs->anchor_generation)
        return vr_frame_offers(VR_FRAME_HUD_OVER_HELD_SCENE);
    return vr_frame_offers(VR_FRAME_MENU_PANEL);
}

// What a frame becomes when the panel its kind needs cannot be built: the scene without its HUD panel (the HUD's
// image is then not shown); the HUD over the held scene falls back to the menu panel, whose own panel is then built;
// the menu panel becomes rejected (menu without a panel). Kinds that need no panel are returned as they are.
static inline VrFrameOutcome vr_frame_without_panel(VrFrameOutcome outcome) {
    switch (outcome.kind) {
    case VR_FRAME_SCENE_WITH_HUD: return vr_frame_offers(VR_FRAME_SCENE);
    case VR_FRAME_HUD_OVER_HELD_SCENE: return vr_frame_offers(VR_FRAME_MENU_PANEL);
    case VR_FRAME_MENU_PANEL: return vr_frame_rejects(VR_FRAME_REJECTED_MENU_WITHOUT_PANEL);
    case VR_FRAME_SCENE:
    case VR_FRAME_REJECTED: break;
    }
    return outcome;
}

// The outcome's name in the log ("[pc-vr-eyes] kind ..." and "offer=... kind=..."): keep these exact strings, which
// the evidence notes quote and log searches look for. The two outcomes that end before any offer is made are never
// logged; they have names for the fixture and for debugging.
static inline const char *vr_frame_kind_name(VrFrameOutcome outcome) {
    switch (outcome.kind) {
    case VR_FRAME_SCENE: return "scene";
    case VR_FRAME_SCENE_WITH_HUD: return "scene-hud";
    case VR_FRAME_HUD_OVER_HELD_SCENE: return "quick-over-scene";
    case VR_FRAME_MENU_PANEL: return "menu-panel";
    case VR_FRAME_REJECTED: break;
    }
    switch (outcome.rejection) {
    case VR_FRAME_REJECTED_NOT_CAPTURED: return "not-captured";
    case VR_FRAME_REJECTED_NOT_A_GAME_FRAME: return "not-a-game-frame";
    case VR_FRAME_REJECTED_UNCERTIFIED: return "uncertified";
    case VR_FRAME_REJECTED_MISPLACED: return "misplaced";
    case VR_FRAME_REJECTED_MENU_WITHOUT_POSE: return "menu-no-pose";
    case VR_FRAME_REJECTED_MENU_WITHOUT_PANEL: return "menu-no-panel";
    case VR_FRAME_NOT_REJECTED: break;
    }
    return "none";
}

#endif
