// PC VR renderer: diagnostic image shots (D048 onward). What a shot is, when one is taken, what it costs and the
// preconditions: vr_shots.h. Moved here from exec.c; the bodies are exec.c's.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "vk.h"
#include "record.h"
#include "vr_shots.h"
#include "../labo_premask_capture.h"
#include "../../pc_vr_config.h"

uint64_t renderer_present_count(void);  // (device.c) the presents so far: the frame being recorded is this plus one
void renderer_submit(void);             // (exec.c) submits the commands recorded so far

#define VR_SHOT_REQUESTS PC_VR_SHOT_PRESENTS  // SAVAGEWIND_PC_VR_SHOT: the most presents one run can ask for

// SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS=1 (diagnostic, off by default): what the headset shows while a quick menu or a shop
// holds the scene (D072, exec.c vr_quick_menu_over_world), saved without leaving the game. At the
// VR_QUICK_MENU_SHOT_DELAY-th present over the held scene in a row (the menu's opening animation over), for the first
// VR_QUICK_MENU_SHOTS_MAX such menus of the run, as build/vr-shot-quick-menu<n>-<image>-<present>.png in the data folder:
//   panel        the HUD's image this frame, which the XR owner puts on the panel over the held scene (slot image 2),
//                premultiplied, over black;
//   panel-alpha  its coverage, as grey: where the panel covers the held scene;
//   final0/1     this frame's final image halves (slot images 0 and 1): what the game drew besides its UI screens'
//                draws. The headset does not show them over the held scene: its eyes keep the last scene pair's
//                images, which live in the XR owner's swapchain and are not saved here.
// One log line per menu, with the world-anchored HUD draws left out in that frame (exec.c draw_).
#define VR_QUICK_MENU_SHOT_DELAY 30
#define VR_QUICK_MENU_SHOTS_MAX 4u

// Everything the shots remember, for the process (vr_shots.h: why there is no reset).
static struct {
    // SAVAGEWIND_PC_VR_SHOT (the configuration's shot_presents): per requested present, a bit for each kind of shot
    // already saved for it (vr_shot_bit).
    unsigned kinds_saved[VR_SHOT_REQUESTS];
    // The uncertified scene shots (vr_shot_scene_side): the present they name sides in, and the finished image (by
    // serial) that took each side in it (0: none yet).
    uint64_t scene_side_present;
    uint64_t scene_side_outputs[2];
    // SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS.
    uint64_t held_presents;      // the presents over the held scene in a row before this one (exec.c vk_vr_present)
    bool shot_this_hold;         // this run of presents over the held scene has its shot
    unsigned quick_menus_taken;  // the menus captured so far
} vr_shots;

// ------------------------------------------------------------------------------------------------- saving an image
// One 10-bit channel as an 8-bit PNG value: rescaled, or sRGB-encoded when srgb is set (encode A2B10G10R10 values,
// which are linear, as the headset shows them).
static uint8_t vr_shot_encode(uint32_t channel, bool srgb) {
    if (!srgb) return (uint8_t)((channel * 255u + 511u) / 1023u);
    const double linear = channel / 1023.0;
    const double encoded = linear <= 0.0031308 ? 12.92 * linear : 1.055 * pow(linear, 1 / 2.4) - 0.055;
    return (uint8_t)lround(fmin(1.0, fmax(0.0, encoded)) * 255.0);
}

bool vr_shot_image_savable(const TexBackend *src) {
    const bool depth = src && src->format == VK_FORMAT_D32_SFLOAT && src->aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    if (!src || !src->image || (src->format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
        src->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && src->format != VK_FORMAT_R8G8B8A8_SRGB && !depth) ||
        src->samples != 1 || src->layers != 1 || src->depth != 1 ||
        (!depth && src->aspect != VK_IMAGE_ASPECT_COLOR_BIT) || !src->width || !src->height ||
        src->width > 4096 || src->height > 4096) return false;
    return true;
}

