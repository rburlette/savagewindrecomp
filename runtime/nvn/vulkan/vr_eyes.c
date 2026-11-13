// PC VR renderer: the eye-image producer (D048 onward). Its life cycle, threading, image ownership and each function's
// preconditions: vr_eyes.h. Moved here from exec.c; the bodies are exec.c's.
#include <SDL3/SDL_timer.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "record.h"
#include "vr_eyes.h"
#include "../renderer.h"
#include "../../pc_vr.h"
#include "../../pc_vr_hud.h"
#include "../../xr/xr_vulkan.h"

uint64_t renderer_present_count(void);  // (device.c) the presents so far: the frame being recorded is this plus one
void renderer_submit(void);             // (exec.c) submits the commands recorded so far

// ------------------------------------------------------------------------------- VR eye images: owned images
// Runs on the XR owner thread once its copy of the slot's images completed (or here, when an offer is not taken):
// hands the slot back.
static void vr_eye_slot_release(void *context) {
    atomic_store_explicit(&((VrEyeSlot *)context)->busy, false, memory_order_release);
}

// Arms the producer on first use. Off (for good) when the GPU cannot blit A2B10G10R10 into sRGB RGBA8.
static bool vr_eyes_configure(void) {
    if (vr_eyes.state) return vr_eyes.state > 0;
    vr_eyes.state = -1;
    VkFormatProperties source, target;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_A2B10G10R10_UNORM_PACK32, &source);
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_R8G8B8A8_SRGB, &target);
    if (!(source.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(target.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        LOG("[pc-vr-eyes] producer disabled: A2B10G10R10 to sRGB RGBA8 blit unsupported\n");
        return false;
    }
    // Rows in the window present's order; SAVAGEWIND_PC_VR_EYE_IMAGES_FLIP=0/1 overrides it for the headset check.
    const int flip = pc_vr_config()->play.eye_images_flip;
    vr_eyes.flip = flip >= 0 ? flip == 1 : renderer_origin_lower_left();
    // The final image holds linear values: the game's lens pass writes it to the display through an sRGB view.
    // Blitting into an sRGB image encodes them, which also keeps dark tones' precision in 8 bits.
    vr_eyes.state = 1;
    LOG("[pc-vr-eyes] producer armed source=final-image-halves slots=%u format=R8G8B8A8_SRGB encode=linear-to-srgb row_flip=%d provenance=dev-unproven\n",
        VR_EYE_SLOTS, vr_eyes.flip);
    return true;
}

// Allocates the slot's three images (both eyes and the HUD) at the current half size, if it has none yet.
static bool vr_eye_slot_images(VrEyeSlot *slot, VkCommandBuffer commands) {
    if (slot->image[0]) return true;
    for (unsigned i = 0; i < 3; ++i) {
        VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_SRGB,
            .extent = {vr_eyes.width, vr_eyes.height, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo memory_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &image_info, &memory_info, &slot->image[i], &slot->memory[i], NULL) !=
            VK_SUCCESS) {
            // (the images made so far were never recorded into: they can go at once)
            for (unsigned made = 0; made < i; ++made) vmaDestroyImage(g_vk.vma, slot->image[made], slot->memory[made]);
            memset(slot->image, 0, sizeof slot->image);
            return false;
        }
    }
    for (unsigned i = 0; i < 3; ++i) vk_image_init_layout(commands, slot->image[i], VK_IMAGE_ASPECT_COLOR_BIT);
    return true;
}

// ----------------------------------------------------------------------------------- VR eye images: the per-frame copy
// At the lens pass, which reads the finished final image (combined): copies both halves into a free slot's eye images,
// and the HUD's own image into its HUD image, once per frame. The copies complete at vr_eyes.ready_seq. exec.c's
// vr_capture_eyes runs the steps: claim, end its render pass, fill, its shots and the crosshair's depth, done.
VrEyeSlot *vr_eyes_capture_claim(const TexBackend *combined) {
    if (!vr_eyes_configure()) return NULL;
    const uint32_t half_width = combined->width / 2, height = combined->height;
    if (!half_width || !height) return NULL;
    if (half_width != vr_eyes.width || height != vr_eyes.height) {
        // A window resize rescales the final image. Reallocate only while XR holds none of the old images.
        for (unsigned i = 0; i < VR_EYE_SLOTS; ++i) {
            if (atomic_load(&vr_eyes.slots[i].busy)) {
                ++vr_eyes.busy_skips;
                return NULL;
            }
        }
        for (unsigned i = 0; i < VR_EYE_SLOTS; ++i) {
            VrEyeSlot *slot = &vr_eyes.slots[i];
            for (unsigned image = 0; slot->image[0] && image < 3; ++image)
                vk_defer_image(slot->image[image], slot->memory[image]);
            memset(slot->image, 0, sizeof slot->image);
            memset(slot->memory, 0, sizeof slot->memory);
        }
        if (vr_eyes.width) ++vr_eyes.resized;
        vr_eyes.width = half_width;
        vr_eyes.height = height;
        ++vr_eyes.image_generation;
    }
    // Claim a slot the XR owner does not hold.
    for (unsigned i = 0; i < VR_EYE_SLOTS && !vr_eyes.filling; ++i) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&vr_eyes.slots[i].busy, &expected, true))
            vr_eyes.filling = &vr_eyes.slots[i];
    }
    if (!vr_eyes.filling) {
        ++vr_eyes.busy_skips;
        return NULL;
    }
    return vr_eyes.filling;
}

