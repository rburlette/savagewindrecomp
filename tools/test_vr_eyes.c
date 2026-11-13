// Asset-free check of the PC VR eye-image producer's slot ownership (runtime/nvn/vulkan/vr_eyes.c, compiled here as
// it is): every slot claimed is offered or released exactly once, and none is released while the XR owner holds it.
// Covers an accepted offer (the slot stays busy until the XR owner's release callback), a refused offer, a rejected
// (uncertified) frame, a UI re-presentation, a present with nothing captured, every slot busy, the image allocation
// failing, a resize while the XR owner holds a slot and after it lets go, the game reset, a scene with its HUD, the
// offer's ownership fields, and a long random sequence of captures, presents, late releases, resizes and resets with
// the ownership invariants checked after every step.
//
// The stubs below stand for what the module calls (the command buffer, image allocation, blits and barriers, deferred
// destruction, the submission numbers, the pose and calibration, the settings, the offer to the XR owner): each records
// its calls and returns what the case scripted. The fixture plays exec.c (the capture's steps and the frame's
// findings, as its draw hooks set them) and the XR owner (it keeps accepted pairs and calls their release callback
// later). The module's one release primitive, the atomic store that clears a slot's busy flag, is observed: the
// fixture checks every release against its own record of who holds the slot. No GPU, OpenXR runtime or game is
// involved. --log keeps the module's log lines (stderr).
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

// (before the interception below: pc_vr_config.c publishes its configuration with atomic_store_explicit too)
#include "test_pc_vr_settings.h"  // (the eye images' row order, SAVAGEWIND_PC_VR_EYE_IMAGES_FLIP: the default)

// The release primitive, observed: vr_eye_slot_release clears busy with atomic_store_explicit, and is the only place
// in vr_eyes.c that uses it.
static void fixture_clear_busy(atomic_bool *busy, bool value, memory_order order);
#undef atomic_store_explicit
#define atomic_store_explicit(object, desired, order) fixture_clear_busy(object, desired, order)
#include "../runtime/nvn/vulkan/vr_eyes.c"
#undef atomic_store_explicit

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

VrEyes vr_eyes;  // (in the runtime exec.c defines it)
VkG g_vk;

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; if (failures < 10) printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

// ------------------------------------------------------------------------------------- what the fixture knows of the slots
// Who holds each slot, by the fixture's own record: nobody, the producer (claimed for the frame being recorded), or
// the XR owner (an accepted offer not yet released).
typedef enum { SLOT_FREE, SLOT_FILLING, SLOT_WITH_XR_OWNER } SlotHolder;
static SlotHolder slot_holder[VR_EYE_SLOTS];
static unsigned long claims, releases, bad_releases;
static bool xr_owner_releasing;  // the fixture is the XR owner calling a release callback

static unsigned slot_index(const VrEyeSlot *slot) { return (unsigned)(slot - vr_eyes.slots); }

static void fixture_clear_busy(atomic_bool *busy, bool value, memory_order order) {
    (void)order;
    const VrEyeSlot *slot = (const VrEyeSlot *)(const void *)((const char *)busy - offsetof(VrEyeSlot, busy));
    const unsigned index = slot_index(slot);
    // A release clears a busy slot, held by whoever releases it: the producer its own slot, the XR owner a slot it took.
    const SlotHolder releaser = xr_owner_releasing ? SLOT_WITH_XR_OWNER : SLOT_FILLING;
    if (value || index >= VR_EYE_SLOTS || !atomic_load(busy) || slot_holder[index] != releaser) {
        ++bad_releases;
    } else {
        slot_holder[index] = SLOT_FREE;
        ++releases;
    }
    atomic_store(busy, value);
}

// ------------------------------------------------------------------------------------------------------------ the stubs
static struct {
    bool blit_supported;      // the GPU can blit the final image's format into the slots' format
    bool allocation_fails;    // the next image allocations fail
    uint64_t pending_seq;     // vk_pending_seq
    uint64_t present_count;   // renderer_present_count
    bool offer_accepted;      // the XR owner's answer to an offer
    bool pose_valid, native_views, anchor_built, panel_builds;
    double hud_depth, menu_depth;  // the panels' distance settings
} script;