// Saves src as a PNG at path: a single-sample 2D image of at most 4096x4096 in A2B10G10R10, B10G11R11 (HDR, through
// labo_premask_rgb8's preview mapping), sRGB RGBA8 or D32 depth (as grey, with a pixel census in the log). Copies the
// image first, so later reuse of its memory cannot change the pixels, then waits for the GPU; a GPU that does not
// finish within 10 seconds stops the process. srgb: encode A2B10G10R10 values to sRGB (vr_shot_encode); alpha: save an
// RGBA8 image's alpha as grey (the PNGs here are written opaque). No render pass may be open. Returns whether the file
// was written.
static bool vr_save_png(TexBackend *src, const char *path, bool srgb, bool alpha) {
    const bool depth = src && src->format == VK_FORMAT_D32_SFLOAT && src->aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    if (!vr_shot_image_savable(src)) return false;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
        .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo image_memory = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &image_info, &image_memory, &image, &allocation, NULL) != VK_SUCCESS) return false;
    const uint64_t pixels = (uint64_t)src->width * src->height;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation buffer_allocation = VK_NULL_HANDLE;
    VmaAllocationInfo buffer_info;
    VkBufferCreateInfo buffer_create = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = pixels * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo buffer_memory = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    if (vmaCreateBuffer(g_vk.vma, &buffer_create, &buffer_memory, &buffer, &buffer_allocation, &buffer_info) !=
        VK_SUCCESS) {
        vmaDestroyImage(g_vk.vma, image, allocation);
        return false;
    }
    VkCommandBuffer commands = vk_cmd();
    vk_image_init_layout(commands, image, src->aspect);
    vk_full_barrier(commands);
    VkImageCopy copy = {.srcSubresource = {src->aspect, 0, 0, 1},
        .dstSubresource = {src->aspect, 0, 0, 1},
        .extent = {src->width, src->height, 1}};
    rec_CmdCopyImage(commands, src->image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    vk_full_barrier(commands);
    VkBufferImageCopy read = {
        .imageSubresource = {src->aspect, 0, 0, 1},
        .imageExtent = {src->width, src->height, 1}};
    rec_CmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &read);
    VkMemoryBarrier2 host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &host};
    rec_CmdPipelineBarrier2(commands, &dependency);
    const uint64_t ready = vk_pending_seq();
    renderer_submit();
    rec_wait_submitted(ready);  // (the recorder hands it to the driver first)
    VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1, .pSemaphores = &g_vk.timeline, .pValues = &ready};
    const VkResult result = vkWaitSemaphores(g_vk.dev, &wait, 10000000000ull);
    if (result != VK_SUCCESS) {
        // Never inspect staging, recycle it, or destroy pending resources. This
        // bounded diagnostic stops the process on GPU timeout/device loss.
        FATAL("[labo-candidate] GPU readback wait failed: %d\n", result);
    }
    uint8_t *rgba = malloc((size_t)pixels * 4);
    if (rgba) {
        VK_CHECK(vmaInvalidateAllocation(g_vk.vma, buffer_allocation, 0, VK_WHOLE_SIZE));
        const uint32_t *packed = buffer_info.pMappedData;
        uint64_t zero = 0, one = 0, invalid = 0;
        for (uint64_t i = 0; i < pixels; ++i) {
            if (depth) {
                float value;
                memcpy(&value, packed + i, sizeof value);
                zero += value == 0.0f; one += value == 1.0f;
                invalid += !isfinite(value) || value < 0 || value > 1;
                labo_mask_depth_rgb8(value, rgba + i*4);
            } else if (src->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
                labo_premask_rgb8(packed[i], rgba + i*4);
            } else if (src->format == VK_FORMAT_R8G8B8A8_SRGB) {  // (the eye-image producer's own images: their bytes)
                memcpy(rgba + i*4, packed + i, 4);
                if (alpha) rgba[i*4] = rgba[i*4+1] = rgba[i*4+2] = rgba[i*4+3];
            } else {
                rgba[i*4] = vr_shot_encode(packed[i] & 1023u, srgb);
                rgba[i*4+1] = vr_shot_encode((packed[i] >> 10) & 1023u, srgb);
                rgba[i*4+2] = vr_shot_encode((packed[i] >> 20) & 1023u, srgb);
                rgba[i*4+3] = (uint8_t)((packed[i] >> 30) * 85u);
            }
        }
        // Raw Vulkan row orientation: no crop, UV transform or distortion.
        vk_save_png(path, rgba, src->width, src->height);
        if (depth) LOG("[labo-depth-pixels] pixels=%llu zero=%llu one=%llu invalid=%llu path=%s\n",
            (unsigned long long)pixels, (unsigned long long)zero,
            (unsigned long long)one, (unsigned long long)invalid, path);
        free(rgba);
    }
    const bool saved = rgba != NULL;
    vmaDestroyBuffer(g_vk.vma, buffer, buffer_allocation);
    vmaDestroyImage(g_vk.vma, image, allocation);
    return saved;
}