bool vr_eyes_capture_fill(VrEyeSlot *slot, const TexBackend *combined, VkImage hud_image, uint64_t hud_drawn,
                          uint32_t hud_width, uint32_t hud_height) {
    const uint32_t half_width = combined->width / 2, height = combined->height;
    VkCommandBuffer commands = vk_cmd();
    if (!vr_eye_slot_images(slot, commands)) {
        LOG("[pc-vr-eyes] producer disabled: owned image allocation failed\n");
        vr_eye_slot_release(slot);
        vr_eyes.filling = NULL;
        vr_eyes.state = -1;
        return false;
    }
    vk_full_barrier(commands);
    const int32_t blit_width = (int32_t)half_width, blit_height = (int32_t)height;
    for (unsigned eye = 0; eye < 2; ++eye) {
        VkImageBlit blit = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{(int32_t)eye * blit_width, 0, 0}, {((int32_t)eye + 1) * blit_width, blit_height, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{0, vr_eyes.flip ? blit_height : 0, 0},
                           {blit_width, vr_eyes.flip ? 0 : blit_height, 1}}};
        rec_CmdBlitImage(commands, combined->image, VK_IMAGE_LAYOUT_GENERAL, slot->image[eye], VK_IMAGE_LAYOUT_GENERAL,
                       1, &blit, VK_FILTER_NEAREST);
    }
    // D057: this frame's HUD, the left half of its own image (one image for both eyes, like the menus).
    slot->hud = hud_drawn == renderer_present_count() + 1 &&
        hud_width == combined->width && hud_height == combined->height;
    if (slot->hud) {
        VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .srcOffsets = {{0, 0, 0}, {blit_width, blit_height, 1}},
                            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .dstOffsets = {{0, vr_eyes.flip ? blit_height : 0, 0},
                                           {blit_width, vr_eyes.flip ? 0 : blit_height, 1}}};
        rec_CmdBlitImage(commands, hud_image, VK_IMAGE_LAYOUT_GENERAL, slot->image[2],
                         VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
    }
    vk_full_barrier(commands);
    return true;
}

void vr_eyes_capture_done(void) {
    vr_eyes.ready_seq = vk_pending_seq();
    ++vr_eyes.captured;
}

