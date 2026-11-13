// Asset-free check of the PC VR frame classification (runtime/nvn/vulkan/vr_frame.h): what a frame offers the XR owner
// at its present. A table of frames reaches every kind (scene, scene with HUD, HUD over the held scene, menu panel) and
// every rejection (not captured, not a game frame, uncertified, misplaced, menu without a pose, menu without a panel),
// with the scene pair's guard broken one comparison at a time (certificates, placements, traversal, generation,
// tracking frame, pose snapshot) and the held-scene conditions failed one at a time. Then the two order traps (a
// scene's HUD does not depend on the eased HUD distance, which changes only after the scene's kind is decided; a quick
// menu whose panel fails falls back to the menu panel, not to a rejection), the whole downgrade chain, the log names,
// and the reset of the findings. No renderer, GPU or headset is involved.
#include "../runtime/nvn/vulkan/vr_frame.h"

#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; if (failures < 10) printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

#define HELD_GENERATION 7  // the tracking generation of the fixture's held scene and anchored pose
#define HUD_DEPTH_SETTING 1.2  // a HUD panel distance setting (times the camera target's distance)
#define EASED_HUD_DISTANCE_M 1.4

// Both eyes' certificates of one rendering: one traversal, generation, tracking frame and pose snapshot, as the
// scene-final draws of a good frame leave them.
static void certify_scene_pair(VrFrameFindings *findings) {
    XrPoseSnapshot snapshot = {.frame_id = 1234, .game_present_id = 500, .generation = HELD_GENERATION,
        .orientation_xyzw = {0, 0, 0, 1}, .native_views_valid = true,
        .eyes = {{.orientation_xyzw = {0, 0, 0, 1}, .position_xyz = {-0.032f, 1.6f, 0},
                  .fov_left_right_up_down = {-0.9f, 0.8f, 0.85f, -0.9f}},
                 {.orientation_xyzw = {0, 0, 0, 1}, .position_xyz = {0.032f, 1.6f, 0},
                  .fov_left_right_up_down = {-0.8f, 0.9f, 0.85f, -0.9f}}},
        .calibration = {.epoch = 1, .heading_xyzw = {0, 0, 0, 1}, .origin_xyz = {0, 1.6f, 0}}};
    for (unsigned eye = 0; eye < 2; ++eye) {
        findings->eyes[eye] = (PcVrRenderCertificate){.id = 40 + eye, .traversal = 900, .frame = 1234,
            .generation = HELD_GENERATION, .units_per_meter = 10, .native_index = eye, .native_snapshot = snapshot};
        findings->scene_output[eye] = 70 + eye;
    }
    findings->scene_seen = true;
    findings->certified = 3;
    findings->placed = 3;
    findings->placement_seen = true;
}

// A frame without a scene whose pose query gave a valid, natively viewed pose and a calibration anchor.
static void anchor_menu(VrFrameInputs *inputs) {
    inputs->pose_valid = inputs->pose_native_views = inputs->anchor_built = true;
    inputs->anchor_generation = HELD_GENERATION;
}

// A quick menu opened over the held scene: everything the HUD over the held scene needs.
static void open_quick_menu(VrFrameFindings *findings, VrFrameInputs *inputs) {
    anchor_menu(inputs);
    findings->over_world = findings->over_world_known = true;
    inputs->slot_has_hud = true;
    inputs->hud_distance_m = EASED_HUD_DISTANCE_M;
    inputs->hud_panel_depth = HUD_DEPTH_SETTING;
    inputs->scene_held = HELD_GENERATION;
}