// ------------------------------------------------------------------------------------- SAVAGEWIND_PC_VR_SHOT shots
// SAVAGEWIND_PC_VR_SHOT=<present>[,<present>...] (at most VR_SHOT_REQUESTS; the configuration's shot_presents): at the
// first frame at or after each of those presents, every kind of image is saved once as
// build/vr-shot-<kind>-<present>.png. vr_shots.kinds_saved holds, per requested present, a bit for each kind already
// saved.

// The bit for a kind of shot.
static unsigned vr_shot_bit(const char *name) {
    return !strcmp(name, "submit0") ? 4096 : !strcmp(name, "submit1") ? 8192 :
        !strcmp(name, "hdr-eye0") ? 32 : !strcmp(name, "hdr-eye1") ? 64 :
        !strcmp(name, "hdr-scene0") ? 128 : !strcmp(name, "hdr-scene1") ? 256 :
        !strcmp(name, "scene0") ? 512 : !strcmp(name, "scene1") ? 1024 : !strcmp(name, "hud-full") ? 2048 :
        !strcmp(name, "hud-alpha") ? 16 : name[0] == 'h' ? 8 : name[0] == 'c' ? 4 :
        name[strlen(name) - 1] == '1' ? 2 : 1;
}

bool vr_shot_due(const char *name) {
    const PcVrConfig *config = pc_vr_config();
    const unsigned bit = vr_shot_bit(name);
    const uint64_t present = renderer_present_count() + 1;
    for (unsigned request = 0; request < config->diagnostics.shot_count; ++request)
        if (present >= config->diagnostics.shot_presents[request] && !(vr_shots.kinds_saved[request] & bit))
            return true;
    return false;
}

void vr_shot(TexBackend *image, const char *name, bool alpha_as_grey) {
    const PcVrConfig *config = pc_vr_config();
    const uint64_t *requested = config->diagnostics.shot_presents;
    unsigned *done = vr_shots.kinds_saved;
    const unsigned bit = vr_shot_bit(name), requests = config->diagnostics.shot_count;
    const uint64_t present = renderer_present_count() + 1;
    for (unsigned request = 0; request < requests; ++request) {
        if (present < requested[request] || (done[request] & bit)) continue;
        done[request] |= bit;
        char path[80];
        snprintf(path, sizeof path, "build/vr-shot-%s-%llu.png", name, (unsigned long long)requested[request]);
        const bool saved = vr_save_png(image, path, true, alpha_as_grey);
        LOG("[pc-vr-eyes] shot %s present=%llu requested=%llu source=%ux%u saved=%d path=%s\n", name,
            (unsigned long long)present, (unsigned long long)requested[request], image->width, image->height, saved,
            path);
    }
}

int vr_shot_scene_side(uint64_t output_serial) {
    const uint64_t present = renderer_present_count() + 1;
    if (vr_shots.scene_side_present != present) {
        vr_shots.scene_side_present = present;
        memset(vr_shots.scene_side_outputs, 0, sizeof vr_shots.scene_side_outputs);
    }
    for (unsigned side = 0; side < 2; ++side) {
        if (vr_shots.scene_side_outputs[side] && vr_shots.scene_side_outputs[side] != output_serial) continue;
        vr_shots.scene_side_outputs[side] = output_serial;
        return (int)side;
    }
    return -1;
}