// ---------------------------------------------------------------------------------------- VR eye images: the 2D panels
// (The screen and its size: VR_SCREEN_HALF_W, VR_SCREEN_HALF_H, VR_SCREEN_WIDTH in vr_eyes.h.)
// Fills *out with the panel for `which` (menu or HUD) at `distance` metres from the anchor pose. `setting` is the
// panel's setting, for the log. False: no panel (distance 0, no images yet, or no calibration).
static bool vr_panel(const XrPoseSnapshot *anchor, double distance, double setting, PcVrPanel which,
                              XrEyePanel *out) {
    if (!distance || !vr_eyes.width || !vr_eyes.height) return false;
    // The crop rectangle, from guest pixels of the half to the owned images' pixels.
    const double scale_x = vr_eyes.width / 960.0, scale_y = vr_eyes.height / 1080.0;
    // Without the game's UI parallax (runtime/pc_vr_hud.c rt_ui_parallax) the screen lies about the half's centre and
    // all of its 16:9 width is inside the half. With it (a game library without the patch), about the lens centre,
    // reaching the half's nasal edge: center_x + VR_SCREEN_HALF_W.
    const bool centred = pc_vr_ui_centred();
    const double center_x = centred ? 480.0 : 480.0 * (1.0 + VR_LENS_CENTER_NDC);
    const double half_w = centred ? VR_SCREEN_HALF_H * 16.0 / 9.0 : VR_SCREEN_HALF_W;
    const long left = lround((center_x - half_w) * scale_x);
    const long right = lround((center_x + half_w) * scale_x);
    const long top = lround((540.0 - VR_SCREEN_HALF_H) * scale_y);
    const long bottom = lround((540.0 + VR_SCREEN_HALF_H) * scale_y);
    XrEyePanel panel = {.valid = true, .eye = 0,
        .rect = {(int32_t)left, (int32_t)top, (int32_t)(right - left), (int32_t)(bottom - top)}};
    // (each guest pixel the same size either way: the wider crop is a wider panel)
    panel.width_m = (float)(distance * VR_SCREEN_WIDTH * half_w / VR_SCREEN_HALF_W);
    panel.height_m = (float)(distance * VR_SCREEN_WIDTH * 9.0 / 16.0);
    // (at the vertical centre of the eye's FOV: fov[2] and fov[3] are the angles of its top and bottom edges)
    const float *fov = anchor->eyes[0].fov_left_right_up_down;
    const double rise = distance * 0.5 * (tan(fov[2]) + tan(fov[3]));
    if (!xr_calibration_panel(&anchor->calibration, (float)distance, (float)rise,
                              panel.position_xyz, panel.orientation_xyzw)) return false;
    // Logged twice, after each setting change, and for the HUD every 600th panel, with the camera target's distance
    // (near Link) at the current world scale, which the HUD distance follows.
    static unsigned logged[2];
    static double logged_setting[2];
    const unsigned hud = which == PC_VR_PANEL_HUD;
    if (logged[hud] < 2 || setting != logged_setting[hud] || (hud && !(logged[hud] % 600))) {
        logged_setting[hud] = setting;
        LOG("[pc-vr-eyes] %s panel distance_m=%.2f size_m=%.3fx%.3f rise_m=%.3f rect=%d,%d,%dx%d image=%ux%u camera_target_m=%.3f\n",
            hud ? "hud" : "ui", distance, panel.width_m, panel.height_m, rise, panel.rect[0], panel.rect[1],
            panel.rect[2], panel.rect[3], vr_eyes.width, vr_eyes.height, pc_vr_camera_focus_distance());
    }
    ++logged[hud];
    *out = panel;
    return true;
}

// ----------------------------------------------------------------------------- VR eye images: offering the frame to XR
// At each present, after the present's submission (so this frame's draws come before it in queue order): takes this
// frame's findings, resets them for the next frame, classifies the frame (vr_frame_classify, vr_frame.h) and offers the
// filled slot to the XR owner thread as one of:
//   scene             both eyes certified by one camera traversal and each placed in its own half: the eye images,
//                     with the pose they were rendered for;
//   scene-hud         the same, with the HUD's image on its panel over them (D057);
//   menu-panel        no scene: the left half on the menu panel, anchored at the calibration (D056);
//   quick-over-scene  a quick menu over the held scene: the HUD's image on its panel over the last scene pair (D072).
// Anything else (an uncertified or misplaced scene, no pose for a menu) is not offered, and the slot is released.
// vr_eyes_present runs three steps: take, check, offer.

// Hands over this frame's slot (NULL: none filled) and findings (*frame), and clears both for the next frame.
static VrEyeSlot *vr_eyes_present_take(VrFrameFindings *frame) {
    VrEyeSlot *slot = vr_eyes.filling;
    *frame = vr_eyes.frame;  // (this frame's findings, kept for its classification)
    // Reset for the next frame.
    vr_eyes.filling = NULL;
    vr_frame_findings_reset(&vr_eyes.frame);
    return slot;
}

// Fills *inputs with the classification's inputs that need no query (vr_frame.h) and says whether the frame goes on to
// an offer: false when nothing was captured, and for a UI re-presentation (game_present_id 0), whose slot is released
// here. frame must stay alive until vr_eyes_present_offer returns (inputs points to it).
static bool vr_eyes_present_check(VrEyeSlot *slot, const VrFrameFindings *frame, uint64_t game_present_id,
                                  VrFrameInputs *inputs) {
    // The classification's inputs that need no query; the pose and the HUD setting follow once the frame is known to
    // offer something.
    *inputs = (VrFrameInputs){.findings = frame, .producer_armed = vr_eyes.state > 0, .slot_filled = slot != NULL,
        .slot_has_hud = slot && slot->hud, .game_present_id = game_present_id, .scene_held = vr_eyes.scene_held,
        .hud_distance_m = vr_eyes.hud_distance_m};
    const VrFrameRejection not_offered = vr_frame_not_offered(inputs);
    if (not_offered == VR_FRAME_REJECTED_NOT_CAPTURED) return false;
    if (not_offered == VR_FRAME_REJECTED_NOT_A_GAME_FRAME) {  // (a UI re-presentation, not a game frame)
        vr_eye_slot_release(slot);
        return false;
    }
    return true;
}