static void producer_off(VrFrameFindings *findings, VrFrameInputs *inputs) {
    certify_scene_pair(findings);
    inputs->producer_armed = false;
}
static void no_slot_filled(VrFrameFindings *findings, VrFrameInputs *inputs) {
    certify_scene_pair(findings);
    inputs->slot_filled = false;
}
static void no_slot_on_re_presentation(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)findings;
    inputs->slot_filled = false;
    inputs->game_present_id = 0;
}
static void ui_re_presentation(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)findings;
    inputs->game_present_id = 0;
}
static void ui_re_presentation_of_scene(VrFrameFindings *findings, VrFrameInputs *inputs) {
    certify_scene_pair(findings);
    inputs->game_present_id = 0;
}
static void scene_pair(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    certify_scene_pair(findings);
}
static void scene_pair_with_hud(VrFrameFindings *findings, VrFrameInputs *inputs) {
    certify_scene_pair(findings);
    inputs->slot_has_hud = true;
    inputs->hud_panel_depth = HUD_DEPTH_SETTING;
}
static void scene_pair_hud_panel_off(VrFrameFindings *findings, VrFrameInputs *inputs) {
    scene_pair_with_hud(findings, inputs);
    inputs->hud_panel_depth = 0;
}
static void scene_pair_no_hud_this_frame(VrFrameFindings *findings, VrFrameInputs *inputs) {
    scene_pair_with_hud(findings, inputs);
    inputs->slot_has_hud = false;
}
static void scene_pair_with_hud_never_placed(VrFrameFindings *findings, VrFrameInputs *inputs) {
    scene_pair_with_hud(findings, inputs);
    inputs->hud_distance_m = 0;  // (order trap: the scene's kind does not wait for the eased distance)
}
static void scene_pair_while_held_inputs_say_quick(VrFrameFindings *findings, VrFrameInputs *inputs) {
    open_quick_menu(findings, inputs);
    certify_scene_pair(findings);  // (a scene after all: the scene path ignores the pose and the held scene)
    inputs->pose_valid = false;
}
static void left_eye_uncertified(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    certify_scene_pair(findings);
    findings->certified = findings->placed = 2;
}
static void scene_without_certificates(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    findings->scene_seen = true;  // (an eye layer's scene-final draw without a certificate, as after a load)
}
static void only_left_certified_unplaced(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    certify_scene_pair(findings);
    findings->certified = 1;
    findings->placed = 0;
}
static void traversals_differ(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    certify_scene_pair(findings);
    findings->eyes[1].traversal = 901;
}
static void generations_differ(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    certify_scene_pair(findings);
    findings->eyes[1].generation = HELD_GENERATION + 1;
}
static void tracking_frames_differ(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    certify_scene_pair(findings);
    findings->eyes[0].frame = 1233;
}
static void pose_snapshots_differ(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    certify_scene_pair(findings);
    findings->eyes[1].native_snapshot.eyes[1].position_xyz[0] = 0.033f;
}
static void eye_placed_in_other_half(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    certify_scene_pair(findings);
    findings->placed = 1;
    findings->misplaced = true;
}
static void misplaced_with_all_else_agreeing(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    certify_scene_pair(findings);
    findings->misplaced = true;
}
static void right_eye_never_placed(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    certify_scene_pair(findings);
    findings->placed = 1;
}
static void misplaced_with_certificates_disagreeing(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    certify_scene_pair(findings);
    findings->misplaced = true;
    findings->eyes[1].generation = HELD_GENERATION + 1;  // (misplaced wins over the disagreement)
}
static void unplaced_with_traversals_differing(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    certify_scene_pair(findings);
    findings->placed = 2;
    findings->eyes[1].traversal = 901;  // (both certified, one unplaced: misplaced, whatever the certificates say)
}
static void menu_without_pose(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)findings;
    (void)inputs;
}
static void menu_pose_without_native_views(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)findings;
    inputs->pose_valid = true;
}
static void menu_without_anchor(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)findings;
    inputs->pose_valid = inputs->pose_native_views = true;
}
static void menu(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)findings;
    anchor_menu(inputs);
}
static void menu_with_hud_but_no_quick_menu(VrFrameFindings *findings, VrFrameInputs *inputs) {
    open_quick_menu(findings, inputs);
    findings->over_world = false;
}
static void quick_menu(VrFrameFindings *findings, VrFrameInputs *inputs) {
    open_quick_menu(findings, inputs);
}
static void quick_menu_without_hud_this_frame(VrFrameFindings *findings, VrFrameInputs *inputs) {
    open_quick_menu(findings, inputs);
    inputs->slot_has_hud = false;
}
static void quick_menu_hud_never_placed(VrFrameFindings *findings, VrFrameInputs *inputs) {
    open_quick_menu(findings, inputs);
    inputs->hud_distance_m = 0;
}
static void quick_menu_hud_panel_off(VrFrameFindings *findings, VrFrameInputs *inputs) {
    open_quick_menu(findings, inputs);
    inputs->hud_panel_depth = 0;
}
static void quick_menu_after_recenter(VrFrameFindings *findings, VrFrameInputs *inputs) {
    open_quick_menu(findings, inputs);
    inputs->anchor_generation = HELD_GENERATION + 1;
}
static void quick_menu_after_menu_panel(VrFrameFindings *findings, VrFrameInputs *inputs) {
    open_quick_menu(findings, inputs);
    inputs->scene_held = 0;  // (a menu panel pair replaced the held scene)
}
// The held scene's generation and the anchor's both 0, everything else a quick menu: the comparison is only equality,
// so this is the HUD over the held scene, pinned as the code decides it. exec.c cannot produce it today:
// vr_quick_menu_over_world latches over_world only while scene_held is nonzero, and scene_held changes only at a
// present or the game reset (which also clears over_world).
static void quick_menu_both_generations_zero(VrFrameFindings *findings, VrFrameInputs *inputs) {
    open_quick_menu(findings, inputs);
    inputs->scene_held = inputs->anchor_generation = 0;
}
static void quick_menu_without_pose(VrFrameFindings *findings, VrFrameInputs *inputs) {
    open_quick_menu(findings, inputs);
    inputs->anchor_built = false;
}