void vr_shot_slot_eyes(const VkImage slot_images[3], uint32_t half_width, uint32_t height) {
    for (unsigned eye = 0; eye < 2; ++eye) {
        TexBackend eye_image = {.image = slot_images[eye], .format = VK_FORMAT_R8G8B8A8_SRGB, .width = half_width,
            .height = height, .depth = 1, .layers = 1, .levels = 1, .samples = 1, .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
        vr_shot(&eye_image, eye ? "submit1" : "submit0", false);
    }
}

void vr_shot_slot_hud(const VkImage slot_images[3], uint32_t half_width, uint32_t height) {
    TexBackend hud = {.image = slot_images[2], .format = VK_FORMAT_R8G8B8A8_SRGB, .width = half_width,
        .height = height, .depth = 1, .layers = 1, .levels = 1, .samples = 1, .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
    vr_shot(&hud, "hud", false);
    vr_shot(&hud, "hud-alpha", true);
}

void vr_shot_hud_full(VkImage hud_image, const TexBackend *combined) {
    const int32_t blit_height = (int32_t)combined->height;
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB, .extent = {combined->width, combined->height, 1}, .mipLevels = 1,
        .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo memory_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage full;
    VmaAllocation full_memory;
    if (vmaCreateImage(g_vk.vma, &image_info, &memory_info, &full, &full_memory, NULL) == VK_SUCCESS) {
        VkCommandBuffer full_cb = vk_cmd();
        vk_image_init_layout(full_cb, full, VK_IMAGE_ASPECT_COLOR_BIT);
        vk_full_barrier(full_cb);
        VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{0, 0, 0}, {(int32_t)combined->width, blit_height, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{0, 0, 0}, {(int32_t)combined->width, blit_height, 1}}};
        rec_CmdBlitImage(full_cb, hud_image, VK_IMAGE_LAYOUT_GENERAL, full, VK_IMAGE_LAYOUT_GENERAL,
                         1, &blit, VK_FILTER_NEAREST);
        vk_full_barrier(full_cb);
        TexBackend shot = {.image = full, .format = VK_FORMAT_R8G8B8A8_SRGB, .width = combined->width,
            .height = combined->height, .depth = 1, .layers = 1, .levels = 1, .samples = 1,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
        vr_shot(&shot, "hud-full", false);
        vk_defer_image(full, full_memory);
    }
}

// ----------------------------------------------------------------------------------- quick menu shots
void vr_quick_menu_shots_present(bool over_world) {
    vr_shots.held_presents = over_world ? vr_shots.held_presents + 1 : 0;
    if (!over_world) vr_shots.shot_this_hold = false;
}

void vr_quick_menu_shot(const VkImage slot_images[3], bool over_world, bool slot_has_hud, uint32_t half_width,
                        uint32_t height, unsigned world_left_out) {
    if (!pc_vr_config()->diagnostics.quick_menu_shots || !over_world || !slot_has_hud || vr_shots.shot_this_hold ||
        vr_shots.held_presents + 1 < VR_QUICK_MENU_SHOT_DELAY ||
        vr_shots.quick_menus_taken >= VR_QUICK_MENU_SHOTS_MAX)
        return;
    if (!vr_shots.quick_menus_taken) mkdir("build", 0755);
    vr_shots.shot_this_hold = true;
    const unsigned number = ++vr_shots.quick_menus_taken;
    const uint64_t present = renderer_present_count() + 1;
    static const char *const image_names[4] = {"final0", "final1", "panel", "panel-alpha"};
    char paths[4][96];
    bool saved[4];
    for (unsigned image = 0; image < 4; ++image) {
        TexBackend shot = {.image = slot_images[image < 2 ? image : 2], .format = VK_FORMAT_R8G8B8A8_SRGB,
            .width = half_width, .height = height, .depth = 1, .layers = 1, .levels = 1, .samples = 1,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT};
        snprintf(paths[image], sizeof paths[image], "build/vr-shot-quick-menu%u-%s-%llu.png", number,
                 image_names[image], (unsigned long long)present);
        saved[image] = vr_save_png(&shot, paths[image], false, image == 3);
    }
    LOG("[pc-vr-eyes] quick menu shot %u of %u present=%llu (present %llu over the held scene) world pieces left out "
        "this frame=%u saved=%d%d%d%d: %s %s %s %s\n", number, VR_QUICK_MENU_SHOTS_MAX, (unsigned long long)present,
        (unsigned long long)vr_shots.held_presents + 1, world_left_out, saved[0],
        saved[1], saved[2], saved[3], paths[0], paths[1], paths[2], paths[3]);
}