// Classifies the frame and offers the slot to the XR owner, or releases it: submits the copies first when they are not
// submitted yet (submitted_seq: the executor's last submitted number), queries the pose for a frame without a scene,
// builds the panel the kind needs, eases the HUD distance on a scene with its HUD, counts and logs. Only after
// vr_eyes_present_check returned true.
static void vr_eyes_present_offer(VrEyeSlot *slot, VrFrameInputs inputs, uint64_t submitted_seq) {
    const VrFrameFindings *frame = inputs.findings;
    const uint64_t game_present_id = inputs.game_present_id;
    if (submitted_seq < vr_eyes.ready_seq) renderer_submit();  // (the copies must be submitted before XR waits)
    XrOwnedEyePair offer = {.device = g_vk.dev, .queue_family = g_vk.qfam,
        .images = {slot->image[0], slot->image[1]}, .format = VK_FORMAT_R8G8B8A8_SRGB,
        .width = vr_eyes.width, .height = vr_eyes.height,
        .ready_timeline = g_vk.timeline, .ready_value = vr_eyes.ready_seq,
        .game_present_id = game_present_id, .image_generation = vr_eyes.image_generation,
        .projection = XR_EYE_PROJECTION_DEV_UNPROVEN,
        .release = vr_eye_slot_release, .context = slot};
    if (!frame->scene_seen) {
        // Menus (D053): world-locked at the calibration pose, where the game's forward is, through tracking
        // loss and reopening; each offer carries the current tracking generation. Shown only as quads (D056, D068):
        // the game's Labo halves would ask the eyes to diverge on the projection layer. (A scene frame queries no pose.)
        XrPoseSnapshot pose;
        inputs.pose_valid = xr_runtime_get_pose(&pose) == XR_POSE_VALID;
        inputs.pose_native_views = inputs.pose_valid && pose.native_views_valid;
        inputs.anchor_built = inputs.pose_native_views && xr_calibration_anchor(&pose, &offer.render_pose);
        inputs.anchor_generation = offer.render_pose.generation;
        offer.anchored = inputs.anchor_built;
    }
    // The HUD setting, read where the frame can use it: a scene pair with this frame's HUD, or an anchored menu frame.
    if (frame->scene_seen ? slot->hud && vr_frame_eyes_agree(frame) : inputs.anchor_built)
        inputs.hud_panel_depth = pc_vr_panel_depth(PC_VR_PANEL_HUD);
    VrFrameOutcome outcome = vr_frame_classify(&inputs);
    // The panel the kind needs; where it cannot be built, what the frame becomes without it (vr_frame_without_panel).
    if (outcome.kind == VR_FRAME_SCENE || outcome.kind == VR_FRAME_SCENE_WITH_HUD)
        offer.render_pose = frame->eyes[0].native_snapshot;
    if (outcome.kind == VR_FRAME_SCENE_WITH_HUD) {
        // D057: the HUD drawn into its own image goes on a quad over the scene, at its setting times the camera
        // target's distance (near Link), eased between presents.
        const uint64_t now = SDL_GetTicksNS();
        vr_eyes.hud_distance_m = pc_vr_hud_distance(inputs.hud_panel_depth, pc_vr_camera_focus_distance(),
            vr_eyes.hud_distance_m, vr_eyes.hud_placed_ns ? (now - vr_eyes.hud_placed_ns) * 1e-9 : INFINITY);
        vr_eyes.hud_placed_ns = now;
        if (vr_panel(&offer.render_pose, vr_eyes.hud_distance_m, inputs.hud_panel_depth, PC_VR_PANEL_HUD, &offer.hud))
            offer.hud_image = slot->image[2];
        else outcome = vr_frame_without_panel(outcome);
    }
    if (outcome.kind == VR_FRAME_HUD_OVER_HELD_SCENE) {
        // A quick menu's HUD image on the HUD's panel, where it was while the menu opened over the scene, over
        // the held scene (vr_quick_menu_over_world).
        if (vr_panel(&offer.render_pose, vr_eyes.hud_distance_m, inputs.hud_panel_depth, PC_VR_PANEL_HUD,
                     &offer.panel)) {
            offer.over_scene = true;
            offer.hud_image = slot->image[2];
        } else outcome = vr_frame_without_panel(outcome);
    }
    if (outcome.kind == VR_FRAME_MENU_PANEL) {
        const double menu_distance = pc_vr_panel_depth(PC_VR_PANEL_MENU);
        if (!vr_panel(&offer.render_pose, menu_distance, menu_distance, PC_VR_PANEL_MENU, &offer.panel))
            outcome = vr_frame_without_panel(outcome);
    }
    if (outcome.kind == VR_FRAME_HUD_OVER_HELD_SCENE || outcome.kind == VR_FRAME_MENU_PANEL) ++vr_eyes.menu_pairs;
    else if (outcome.rejection == VR_FRAME_REJECTED_MISPLACED) ++vr_eyes.misplaced_frames;
    else if (outcome.rejection == VR_FRAME_REJECTED_UNCERTIFIED) ++vr_eyes.uncertified;
    const bool ready = outcome.kind != VR_FRAME_REJECTED;
    const char *kind = vr_frame_kind_name(outcome);
    memcpy(offer.rendered_eyes, offer.render_pose.eyes, sizeof offer.rendered_eyes);
    const bool accepted = ready && xr_runtime_offer_eye_pair(&offer);
    ++vr_eyes.offered;
    if (accepted) {
        ++vr_eyes.accepted;
        if (offer.over_scene) ++vr_eyes.over_pairs;
        else vr_eyes.scene_held = offer.anchored ? 0 : offer.render_pose.generation;  // (a menu panel replaces it)
    } else {
        ++vr_eyes.rejected;
        vr_eye_slot_release(slot);
    }
    // Each change of kind, for the first 200 (the quick menus' and menus' frames between the periodic lines).
    if (kind != vr_eyes.last_kind && vr_eyes.kind_changes < 200) {
        extern uint32_t framepace_pause_state(void);
        ++vr_eyes.kind_changes;
        LOG("[pc-vr-eyes] kind %s -> %s at present=%llu accepted=%d pause_state=%u over_pairs=%llu\n",
            vr_eyes.last_kind ? vr_eyes.last_kind : "none", kind, (unsigned long long)game_present_id, accepted,
            framepace_pause_state(), (unsigned long long)vr_eyes.over_pairs);
    }
    vr_eyes.last_kind = kind;
    if (vr_eyes.offered <= 8 || !(vr_eyes.offered % 120) || (!accepted && vr_eyes.rejected <= 16))
        LOG("[pc-vr-eyes] offer=%llu present=%llu kind=%s traversal=%llu generation=%llu accepted=%d accepted_total=%llu rejected=%llu menu_pairs=%llu uncertified=%llu misplaced=%llu busy_skips=%llu resized=%llu half=%ux%u provenance=dev-unproven\n",
            (unsigned long long)vr_eyes.offered, (unsigned long long)game_present_id, kind,
            (unsigned long long)(frame->scene_seen ? frame->eyes[0].traversal : 0),
            (unsigned long long)offer.render_pose.generation, accepted,
            (unsigned long long)vr_eyes.accepted, (unsigned long long)vr_eyes.rejected,
            (unsigned long long)vr_eyes.menu_pairs, (unsigned long long)vr_eyes.uncertified,
            (unsigned long long)vr_eyes.misplaced_frames, (unsigned long long)vr_eyes.busy_skips,
            (unsigned long long)vr_eyes.resized, vr_eyes.width, vr_eyes.height);
}

bool vr_eyes_present(uint64_t game_present_id, uint64_t submitted_seq) {
    VrFrameFindings frame;  // (this frame's findings, kept for its classification)
    VrEyeSlot *slot = vr_eyes_present_take(&frame);
    VrFrameInputs inputs;  // (the classification's inputs: vr_eyes_present_check fills them, _offer completes them)
    if (!vr_eyes_present_check(slot, &frame, game_present_id, &inputs)) return false;
    vr_eyes_present_offer(slot, inputs, submitted_seq);
    return true;
}

// ------------------------------------------------------------------------------------------------- the game reset
void vr_eyes_game_reset(void) {
    // PC VR: forget the frame in progress and the previous frame's eyes. (Pairs the XR owner accepted come back
    // through its release callback.)
    if (vr_eyes.filling) vr_eye_slot_release(vr_eyes.filling);
    vr_eyes.filling = NULL;
    vr_frame_findings_reset(&vr_eyes.frame);
    vr_eyes.scene_held = 0;
}