// One frame of the table: how it differs from the base frame (armed, a slot without HUD, a game present, no scene,
// no pose), and what it must be classified as.
typedef struct {
    const char *name;
    void (*arrange)(VrFrameFindings *findings, VrFrameInputs *inputs);
    VrFrameKind kind;
    VrFrameRejection rejection;
    const char *log_name;
} ClassifyCase;

static const ClassifyCase classify_cases[] = {
    {"producer off", producer_off, VR_FRAME_REJECTED, VR_FRAME_REJECTED_NOT_CAPTURED, "not-captured"},
    {"no slot filled", no_slot_filled, VR_FRAME_REJECTED, VR_FRAME_REJECTED_NOT_CAPTURED, "not-captured"},
    {"no slot on a re-presentation", no_slot_on_re_presentation, VR_FRAME_REJECTED, VR_FRAME_REJECTED_NOT_CAPTURED,
     "not-captured"},
    {"UI re-presentation", ui_re_presentation, VR_FRAME_REJECTED, VR_FRAME_REJECTED_NOT_A_GAME_FRAME, "not-a-game-frame"},
    {"UI re-presentation of a scene pair", ui_re_presentation_of_scene, VR_FRAME_REJECTED,
     VR_FRAME_REJECTED_NOT_A_GAME_FRAME, "not-a-game-frame"},
    {"scene pair", scene_pair, VR_FRAME_SCENE, VR_FRAME_NOT_REJECTED, "scene"},
    {"scene pair with HUD", scene_pair_with_hud, VR_FRAME_SCENE_WITH_HUD, VR_FRAME_NOT_REJECTED, "scene-hud"},
    {"scene pair, HUD panel off", scene_pair_hud_panel_off, VR_FRAME_SCENE, VR_FRAME_NOT_REJECTED, "scene"},
    {"scene pair, no HUD this frame", scene_pair_no_hud_this_frame, VR_FRAME_SCENE, VR_FRAME_NOT_REJECTED, "scene"},
    {"scene pair with HUD never placed", scene_pair_with_hud_never_placed, VR_FRAME_SCENE_WITH_HUD,
     VR_FRAME_NOT_REJECTED, "scene-hud"},
    {"scene pair while the held inputs say quick menu", scene_pair_while_held_inputs_say_quick,
     VR_FRAME_SCENE_WITH_HUD, VR_FRAME_NOT_REJECTED, "scene-hud"},
    {"left eye uncertified", left_eye_uncertified, VR_FRAME_REJECTED, VR_FRAME_REJECTED_UNCERTIFIED, "uncertified"},
    {"scene without certificates", scene_without_certificates, VR_FRAME_REJECTED, VR_FRAME_REJECTED_UNCERTIFIED,
     "uncertified"},
    {"only the left certified, unplaced", only_left_certified_unplaced, VR_FRAME_REJECTED,
     VR_FRAME_REJECTED_UNCERTIFIED, "uncertified"},
    {"traversals differ", traversals_differ, VR_FRAME_REJECTED, VR_FRAME_REJECTED_UNCERTIFIED, "uncertified"},
    {"generations differ", generations_differ, VR_FRAME_REJECTED, VR_FRAME_REJECTED_UNCERTIFIED, "uncertified"},
    {"tracking frames differ", tracking_frames_differ, VR_FRAME_REJECTED, VR_FRAME_REJECTED_UNCERTIFIED,
     "uncertified"},
    {"pose snapshots differ", pose_snapshots_differ, VR_FRAME_REJECTED, VR_FRAME_REJECTED_UNCERTIFIED, "uncertified"},
    {"an eye placed in the other half", eye_placed_in_other_half, VR_FRAME_REJECTED, VR_FRAME_REJECTED_MISPLACED,
     "misplaced"},
    {"misplaced with all else agreeing", misplaced_with_all_else_agreeing, VR_FRAME_REJECTED,
     VR_FRAME_REJECTED_MISPLACED, "misplaced"},
    {"right eye never placed", right_eye_never_placed, VR_FRAME_REJECTED, VR_FRAME_REJECTED_MISPLACED, "misplaced"},
    {"misplaced with certificates disagreeing", misplaced_with_certificates_disagreeing, VR_FRAME_REJECTED,
     VR_FRAME_REJECTED_MISPLACED, "misplaced"},
    {"one eye unplaced, traversals differing", unplaced_with_traversals_differing, VR_FRAME_REJECTED,
     VR_FRAME_REJECTED_MISPLACED, "misplaced"},
    {"menu without a pose", menu_without_pose, VR_FRAME_REJECTED, VR_FRAME_REJECTED_MENU_WITHOUT_POSE, "menu-no-pose"},
    {"menu, pose without native views", menu_pose_without_native_views, VR_FRAME_REJECTED,
     VR_FRAME_REJECTED_MENU_WITHOUT_POSE, "menu-no-pose"},
    {"menu without a calibration anchor", menu_without_anchor, VR_FRAME_REJECTED, VR_FRAME_REJECTED_MENU_WITHOUT_POSE,
     "menu-no-pose"},
    {"menu", menu, VR_FRAME_MENU_PANEL, VR_FRAME_NOT_REJECTED, "menu-panel"},
    {"menu with a HUD but no quick menu", menu_with_hud_but_no_quick_menu, VR_FRAME_MENU_PANEL, VR_FRAME_NOT_REJECTED,
     "menu-panel"},
    {"quick menu over the held scene", quick_menu, VR_FRAME_HUD_OVER_HELD_SCENE, VR_FRAME_NOT_REJECTED,
     "quick-over-scene"},
    {"quick menu without its HUD this frame", quick_menu_without_hud_this_frame, VR_FRAME_MENU_PANEL,
     VR_FRAME_NOT_REJECTED, "menu-panel"},
    {"quick menu, HUD never placed", quick_menu_hud_never_placed, VR_FRAME_MENU_PANEL, VR_FRAME_NOT_REJECTED,
     "menu-panel"},
    {"quick menu, HUD panel off", quick_menu_hud_panel_off, VR_FRAME_MENU_PANEL, VR_FRAME_NOT_REJECTED, "menu-panel"},
    {"quick menu after a recenter", quick_menu_after_recenter, VR_FRAME_MENU_PANEL, VR_FRAME_NOT_REJECTED,
     "menu-panel"},
    {"quick menu after a menu panel", quick_menu_after_menu_panel, VR_FRAME_MENU_PANEL, VR_FRAME_NOT_REJECTED,
     "menu-panel"},
    {"quick menu, both generations 0", quick_menu_both_generations_zero, VR_FRAME_HUD_OVER_HELD_SCENE,
     VR_FRAME_NOT_REJECTED, "quick-over-scene"},
    {"quick menu without a pose", quick_menu_without_pose, VR_FRAME_REJECTED, VR_FRAME_REJECTED_MENU_WITHOUT_POSE,
     "menu-no-pose"},
};

