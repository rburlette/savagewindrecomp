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
        .local_definition = HELD_GENERATION,
        .orientation_xyzw = {0, 0, 0, 1}, .native_views_valid = true,
        .eyes = {{.orientation_xyzw = {0, 0, 0, 1}, .position_xyz = {-0.032f, 1.6f, 0},
                  .fov_left_right_up_down = {-0.9f, 0.8f, 0.85f, -0.9f}},
                 {.orientation_xyzw = {0, 0, 0, 1}, .position_xyz = {0.032f, 1.6f, 0},
                  .fov_left_right_up_down = {-0.8f, 0.9f, 0.85f, -0.9f}}},
        .calibration = {.epoch = 1, .heading_xyzw = {0, 0, 0, 1}, .origin_xyz = {0, 1.6f, 0}}};
    for (unsigned eye = 0; eye < 2; ++eye) {
        findings->eyes[eye] = (PcVrRenderCertificate){.id = 40 + eye, .traversal = 900,
            .units_per_meter = 10, .native_index = eye, .native_snapshot = snapshot};
    }
    findings->scene_seen = findings->scene_bound_seen = true;
    findings->certified = 3;
    findings->placed = 3;
    findings->placement_seen = true;
}

// A frame without a scene whose pose query gave a valid, natively viewed pose and a calibration anchor.
static void anchor_menu(VrFrameInputs *inputs) {
    inputs->pose_valid = inputs->pose_native_views = inputs->anchor_built = true;
    inputs->anchor_local = HELD_GENERATION;
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
    findings->scene_seen = findings->scene_bound_seen = true;  // (given the headset's view, then no certificate)
}
static void scene_with_the_games_camera(VrFrameFindings *findings, VrFrameInputs *inputs) {
    (void)inputs;
    findings->scene_seen = true;  // (the eye layers drawn with the game's own camera, as after a load)
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
// (The same traversal with another generation, tracking frame or pose snapshot cannot happen: one traversal makes
// both certificates from one snapshot. The validation review's step 3 removed those comparisons and their cases.)
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
    findings->eyes[1].traversal = 901;  // (misplaced wins over the disagreement)
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
    inputs->anchor_local = HELD_GENERATION + 1;
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
    inputs->scene_held = inputs->anchor_local = 0;
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
    {"scene drawn with the game's camera", scene_with_the_games_camera, VR_FRAME_REJECTED, VR_FRAME_REJECTED_UNBOUND,
     "unbound"},
    {"only the left certified, unplaced", only_left_certified_unplaced, VR_FRAME_REJECTED,
     VR_FRAME_REJECTED_UNCERTIFIED, "uncertified"},
    {"traversals differ", traversals_differ, VR_FRAME_REJECTED, VR_FRAME_REJECTED_UNCERTIFIED, "uncertified"},
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
                                 1u << VR_FRAME_REJECTED_UNBOUND | 1u << VR_FRAME_REJECTED_MISPLACED |
                                 1u << VR_FRAME_REJECTED_MENU_WITHOUT_POSE));
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
        VR_FRAME_REJECTED_UNCERTIFIED, VR_FRAME_REJECTED_UNBOUND, VR_FRAME_REJECTED_MISPLACED,
        VR_FRAME_REJECTED_MENU_WITHOUT_POSE,
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
    CHECK(!strcmp(vr_frame_kind_name(vr_frame_rejects(VR_FRAME_REJECTED_UNBOUND)), "unbound"));
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
    CHECK(!findings.eyes[0].traversal && !findings.eyes[1].native_snapshot.generation);
    // A reset frame with a filled slot at a game present is a menu frame: it never passes for a scene.
    inputs = (VrFrameInputs){.findings = &findings, .producer_armed = true, .slot_filled = true, .game_present_id = 501};
    CHECK(outcome_is(vr_frame_classify(&inputs), VR_FRAME_REJECTED, VR_FRAME_REJECTED_MENU_WITHOUT_POSE));

    printf("vr_frame fixture: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
