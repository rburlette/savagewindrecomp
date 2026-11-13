#pragma once
#include "vk.h"
#include "record.h"

// The measured 4x4 R32 array binding is queried and sampled with ALWAYS.
// Keep its dimensions/levels/layers and comparison sampler, but provide a legal
// depth backing: comparison results do not depend on its texels. Other shapes
// and comparison operators require their own evidence and retain their image.
static VkImageView vk_always_shadow_backing(VkImageView view, bool shader_shadow, const NvnSamplerDesc *sampler,
                                           const TexBackend *backend, uint64_t view_key1, uint64_t view_key2) {
    if (!shader_shadow || !sampler || !sampler->compare_mode || sampler->compare_func != 8) return view;
    const uint64_t key1 = VK_IMAGE_VIEW_TYPE_2D_ARRAY | (uint64_t)VK_FORMAT_R32_SFLOAT << 8 |
        UINT64_C(1) << 48 | (uint64_t)VK_IMAGE_ASPECT_COLOR_BIT << 56;
    if (!backend || view_key1 != key1 || (uint32_t)view_key2 != UINT32_C(1) << 16 ||
        backend->format != VK_FORMAT_R32_SFLOAT || backend->aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
        backend->width != 4 || backend->height != 4 || backend->depth != 1 || backend->layers != 1 ||
        backend->levels != 1 || backend->samples != VK_SAMPLE_COUNT_1_BIT) return view;
    static TexBackend backing;
    if (!backing.image) {
        VkFormatProperties3 f3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 f2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &f3};
        vkGetPhysicalDeviceFormatProperties2(g_vk.phys, VK_FORMAT_D32_SFLOAT, &f2);
        const VkFormatFeatureFlags2 required = VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT |
            VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
        if ((f3.optimalTilingFeatures & required) != required) return view;
        VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_D32_SFLOAT, .extent = {4, 4, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo memory_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &image_info, &memory_info, &backing.image, &backing.mem, NULL) != VK_SUCCESS)
            return view;
        backing.format = image_info.format; backing.width = 4; backing.height = 4; backing.depth = 1;
        backing.levels = 1; backing.layers = 1; backing.samples = VK_SAMPLE_COUNT_1_BIT;
        backing.aspect = VK_IMAGE_ASPECT_DEPTH_BIT; backing.base_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        VkCommandBuffer commands = vk_upload_cmd();
        vk_image_init_layout(commands, backing.image, backing.aspect);
        const VkClearDepthStencilValue clear = {1, 0};
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        rec_CmdClearDepthStencilImage(commands, backing.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        vk_full_barrier(commands);
        LOG("[vk] constant comparison backing: 4x4 array, one layer/mip, D32; original ALWAYS sampler retained\n");
    }
    VkImageView depth = vk_image_view(&backing, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_D32_SFLOAT,
        0, 1, 0, 1, (VkComponentMapping){0}, VK_IMAGE_ASPECT_DEPTH_BIT);
    return depth ? depth : view;
}