// The base frame of the table: the producer armed, a filled slot without a HUD, a game present, nothing drawn yet.
static void base_frame(VrFrameFindings *findings, VrFrameInputs *inputs) {
    memset(findings, 0, sizeof *findings);
    *inputs = (VrFrameInputs){.findings = findings, .producer_armed = true, .slot_filled = true,
                              .game_present_id = 500};
}

static bool outcome_is(VrFrameOutcome outcome, VrFrameKind kind, VrFrameRejection rejection) {
    return outcome.kind == kind && outcome.rejection == rejection;
}

// ------------------------------------------------------------------------------------------- the differential check
// The decision vk_vr_present made before vr_frame.h (the runtime at 96fba7c), transcribed as a reference over plain
// values, against the new sequence (vr_frame_not_offered, vr_frame_classify, the panels and vr_frame_without_panel as
// exec.c now applies them), for every combination of the values the decision reads.

// Everything the old decision read, as plain values. A panel "builds" when vr_panel would succeed for it.
typedef struct {
    bool producer_armed, slot_filled, game_frame;  // vr_eyes.state > 0; a slot filled; game_present_id != 0
    bool scene_seen;
    unsigned certified, placed;  // per-eye bits, 0 to 3
    bool misplaced;
    bool same_traversal, same_generation, same_frame, same_snapshot;  // the two eyes' certificates
    bool over_world, slot_has_hud;
    bool hud_setting_on;         // pc_vr_panel_depth(PC_VR_PANEL_HUD) nonzero
    bool hud_distance_positive;  // the eased HUD distance before this frame
    bool pose_valid, native_views, anchor_succeeds;
    bool held_is_anchor_generation;  // vr_eyes.scene_held == the anchor's generation
    bool scene_hud_panel_builds, quick_panel_builds, menu_panel_builds;
} DecisionInputs;

// What a decision did, in the terms both versions share.
typedef struct {
    int ended;          // 0: went on to the offer; 1: returned with nothing; 2: released the slot unoffered
    const char *kind;   // the log name (NULL when it ended early)
    bool ready;         // offered to the XR owner
    char panels[4];     // the panels tried, in order: 'S' the scene's HUD, 'Q' the quick menu's, 'M' the menu's
    unsigned panel_count;
    bool eased;         // the HUD distance easing ran
    char counter;       // the counter bumped: 'm' misplaced_frames, 'u' uncertified, 'p' menu_pairs, 0 none
    bool pose_queried, anchor_called, hud_setting_read, menu_setting_read;
} DecisionTrace;

// A vr_panel call: recorded in order; returns whether that panel builds.
static bool try_panel(DecisionTrace *trace, char panel, bool builds) {
    trace->panels[trace->panel_count++] = panel;
    return builds;
}

