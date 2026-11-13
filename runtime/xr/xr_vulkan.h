#ifndef SAVAGEWIND_XR_VULKAN_H
#define SAVAGEWIND_XR_VULKAN_H
#include <stdbool.h>
#include <vulkan/vulkan.h>
#include "xr_runtime.h"

typedef enum {
    XR_EYE_PROJECTION_UNPROVEN = 0,
    XR_EYE_PROJECTION_NATIVE_RENDERED = 1,
    // D048, the PC VR eye images only: native view/FOV certificates reached both
    // scene-final draws, but shader consumption and full coverage remain
    // unproven. Admitted only in the PC VR play configuration (pc_vr_config.h).
    XR_EYE_PROJECTION_DEV_UNPROVEN = 2
} XrEyeProjectionProvenance;

// One immutable, owned GPU pair, not borrowed guest textures. Both images must
// be single-sample 2D color images, mip/layer zero, TRANSFER_SRC usage, GENERAL
// layout, on this exact device and queue family. Producer submission MUST
// already exist and signal ready_value on ready_timeline. Neither images nor
// semaphore may be destroyed/reused until release. The producer establishes
// both eyes belong to one game state; present identity alone cannot prove it.
//
// NATIVE_RENDERED is a renderer assertion: native runtime eye transforms/FOV
// were actually used for all corresponding image/culling consumers. Original
// Labo views and a copied tracking snapshot MUST remain UNPROVEN. rendered_eyes
// are the exact runtime-space poses/FOV used to render, before the game's
// third-person world-anchor conversion; they must match render_pose.eyes.
//
// true transfers ownership; false retains ownership with the caller. The
// mailbox queues up to two pairs (EYE_QUEUE, D081) and refuses an offer while
// two are pending, never replacing an owned pair; each display frame the owner
// copies the newest finished pair and returns the older ones unshown.
// The sole XR owner invokes release once after producer completion and any XR copy fence,
// including stale/reset/drop paths. release must only enqueue thread-safe
// deferred retirement; it must not use renderer/VMA state from the XR thread.
// Device loss also permits retirement; unexpected GPU-wait errors quarantine
// the pair until shutdown can establish completion. Offer only after the
// submission that signals ready_value. The display-rate owner picks offers up
// at its next frame; present notifications never authorize image submission.
// render_pose must be in the current reference generation and fresh; it need
// not be the latest publication.
//
// display_encoded: pixels already hold display (sRGB) code values in a UNORM
// format. They reach an _SRGB swapchain by a raw size-compatible copy.
//
// anchored: 2D content (menus) with no render pose. render_pose is a tracking
// sample latched to place it; freshness counts from the offer, not the sample.
// Only DEV_UNPROVEN pairs may be anchored.
//
// panel (anchored only, D056): instead of the projection layer, show `rect`
// (x, y, width, height in pixels) of image `eye` to both eyes on one world-
// locked quad layer in LOCAL (pose, size in meters), so moving the head
// toward or around 2D content changes it like an object.
typedef struct {
    bool valid;
    uint32_t eye;
    int32_t rect[4];
    float position_xyz[3], orientation_xyzw[4];
    float width_m, height_m;
} XrEyePanel;

typedef struct {
    VkDevice device;
    uint32_t queue_family;
    VkImage images[2];
    VkFormat format;
    uint32_t width, height;
    VkSemaphore ready_timeline;
    uint64_t ready_value;
    uint64_t game_present_id, image_generation;
    XrPoseSnapshot render_pose;
    XrEyeSnapshot rendered_eyes[2];
    XrEyeProjectionProvenance projection;
    bool display_encoded, anchored;
    XrEyePanel panel;
    // hud (scene pairs only, D057): hud_image, owned like images[], holds the
    // 2D overlay with premultiplied alpha; it is shown on one world-locked quad
    // over the projection layer (hud.rect, pose, size; hud.eye unused).
    VkImage hud_image;
    XrEyePanel hud;
    // over_scene (panel pairs only): the panel shows hud_image, premultiplied,
    // instead of image `eye`, over the projection of the last scene pair, which
    // is held (not acquired again) while the game draws no scene (the quick
    // menus); without one in this reference generation, the panel alone.
    bool over_scene;
    void (*release)(void *context);
    void *context;
} XrOwnedEyePair;

bool xr_runtime_offer_eye_pair(const XrOwnedEyePair *pair);
// The PC VR play configuration's eye images (pc_vr_config.h); read once per bound session.
bool xr_runtime_eye_images_enabled(void);

// Explicit PC VR mode must fail if discovery/admission fails; no desktop fallback.
bool xr_runtime_discover(void);
VkResult xr_runtime_create_vulkan_instance(const VkInstanceCreateInfo *info, VkInstance *instance);
bool xr_runtime_select_vulkan_device(VkInstance instance, VkPhysicalDevice *physical);
VkResult xr_runtime_create_vulkan_device(VkPhysicalDevice physical,
                                       const VkDeviceCreateInfo *info, VkDevice *device);
// queue_index must be reserved exclusively for XR on the game's actual device.
bool xr_runtime_bind_vulkan(VkInstance instance, VkPhysicalDevice physical,
                            VkDevice device, uint32_t queue_family, uint32_t queue_index);
#endif