static struct {
    unsigned images_created, images_destroyed, images_deferred, layouts, barriers, blits, submits, offers;
    XrOwnedEyePair last_offer;
} calls;

// The pairs the XR owner accepted and has not released yet.
static XrOwnedEyePair held_pairs[VR_EYE_SLOTS * 4];
static unsigned held_pair_count;

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice device, VkFormat format,
                                                               VkFormatProperties *properties) {
    (void)device;
    (void)format;
    *properties = (VkFormatProperties){0};
    if (script.blit_supported)
        properties->optimalTilingFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
}
VkResult vmaCreateImage(VmaAllocator allocator, const VkImageCreateInfo *image_info,
                        const VmaAllocationCreateInfo *memory_info, VkImage *image, VmaAllocation *memory,
                        VmaAllocationInfo *allocation_info) {
    (void)allocator; (void)image_info; (void)memory_info; (void)allocation_info;
    if (script.allocation_fails) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    ++calls.images_created;
    *image = (VkImage)(uintptr_t)(0x1000 + calls.images_created);
    *memory = (VmaAllocation)(uintptr_t)(0x2000 + calls.images_created);
    return VK_SUCCESS;
}
void vmaDestroyImage(VmaAllocator allocator, VkImage image, VmaAllocation memory) {
    (void)allocator; (void)image; (void)memory;
    ++calls.images_destroyed;
}
void vk_defer_image(VkImage image, VmaAllocation memory) {
    (void)image; (void)memory;
    ++calls.images_deferred;
}
VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)(uintptr_t)0xc0; }
void vk_full_barrier(VkCommandBuffer commands) { (void)commands; ++calls.barriers; }
void vk_image_init_layout(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect) {
    (void)commands; (void)image; (void)aspect;
    ++calls.layouts;
}
void rec_CmdBlitImage(VkCommandBuffer commands, VkImage source, VkImageLayout source_layout, VkImage target,
                      VkImageLayout target_layout, uint32_t count, const VkImageBlit *regions, VkFilter filter) {
    (void)commands; (void)source; (void)source_layout; (void)target; (void)target_layout; (void)count; (void)regions;
    (void)filter;
    ++calls.blits;
}
uint64_t vk_pending_seq(void) { return script.pending_seq; }
uint64_t renderer_present_count(void) { return script.present_count; }
void renderer_submit(void) { ++calls.submits; }
bool renderer_origin_lower_left(void) { return false; }
XrPoseReadResult xr_runtime_get_pose(XrPoseSnapshot *out) {
    *out = (XrPoseSnapshot){.generation = 7, .native_views_valid = script.native_views};
    return script.pose_valid ? XR_POSE_VALID : XR_POSE_UNAVAILABLE;
}
bool xr_calibration_anchor(const XrPoseSnapshot *current, XrPoseSnapshot *out) {
    if (script.anchor_built) *out = *current;
    return script.anchor_built;
}
bool xr_calibration_panel(const XrCalibration *calibration, float distance, float rise, float position_xyz[3],
                          float orientation_xyzw[4]) {
    (void)calibration; (void)distance; (void)rise; (void)position_xyz; (void)orientation_xyzw;
    return script.panel_builds;
}
double pc_vr_panel_depth(PcVrPanel which) { return which == PC_VR_PANEL_HUD ? script.hud_depth : script.menu_depth; }
double pc_vr_hud_distance(double depth, double focus_m, double previous_m, double dt_s) {
    (void)depth; (void)focus_m; (void)previous_m; (void)dt_s;
    return 1.4;
}
double pc_vr_camera_focus_distance(void) { return 2.0; }
bool pc_vr_ui_centred(void) { return true; }
uint32_t framepace_pause_state(void) { return 0; }
void rt_fatal_trace(void) { abort(); }