// The old decision, branch for branch as vk_vr_present had it at 96fba7c (exec.c lines 6067-6140 there), with its
// calls replaced by the trace. Written from that code, not from vr_frame.h.
static DecisionTrace decision_before_vr_frame(const DecisionInputs *in) {
    DecisionTrace trace = {0};
    if (!in->producer_armed || !in->slot_filled) {  // if (vr_eyes.state <= 0 || !slot) return;
        trace.ended = 1;
        return trace;
    }
    if (!in->game_frame) {  // if (!game_present_id) { vr_eye_slot_release(slot); return; }
        trace.ended = 2;
        return trace;
    }
    const char *kind = "none";
    bool ready = false;
    if (in->scene_seen) {
        const bool same = in->certified == 3 && in->placed == 3 && !in->misplaced && in->same_traversal &&
            in->same_generation && in->same_frame && in->same_snapshot;
        if (same) {
            kind = "scene";
            ready = true;
            trace.hud_setting_read = in->slot_has_hud;  // (slot->hud ? pc_vr_panel_depth(PC_VR_PANEL_HUD) : 0)
            const bool hud_depth = in->slot_has_hud && in->hud_setting_on;
            if (hud_depth) {
                trace.eased = true;
                if (try_panel(&trace, 'S', in->scene_hud_panel_builds)) kind = "scene-hud";
            }
        } else if (in->misplaced || (in->certified == 3 && in->placed != 3)) {
            trace.counter = 'm';
            kind = "misplaced";
        } else {
            trace.counter = 'u';
            kind = "uncertified";
        }
    } else {
        trace.pose_queried = true;  // (xr_runtime_get_pose)
        trace.anchor_called = in->pose_valid && in->native_views;
        if (in->pose_valid && in->native_views && in->anchor_succeeds) {
            trace.hud_setting_read = true;
            if (in->over_world && in->slot_has_hud && in->hud_distance_positive && in->hud_setting_on &&
                in->held_is_anchor_generation && try_panel(&trace, 'Q', in->quick_panel_builds)) {
                ready = true;
                kind = "quick-over-scene";
            } else {
                trace.menu_setting_read = true;
                ready = try_panel(&trace, 'M', in->menu_panel_builds);
                kind = ready ? "menu-panel" : "menu-no-panel";
            }
            if (ready) trace.counter = 'p';
        } else kind = "menu-no-pose";
    }
    trace.kind = kind;
    trace.ready = ready;
    return trace;
}

// The new sequence, as exec.c's vk_vr_present now runs it, over the same values. findings already holds the eyes'
// certificates for the four comparisons; the flags are set here.
static DecisionTrace decision_with_vr_frame(const DecisionInputs *in, VrFrameFindings *findings) {
    DecisionTrace trace = {0};
    findings->scene_seen = in->scene_seen;
    findings->certified = in->certified;
    findings->placed = in->placed;
    findings->misplaced = in->misplaced;
    findings->over_world = in->over_world;
    VrFrameInputs inputs = {.findings = findings, .producer_armed = in->producer_armed, .slot_filled = in->slot_filled,
        .slot_has_hud = in->slot_filled && in->slot_has_hud, .game_present_id = in->game_frame ? 500 : 0,
        .scene_held = in->held_is_anchor_generation ? HELD_GENERATION : HELD_GENERATION + 1,
        .hud_distance_m = in->hud_distance_positive ? EASED_HUD_DISTANCE_M : 0};
    const VrFrameRejection not_offered = vr_frame_not_offered(&inputs);
    if (not_offered == VR_FRAME_REJECTED_NOT_CAPTURED) {
        trace.ended = 1;
        return trace;
    }
    if (not_offered == VR_FRAME_REJECTED_NOT_A_GAME_FRAME) {
        trace.ended = 2;
        return trace;
    }
    if (!findings->scene_seen) {
        trace.pose_queried = true;
        inputs.pose_valid = in->pose_valid;
        inputs.pose_native_views = inputs.pose_valid && in->native_views;
        trace.anchor_called = inputs.pose_native_views;
        inputs.anchor_built = inputs.pose_native_views && in->anchor_succeeds;
        inputs.anchor_generation = inputs.anchor_built ? HELD_GENERATION : 0;  // (offer.render_pose.generation)
    }
    if (findings->scene_seen ? in->slot_has_hud && vr_frame_eyes_agree(findings) : inputs.anchor_built) {
        trace.hud_setting_read = true;
        inputs.hud_panel_depth = in->hud_setting_on ? HUD_DEPTH_SETTING : 0;
    }
    VrFrameOutcome outcome = vr_frame_classify(&inputs);
    if (outcome.kind == VR_FRAME_SCENE_WITH_HUD) {
        trace.eased = true;
        if (!try_panel(&trace, 'S', in->scene_hud_panel_builds)) outcome = vr_frame_without_panel(outcome);
    }
    if (outcome.kind == VR_FRAME_HUD_OVER_HELD_SCENE) {
        if (!try_panel(&trace, 'Q', in->quick_panel_builds)) outcome = vr_frame_without_panel(outcome);
    }
    if (outcome.kind == VR_FRAME_MENU_PANEL) {
        trace.menu_setting_read = true;
        if (!try_panel(&trace, 'M', in->menu_panel_builds)) outcome = vr_frame_without_panel(outcome);
    }
    if (outcome.kind == VR_FRAME_HUD_OVER_HELD_SCENE || outcome.kind == VR_FRAME_MENU_PANEL) trace.counter = 'p';
    else if (outcome.rejection == VR_FRAME_REJECTED_MISPLACED) trace.counter = 'm';
    else if (outcome.rejection == VR_FRAME_REJECTED_UNCERTIFIED) trace.counter = 'u';
    trace.ready = outcome.kind != VR_FRAME_REJECTED;
    trace.kind = vr_frame_kind_name(outcome);
    return trace;
}

