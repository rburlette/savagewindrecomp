// PC VR renderer: the desktop window's picture (D085). What it shows and why, when it runs, what it replaces, the
// preconditions and image ownership: vr_mirror.h. Moved here from exec.c (vr_window_mirror); the bodies are exec.c's.
#include <string.h>

#include "vk.h"
#include "record.h"
#include "vr_math.h"
#include "vr_mirror.h"

uint64_t renderer_present_count(void);  // (device.c) the presents so far: the frame being recorded is this plus one

// The mirror's state, for the process (vr_mirror.h: why there is no reset).
static struct {
    int mode;  // 0 undecided, 1 the left eye, -1 the game's lens pass kept (unsupported presented format)
    // The present (count plus one) the mirror last handled, whether or not it copied (nothing to copy, or the image's
    // allocation failed): the lens pass can be more than one draw, and only the first is handled.
    uint64_t mirrored_present;
    // The image of the fitted size the half is blitted into (VK_NULL_HANDLE: none yet, or its allocation failed).
    struct {
        VkImage image;
        VmaAllocation memory;
        uint32_t width, height;
        VkFormat format;
    } fitted_image;
} vr_mirror;

// The sRGB format with the presented image's byte layout (VK_FORMAT_UNDEFINED: none the mirror can write).
static VkFormat vr_mirror_format(const TexBackend *presented) {
    return
        presented->format == VK_FORMAT_R8G8B8A8_UNORM || presented->format == VK_FORMAT_R8G8B8A8_SRGB ?
            VK_FORMAT_R8G8B8A8_SRGB :
        presented->format == VK_FORMAT_B8G8R8A8_UNORM || presented->format == VK_FORMAT_B8G8R8A8_SRGB ?
            VK_FORMAT_B8G8R8A8_SRGB :
            VK_FORMAT_UNDEFINED;
}

bool vr_mirror_takes(const TexBackend *presented, const TexBackend *final_image) {
    const VkFormat mirror_format = vr_mirror_format(presented);
    if (!vr_mirror.mode) {
        VkFormatProperties properties = {0};
        if (mirror_format != VK_FORMAT_UNDEFINED)
            vkGetPhysicalDeviceFormatProperties(g_vk.phys, mirror_format, &properties);
        vr_mirror.mode = properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT ? 1 : -1;
        LOG("[pc-vr-mirror] window: %s (presented image %ux%u format %d, final image %ux%u)\n",
            vr_mirror.mode > 0 ? "the left eye, the game's lens pass not drawn" : "the game's lens pass (presented format unsupported)",
            presented->width, presented->height, (int)presented->format, final_image->width, final_image->height);
    }
    if (vr_mirror.mode < 0 || mirror_format == VK_FORMAT_UNDEFINED) return false;
    return true;
}

bool vr_mirror_due(const TexBackend *presented, const TexBackend *final_image, VrLetterbox *box) {
    const uint64_t present = renderer_present_count() + 1;
    if (vr_mirror.mirrored_present == present) return false;
    vr_mirror.mirrored_present = present;
    // The left half, scaled to fit the presented image and centred in it.
    return vr_letterbox(presented->width, presented->height, final_image->width, final_image->height, box);
}

void vr_mirror_copy(const TexBackend *presented, const TexBackend *final_image, const VrLetterbox *box) {
    const VkFormat mirror_format = vr_mirror_format(presented);
    const uint32_t half_width = box->half_width, half_height = box->half_height;
    const int32_t fitted_w = box->fitted_width, fitted_h = box->fitted_height, left = box->left, top = box->top;
    VkCommandBuffer commands = vk_cmd();
    if (vr_mirror.fitted_image.width != (uint32_t)fitted_w || vr_mirror.fitted_image.height != (uint32_t)fitted_h ||
        vr_mirror.fitted_image.format != mirror_format) {
        if (vr_mirror.fitted_image.image) vk_defer_image(vr_mirror.fitted_image.image, vr_mirror.fitted_image.memory);
        memset(&vr_mirror.fitted_image, 0, sizeof vr_mirror.fitted_image);
        VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = mirror_format, .extent = {(uint32_t)fitted_w, (uint32_t)fitted_h, 1}, .mipLevels = 1,
            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo memory_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &image_info, &memory_info, &vr_mirror.fitted_image.image,
                           &vr_mirror.fitted_image.memory, NULL) != VK_SUCCESS) {
            memset(&vr_mirror.fitted_image, 0, sizeof vr_mirror.fitted_image);
            return;  // (the window keeps last frame's picture)
        }
        vr_mirror.fitted_image.width = (uint32_t)fitted_w;
        vr_mirror.fitted_image.height = (uint32_t)fitted_h;
        vr_mirror.fitted_image.format = mirror_format;
        vk_image_init_layout(commands, vr_mirror.fitted_image.image, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    vk_full_barrier(commands);
    const VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    rec_CmdClearColorImage(commands, presented->image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {(int32_t)half_width, (int32_t)half_height, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{0, 0, 0}, {fitted_w, fitted_h, 1}}};
    // (nearest when the fitted size is the half's own: a plain copy)
    rec_CmdBlitImage(commands, final_image->image, VK_IMAGE_LAYOUT_GENERAL, vr_mirror.fitted_image.image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                     fitted_w == (int32_t)half_width && fitted_h == (int32_t)half_height ? VK_FILTER_NEAREST
                                                                                       : VK_FILTER_LINEAR);
    vk_full_barrier(commands);
    VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffset = {left, top, 0},
        .extent = {(uint32_t)fitted_w, (uint32_t)fitted_h, 1}};
    rec_CmdCopyImage(commands, vr_mirror.fitted_image.image, VK_IMAGE_LAYOUT_GENERAL, presented->image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    vk_full_barrier(commands);
}