// The XR owner: takes an accepted pair (its slot is then the owner's until it calls release) or refuses it.
bool xr_runtime_offer_eye_pair(const XrOwnedEyePair *pair) {
    ++calls.offers;
    calls.last_offer = *pair;
    if (!script.offer_accepted) return false;
    const unsigned index = slot_index(pair->context);
    if (index >= VR_EYE_SLOTS || slot_holder[index] != SLOT_FILLING) ++bad_releases;  // (an offer of a slot not claimed)
    else slot_holder[index] = SLOT_WITH_XR_OWNER;
    held_pairs[held_pair_count++] = *pair;
    return true;
}

// The XR owner done with its held pair number `which` (oldest first): calls its release callback.
static void xr_owner_release(unsigned which) {
    const XrOwnedEyePair pair = held_pairs[which];
    memmove(&held_pairs[which], &held_pairs[which + 1], (held_pair_count - which - 1) * sizeof held_pairs[0]);
    --held_pair_count;
    xr_owner_releasing = true;
    pair.release(pair.context);
    xr_owner_releasing = false;
}

// ----------------------------------------------------------------------------------------- the fixture as exec.c
static const VkImage hud_image = (VkImage)(uintptr_t)0x4d;  // the HUD's own image (exec.c owns it)

// A producer as at startup, with the stubs' defaults: blits supported, offers accepted, a pose and panels available.
static void fresh_producer(void) {
    memset(&vr_eyes, 0, sizeof vr_eyes);
    memset(slot_holder, 0, sizeof slot_holder);
    memset(&calls, 0, sizeof calls);
    held_pair_count = 0;
    claims = releases = bad_releases = 0;
    script = (__typeof__(script)){.blit_supported = true, .pending_seq = 10, .present_count = 100, .offer_accepted = true,
        .pose_valid = true, .native_views = true, .anchor_built = true, .panel_builds = true, .hud_depth = 1.2,
        .menu_depth = 1.0};
    g_vk = (VkG){.dev = (VkDevice)(uintptr_t)0xde, .qfam = 3, .timeline = (VkSemaphore)(uintptr_t)0x71};
}

// The lens pass of a frame, as exec.c's vr_capture_eyes runs it (without its shots and crosshair sample), only when no
// slot is filled yet this frame (exec.c's draw hooks capture once a frame). Returns the slot filled, or NULL.
static VrEyeSlot *lens_pass(uint32_t width, uint32_t height, bool hud_drawn) {
    if (vr_eyes.filling) return NULL;
    const TexBackend combined = {.image = (VkImage)(uintptr_t)0xf1, .width = width, .height = height};
    VrEyeSlot *slot = vr_eyes_capture_claim(&combined);
    if (!slot) return NULL;
    const unsigned index = slot_index(slot);
    if (index >= VR_EYE_SLOTS || slot_holder[index] != SLOT_FREE) ++bad_releases;  // (claimed while held)
    else slot_holder[index] = SLOT_FILLING;
    ++claims;
    if (!vr_eyes_capture_fill(slot, &combined, hud_image, hud_drawn ? script.present_count + 1 : 0, width, height))
        return NULL;
    vr_eyes_capture_done();
    return slot;
}

// The frame's findings as exec.c's draw hooks leave them: a certified scene pair (of the tracking generation 5), an
// uncertified scene, a misplaced one, or no scene (a menu).
typedef enum { DRAWN_SCENE, DRAWN_UNCERTIFIED_SCENE, DRAWN_MISPLACED_SCENE, DRAWN_MENU } DrawnFrame;
static void draw_frame(DrawnFrame drawn) {
    VrFrameFindings *frame = &vr_eyes.frame;
    vr_frame_findings_reset(frame);
    if (drawn == DRAWN_MENU) return;
    frame->scene_seen = frame->placement_seen = true;
    for (unsigned eye = 0; eye < 2; ++eye) {
        frame->eyes[eye].traversal = 9;
        frame->eyes[eye].native_index = eye;
        frame->eyes[eye].native_snapshot.generation = 5;
    }
    frame->certified = drawn == DRAWN_UNCERTIFIED_SCENE ? 1 : 3;
    frame->placed = drawn == DRAWN_UNCERTIFIED_SCENE ? 1 : 3;
    frame->misplaced = drawn == DRAWN_MISPLACED_SCENE;
}