static bool traces_match(const DecisionTrace *before, const DecisionTrace *after) {
    const bool kinds_match = before->kind && after->kind ? !strcmp(before->kind, after->kind) : before->kind == after->kind;
    return before->ended == after->ended && kinds_match && before->ready == after->ready &&
        !strcmp(before->panels, after->panels) && before->eased == after->eased && before->counter == after->counter &&
        before->pose_queried == after->pose_queried && before->anchor_called == after->anchor_called &&
        before->hud_setting_read == after->hud_setting_read && before->menu_setting_read == after->menu_setting_read;
}

static void print_trace(const char *label, const DecisionTrace *trace) {
    printf("  %s: ended=%d kind=%s ready=%d panels=%s eased=%d counter=%c pose=%d anchor=%d hud_read=%d menu_read=%d\n",
           label, trace->ended, trace->kind ? trace->kind : "-", trace->ready, trace->panels, trace->eased,
           trace->counter ? trace->counter : '-', trace->pose_queried, trace->anchor_called, trace->hud_setting_read,
           trace->menu_setting_read);
}

#define DECISION_INPUT_BITS 24  // the bits of one combination, decoded below

// The next width bits of a combination's number, lowest first; *position moves past them.
static unsigned take_bits(unsigned long number, unsigned *position, unsigned width) {
    const unsigned value = (unsigned)((number >> *position) & ((1ul << width) - 1));
    *position += width;
    return value;
}

// Runs both decisions on every combination, in increasing order of its number (so the first disagreement is the
// smallest), and returns how many combinations disagree; *combinations receives how many were run.
static unsigned long differential_check(unsigned long *combinations) {
    // The eyes' certificates for each set of the four comparisons (bit 0 traversal, 1 generation, 2 tracking frame,
    // 3 pose snapshot; a set bit: that comparison differs).
    static VrFrameFindings eye_templates[16];
    for (unsigned differing = 0; differing < 16; ++differing) {
        memset(&eye_templates[differing], 0, sizeof eye_templates[differing]);
        certify_scene_pair(&eye_templates[differing]);
        if (differing & 1) eye_templates[differing].eyes[1].traversal = 901;
        if (differing & 2) eye_templates[differing].eyes[1].generation = HELD_GENERATION + 1;
        if (differing & 4) eye_templates[differing].eyes[0].frame = 1233;
        if (differing & 8) eye_templates[differing].eyes[1].native_snapshot.eyes[1].position_xyz[0] = 0.033f;
    }
    VrFrameFindings findings;
    unsigned current_template = 16;
    unsigned long disagreements = 0;
    *combinations = 0;
    for (unsigned long number = 0; number < 1ul << DECISION_INPUT_BITS; ++number) {
        unsigned position = 0;
        DecisionInputs in = {0};
        in.scene_hud_panel_builds = take_bits(number, &position, 1);
        in.quick_panel_builds = take_bits(number, &position, 1);
        in.menu_panel_builds = take_bits(number, &position, 1);
        in.held_is_anchor_generation = take_bits(number, &position, 1);
        in.anchor_succeeds = take_bits(number, &position, 1);
        in.native_views = take_bits(number, &position, 1);
        in.pose_valid = take_bits(number, &position, 1);
        in.hud_distance_positive = take_bits(number, &position, 1);
        in.hud_setting_on = take_bits(number, &position, 1);
        in.slot_has_hud = take_bits(number, &position, 1);
        in.over_world = take_bits(number, &position, 1);
        const unsigned differing = take_bits(number, &position, 4);
        in.same_traversal = !(differing & 1);
        in.same_generation = !(differing & 2);
        in.same_frame = !(differing & 4);
        in.same_snapshot = !(differing & 8);
        in.misplaced = take_bits(number, &position, 1);
        in.placed = take_bits(number, &position, 2);
        in.certified = take_bits(number, &position, 2);
        in.scene_seen = take_bits(number, &position, 1);
        in.game_frame = take_bits(number, &position, 1);
        in.slot_filled = take_bits(number, &position, 1);
        in.producer_armed = take_bits(number, &position, 1);
        if (differing != current_template) {
            findings = eye_templates[differing];
            current_template = differing;
        }
        const DecisionTrace before = decision_before_vr_frame(&in);
        const DecisionTrace after = decision_with_vr_frame(&in, &findings);
        ++*combinations;
        if (traces_match(&before, &after)) continue;
        if (!disagreements++) {
            printf("first disagreement, combination %lu: armed=%d slot=%d game_frame=%d scene=%d certified=%u placed=%u "
                   "misplaced=%d same(traversal,generation,frame,snapshot)=%d%d%d%d over_world=%d slot_hud=%d "
                   "hud_on=%d distance=%d pose=%d native=%d anchor=%d held_match=%d panels(S,Q,M)=%d%d%d\n",
                   number, in.producer_armed, in.slot_filled, in.game_frame, in.scene_seen, in.certified, in.placed,
                   in.misplaced, in.same_traversal, in.same_generation, in.same_frame, in.same_snapshot, in.over_world,
                   in.slot_has_hud, in.hud_setting_on, in.hud_distance_positive, in.pose_valid, in.native_views,
                   in.anchor_succeeds, in.held_is_anchor_generation, in.scene_hud_panel_builds, in.quick_panel_builds,
                   in.menu_panel_builds);
            print_trace("before", &before);
            print_trace("after", &after);
        }
    }
    return disagreements;
}

int main(void) {
    VrFrameFindings findings;
    VrFrameInputs inputs;

    // Every frame of the table, and its log name.
    unsigned kinds_reached = 0, rejections_reached = 0;
    for (size_t index = 0; index < sizeof classify_cases / sizeof classify_cases[0]; ++index) {
        const ClassifyCase *frame_case = &classify_cases[index];
        base_frame(&findings, &inputs);
        frame_case->arrange(&findings, &inputs);
        const VrFrameOutcome outcome = vr_frame_classify(&inputs);
        const bool as_expected = outcome_is(outcome, frame_case->kind, frame_case->rejection) &&
            !strcmp(vr_frame_kind_name(outcome), frame_case->log_name);
        CHECK(as_expected);
        if (!as_expected) printf("  case \"%s\": got %s\n", frame_case->name, vr_frame_kind_name(outcome));
        // The early step agrees with the whole classification about whether anything is offered.
        const VrFrameRejection not_offered = vr_frame_not_offered(&inputs);
        CHECK(not_offered == (outcome.rejection == VR_FRAME_REJECTED_NOT_CAPTURED ||
                              outcome.rejection == VR_FRAME_REJECTED_NOT_A_GAME_FRAME ? outcome.rejection
                                                                                       : VR_FRAME_NOT_REJECTED));
        kinds_reached |= 1u << outcome.kind;
        rejections_reached |= 1u << outcome.rejection;
    }
    // Every kind and every rejection was reached by some frame.
    CHECK(kinds_reached == (1u << VR_FRAME_SCENE | 1u << VR_FRAME_SCENE_WITH_HUD | 1u << VR_FRAME_HUD_OVER_HELD_SCENE |
                            1u << VR_FRAME_MENU_PANEL | 1u << VR_FRAME_REJECTED));
    CHECK(rejections_reached == (1u << VR_FRAME_NOT_REJECTED | 1u << VR_FRAME_REJECTED_NOT_CAPTURED |
                                 1u << VR_FRAME_REJECTED_NOT_A_GAME_FRAME | 1u << VR_FRAME_REJECTED_UNCERTIFIED |
                                 1u << VR_FRAME_REJECTED_MISPLACED | 1u << VR_FRAME_REJECTED_MENU_WITHOUT_POSE));
    // (menu without a panel comes only from a panel that cannot be built: below)

    // The downgrade chain: a quick menu whose HUD panel fails is the menu panel (not a rejection: order trap), whose
    // own panel failing is a rejection; a scene whose HUD panel fails is still a scene.
    base_frame(&findings, &inputs);
    open_quick_menu(&findings, &inputs);
    const VrFrameOutcome quick = vr_frame_classify(&inputs);
    const VrFrameOutcome quick_without_panel = vr_frame_without_panel(quick);
    CHECK(outcome_is(quick_without_panel, VR_FRAME_MENU_PANEL, VR_FRAME_NOT_REJECTED));
    CHECK(!strcmp(vr_frame_kind_name(quick_without_panel), "menu-panel"));
    const VrFrameOutcome menu_without_panel = vr_frame_without_panel(quick_without_panel);
    CHECK(outcome_is(menu_without_panel, VR_FRAME_REJECTED, VR_FRAME_REJECTED_MENU_WITHOUT_PANEL));
    CHECK(!strcmp(vr_frame_kind_name(menu_without_panel), "menu-no-panel"));
    base_frame(&findings, &inputs);
    scene_pair_with_hud(&findings, &inputs);
    const VrFrameOutcome scene_without_hud_panel = vr_frame_without_panel(vr_frame_classify(&inputs));
    CHECK(outcome_is(scene_without_hud_panel, VR_FRAME_SCENE, VR_FRAME_NOT_REJECTED));
    CHECK(!strcmp(vr_frame_kind_name(scene_without_hud_panel), "scene"));
    // Kinds that need no panel stay as they are.
    CHECK(outcome_is(vr_frame_without_panel(vr_frame_offers(VR_FRAME_SCENE)), VR_FRAME_SCENE, VR_FRAME_NOT_REJECTED));
    const VrFrameRejection every_rejection[] = {VR_FRAME_REJECTED_NOT_CAPTURED, VR_FRAME_REJECTED_NOT_A_GAME_FRAME,
        VR_FRAME_REJECTED_UNCERTIFIED, VR_FRAME_REJECTED_MISPLACED, VR_FRAME_REJECTED_MENU_WITHOUT_POSE,
        VR_FRAME_REJECTED_MENU_WITHOUT_PANEL};
    for (size_t index = 0; index < sizeof every_rejection / sizeof every_rejection[0]; ++index)
        CHECK(outcome_is(vr_frame_without_panel(vr_frame_rejects(every_rejection[index])), VR_FRAME_REJECTED,
                         every_rejection[index]));

    // The order trap on the scene path: whatever the eased HUD distance, a scene pair with this frame's HUD asks for
    // its HUD panel (exec.c updates the distance after this decision, before building the panel).
    base_frame(&findings, &inputs);
    scene_pair_with_hud(&findings, &inputs);
    inputs.hud_distance_m = 0;
    const VrFrameOutcome before_any_placement = vr_frame_classify(&inputs);
    inputs.hud_distance_m = 25;
    CHECK(outcome_is(before_any_placement, VR_FRAME_SCENE_WITH_HUD, VR_FRAME_NOT_REJECTED));
    CHECK(outcome_is(vr_frame_classify(&inputs), VR_FRAME_SCENE_WITH_HUD, VR_FRAME_NOT_REJECTED));

    // The log names of every outcome exec.c logs, exactly as the evidence quotes them.
    CHECK(!strcmp(vr_frame_kind_name(vr_frame_offers(VR_FRAME_SCENE)), "scene"));
    CHECK(!strcmp(vr_frame_kind_name(vr_frame_offers(VR_FRAME_SCENE_WITH_HUD)), "scene-hud"));
    CHECK(!strcmp(vr_frame_kind_name(vr_frame_offers(VR_FRAME_HUD_OVER_HELD_SCENE)), "quick-over-scene"));
    CHECK(!strcmp(vr_frame_kind_name(vr_frame_offers(VR_FRAME_MENU_PANEL)), "menu-panel"));
    CHECK(!strcmp(vr_frame_kind_name(vr_frame_rejects(VR_FRAME_REJECTED_UNCERTIFIED)), "uncertified"));
    CHECK(!strcmp(vr_frame_kind_name(vr_frame_rejects(VR_FRAME_REJECTED_MISPLACED)), "misplaced"));
    CHECK(!strcmp(vr_frame_kind_name(vr_frame_rejects(VR_FRAME_REJECTED_MENU_WITHOUT_POSE)), "menu-no-pose"));
    CHECK(!strcmp(vr_frame_kind_name(vr_frame_rejects(VR_FRAME_REJECTED_MENU_WITHOUT_PANEL)), "menu-no-panel"));
    // And the two outcomes that end before any offer (never logged).
    CHECK(!strcmp(vr_frame_kind_name(vr_frame_rejects(VR_FRAME_REJECTED_NOT_CAPTURED)), "not-captured"));
    CHECK(!strcmp(vr_frame_kind_name(vr_frame_rejects(VR_FRAME_REJECTED_NOT_A_GAME_FRAME)), "not-a-game-frame"));

    // The reset: every field of a frame's findings back to its initial value, zero, as vr_eyes starts.
    memset(&findings, 0xa5, sizeof findings);
    certify_scene_pair(&findings);
    findings.misplaced = findings.over_world = findings.over_world_known = true;
    vr_frame_findings_reset(&findings);
    static const VrFrameFindings initial_findings;
    CHECK(!memcmp(&findings, &initial_findings, sizeof findings));
    CHECK(!findings.scene_seen && !findings.certified && !findings.placed && !findings.misplaced &&
          !findings.placement_seen && !findings.over_world && !findings.over_world_known);
    CHECK(!findings.eyes[0].traversal && !findings.eyes[1].native_snapshot.generation && !findings.scene_output[0] &&
          !findings.scene_output[1]);
    // A reset frame with a filled slot at a game present is a menu frame: it never passes for a scene.
    inputs = (VrFrameInputs){.findings = &findings, .producer_armed = true, .slot_filled = true, .game_present_id = 501};
    CHECK(outcome_is(vr_frame_classify(&inputs), VR_FRAME_REJECTED, VR_FRAME_REJECTED_MENU_WITHOUT_POSE));

    // The new sequence against the old decision, every combination of what that decision reads.
    unsigned long combinations = 0;
    const unsigned long disagreements = differential_check(&combinations);
    CHECK(combinations == 1ul << DECISION_INPUT_BITS);
    CHECK(disagreements == 0);
    printf("vr_frame differential: %lu combinations, %lu disagreements\n", combinations, disagreements);

    printf("vr_frame fixture: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