// The present, as exec.c's vk_vr_present calls the producer, the executor having submitted up to submitted_seq.
// Afterwards the slot filled this frame (if any) must be with the XR owner (an accepted offer) or free (released once),
// and nothing may be filling.
static bool present_submitted(uint64_t game_present_id, uint64_t submitted_seq) {
    VrEyeSlot *filled = vr_eyes.filling;
    const unsigned long releases_before = releases;
    const bool captured_game_frame = vr_eyes_present(game_present_id, submitted_seq);
    ++script.present_count;
    if (filled) {
        const SlotHolder holder = slot_holder[slot_index(filled)];
        if (holder == SLOT_FILLING) ++bad_releases;  // (neither offered nor released)
        if (holder == SLOT_FREE && releases != releases_before + 1) ++bad_releases;
    } else if (releases != releases_before) {
        ++bad_releases;
    }
    if (vr_eyes.filling) ++bad_releases;
    return captured_game_frame;
}

// The usual present: the frame's copies already submitted (backend_present submits the frame first).
static bool present(uint64_t game_present_id) { return present_submitted(game_present_id, script.pending_seq); }

// The ownership invariants: each slot's busy flag says what the fixture's record says; busy slots are claims minus
// releases; no release was of a slot not busy or not held by the releaser.
static bool ownership_holds(void) {
    unsigned long busy_slots = 0;
    bool consistent = true;
    for (unsigned index = 0; index < VR_EYE_SLOTS; ++index) {
        const bool busy = atomic_load(&vr_eyes.slots[index].busy);
        busy_slots += busy;
        consistent &= busy == (slot_holder[index] != SLOT_FREE);
    }
    return consistent && busy_slots == claims - releases && bad_releases == 0 &&
        (!vr_eyes.filling || slot_holder[slot_index(vr_eyes.filling)] == SLOT_FILLING);
}

// A deterministic pseudo-random sequence (xorshift64).
static uint64_t random_state = 0x9e3779b97f4a7c15ull;
static unsigned random_below(unsigned bound) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return (unsigned)(random_state % bound);
}

int main(int argc, char **argv) {
    if (!(argc > 1 && !strcmp(argv[1], "--log")) && !freopen("NUL", "w", stderr)) return 2;
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL});

    // An accepted offer: the slot goes to the XR owner with the offer and stays busy until its release callback.
    fresh_producer();
    VrEyeSlot *slot = lens_pass(1920, 1080, false);
    CHECK(slot && vr_eyes.filling == slot && atomic_load(&slot->busy) && vr_eyes.state == 1);
    CHECK(calls.images_created == 3 && calls.layouts == 3 && vr_eyes.width == 960 && vr_eyes.height == 1080);
    CHECK(vr_eyes.ready_seq == 10 && vr_eyes.image_generation == 1);
    draw_frame(DRAWN_SCENE);
    CHECK(present(500));
    CHECK(calls.offers == 1 && atomic_load(&slot->busy) && slot_holder[slot_index(slot)] == SLOT_WITH_XR_OWNER);
    CHECK(vr_eyes.scene_held == 5 && vr_eyes.accepted == 1 && vr_eyes.filling == NULL);
    // The offer's fields that carry ownership: the claimed slot's images, its release callback and context, and when
    // its copies are complete, on the game's device, queue family and timeline.
    const XrOwnedEyePair *offer = &calls.last_offer;
    CHECK(offer->images[0] == slot->image[0] && offer->images[1] == slot->image[1] &&
          offer->hud_image == VK_NULL_HANDLE);
    CHECK(offer->release == vr_eye_slot_release && offer->context == slot);
    CHECK(offer->ready_value == vr_eyes.ready_seq && offer->ready_timeline == g_vk.timeline);
    CHECK(offer->image_generation == 1 && offer->device == g_vk.dev && offer->queue_family == 3);
    CHECK(offer->width == 960 && offer->height == 1080 && offer->game_present_id == 500);
    CHECK(calls.submits == 0);  // (the copies, ready at submission 10, were submitted already)
    xr_owner_release(0);
    CHECK(!atomic_load(&slot->busy) && releases == 1 && ownership_holds());

    // A refused offer: the slot is released at once, once.
    fresh_producer();
    slot = lens_pass(1920, 1080, false);
    draw_frame(DRAWN_SCENE);
    script.offer_accepted = false;
    CHECK(present(501));
    CHECK(calls.offers == 1 && !atomic_load(&slot->busy) && releases == 1 && vr_eyes.rejected == 1 && ownership_holds());

    // A rejected frame (an uncertified scene): no offer, the slot released.
    fresh_producer();
    slot = lens_pass(1920, 1080, false);
    draw_frame(DRAWN_UNCERTIFIED_SCENE);
    CHECK(present(502));
    CHECK(calls.offers == 0 && !atomic_load(&slot->busy) && releases == 1 && vr_eyes.uncertified == 1);
    CHECK(ownership_holds());

    // A UI re-presentation: the slot released without an offer, and not a captured game frame.
    fresh_producer();
    slot = lens_pass(1920, 1080, false);
    draw_frame(DRAWN_SCENE);
    CHECK(!present(0));
    CHECK(calls.offers == 0 && !atomic_load(&slot->busy) && releases == 1 && vr_eyes.offered == 0 && ownership_holds());

    // Nothing captured: nothing offered, nothing released.
    fresh_producer();
    draw_frame(DRAWN_SCENE);
    CHECK(!present(503));
    CHECK(calls.offers == 0 && releases == 0 && claims == 0 && ownership_holds());

    // Every slot with the XR owner: the next lens pass claims nothing and counts a busy skip.
    fresh_producer();
    for (unsigned frame = 0; frame < VR_EYE_SLOTS; ++frame) {
        CHECK(lens_pass(1920, 1080, false) != NULL);
        draw_frame(DRAWN_MENU);
        CHECK(present(504 + frame));
    }
    CHECK(held_pair_count == VR_EYE_SLOTS && calls.offers == VR_EYE_SLOTS);
    CHECK(lens_pass(1920, 1080, false) == NULL && vr_eyes.busy_skips == 1 && claims == VR_EYE_SLOTS);
    CHECK(ownership_holds());
    xr_owner_release(2);  // (one let go, late and out of order: that slot is claimed next)
    slot = lens_pass(1920, 1080, false);
    CHECK(slot && slot_index(slot) == 2 && ownership_holds());

    // The image allocation failing: the producer off for good, the slot released, nothing filling.
    fresh_producer();
    script.allocation_fails = true;
    CHECK(lens_pass(1920, 1080, false) == NULL);
    CHECK(vr_eyes.state == -1 && vr_eyes.filling == NULL && claims == 1 && releases == 1 && ownership_holds());
    script.allocation_fails = false;
    CHECK(lens_pass(1920, 1080, false) == NULL && claims == 1);  // (off: no claim)

    // A resize while the XR owner holds a slot: no reallocation and no claim; once it is free, the old images go
    // through deferred destruction and a new image generation starts.
    fresh_producer();
    slot = lens_pass(1920, 1080, false);
    draw_frame(DRAWN_SCENE);
    CHECK(present(510));
    CHECK(lens_pass(2560, 1440, false) == NULL);
    CHECK(vr_eyes.busy_skips == 1 && calls.images_deferred == 0 && vr_eyes.image_generation == 1 && claims == 1);
    CHECK(vr_eyes.width == 960 && ownership_holds());
    xr_owner_release(0);
    slot = lens_pass(2560, 1440, false);
    CHECK(slot && calls.images_deferred == 3 && vr_eyes.image_generation == 2 && vr_eyes.width == 1280);
    CHECK(vr_eyes.height == 1440 && vr_eyes.resized == 1 && calls.images_created == 6 && ownership_holds());
    draw_frame(DRAWN_SCENE);
    CHECK(present(511) && calls.last_offer.image_generation == 2 && calls.last_offer.width == 1280);

    // The game reset: releases the slot being filled and forgets the held scene, but leaves the XR owner's slot.
    fresh_producer();
    VrEyeSlot *held = lens_pass(1920, 1080, false);
    draw_frame(DRAWN_SCENE);
    CHECK(present(520) && vr_eyes.scene_held == 5);
    VrEyeSlot *filling = lens_pass(1920, 1080, false);
    CHECK(filling && filling != held);
    vr_eyes_game_reset();
    CHECK(!atomic_load(&filling->busy) && atomic_load(&held->busy) && vr_eyes.filling == NULL);
    CHECK(vr_eyes.scene_held == 0 && !vr_eyes.frame.scene_seen && ownership_holds());
    xr_owner_release(0);
    CHECK(ownership_holds() && claims == releases);

    // A scene with its HUD: the slot's HUD image goes with the offer, on its panel. Then copies not yet submitted at
    // the present: submitted before the offer.
    fresh_producer();
    slot = lens_pass(1920, 1080, true);
    CHECK(slot && slot->hud && calls.blits == 3);
    draw_frame(DRAWN_SCENE);
    CHECK(present(530));
    CHECK(calls.last_offer.hud_image == slot->image[2] && calls.last_offer.hud.valid && calls.submits == 0);
    xr_owner_release(0);
    slot = lens_pass(1920, 1080, false);
    CHECK(slot && !slot->hud);
    draw_frame(DRAWN_SCENE);
    CHECK(present_submitted(531, vr_eyes.ready_seq - 1) && calls.submits == 1 && calls.offers == 2);
    CHECK(calls.last_offer.hud_image == VK_NULL_HANDLE && ownership_holds());

    // A long random sequence: lens passes, presents of every kind with the XR owner accepting or refusing, the owner
    // releasing late and out of order, resizes and game resets. The invariants after every step.
    fresh_producer();
    const uint32_t sizes[2][2] = {{1920, 1080}, {2560, 1440}};
    unsigned size = 0, steps = 0, broken_at = 0;
    unsigned long offers_accepted = 0;
    for (unsigned step = 1; step <= 50000; ++step) {
        const unsigned event = random_below(100);
        if (event < 30) {
            lens_pass(sizes[size][0], sizes[size][1], random_below(2));
            script.pending_seq += random_below(2);
        } else if (event < 60) {
            draw_frame((DrawnFrame)random_below(4));
            script.offer_accepted = random_below(5) != 0;
            const unsigned long offers_before = calls.offers;
            present(random_below(20) ? 600 + step : 0);
            offers_accepted += script.offer_accepted && calls.offers != offers_before;
        } else if (event < 92) {
            if (held_pair_count) xr_owner_release(random_below(held_pair_count));
        } else if (event < 95) {
            size ^= 1;  // (a window resize: the next lens passes see the new size)
        } else {
            vr_eyes_game_reset();
        }
        ++steps;
        if (!ownership_holds() && !broken_at) broken_at = step;
    }
    while (held_pair_count) xr_owner_release(0);
    if (vr_eyes.filling) vr_eyes_game_reset();
    CHECK(broken_at == 0);
    CHECK(ownership_holds() && claims == releases && claims > 1000 && offers_accepted > 1000);
    CHECK(vr_eyes.resized > 10 && vr_eyes.busy_skips > 0);
    if (broken_at) printf("  ownership broken at random step %u\n", broken_at);
    printf("vr_eyes random sequence: %u steps, %lu claims, %lu releases, %lu offers accepted, %llu resizes\n", steps,
           claims, releases, offers_accepted, (unsigned long long)vr_eyes.resized);

    printf("vr_eyes fixture: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
