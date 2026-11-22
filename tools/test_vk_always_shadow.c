// Exercise the real backing-image guard and cache with authored metadata only.
#include "../runtime/nvn/vulkan/vk.h"
#include "../runtime/nvn/vulkan/always_shadow.h"
#include <assert.h>

VkG g_vk;
static unsigned allocations, clears, barriers, image_views;
static bool supported = true, allocation_fails;
static VkImageCreateInfo created;
static VkImageViewCreateInfo viewed;
uint64_t vk_pending_seq(void) { return 1; }
VkCommandBuffer vk_upload_cmd(void) { return (VkCommandBuffer)(uintptr_t)1; }
void vk_full_barrier(VkCommandBuffer cb) { (void)cb; ++barriers; }
void rt_fatal_trace(void) { abort(); }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice device, VkFormat format, VkFormatProperties2 *p) {
    (void)device; assert(format == VK_FORMAT_D32_SFLOAT);
    VkFormatProperties3 *f = p->pNext;
    f->optimalTilingFeatures = supported ? VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT |
        VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT : 0;
}
VkResult vmaCreateImage(VmaAllocator allocator, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai,
                       VkImage *image, VmaAllocation *memory, VmaAllocationInfo *info) {
    (void)allocator; (void)ai; (void)info; ++allocations; created = *ci;
    if (allocation_fails) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *image = (VkImage)(uintptr_t)5; *memory = (VmaAllocation)(uintptr_t)6;
    return VK_SUCCESS;
}
void vk_image_init_layout(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect) {
    (void)cb; (void)image; assert(aspect == VK_IMAGE_ASPECT_DEPTH_BIT);
}
void rec_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
    const VkClearDepthStencilValue *value, uint32_t count, const VkImageSubresourceRange *range) {
    (void)cb; (void)image; assert(layout == VK_IMAGE_LAYOUT_GENERAL && value->depth == 1 && count == 1);
    assert(range->aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT && range->levelCount == 1 && range->layerCount == 1);
    ++clears;
}
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat format, uint32_t lv, uint32_t nlv,
    uint32_t layer, uint32_t nlayer, VkComponentMapping sw, VkImageAspectFlags aspect) {
    (void)b; (void)sw; viewed = (VkImageViewCreateInfo){.viewType = type, .format = format,
        .subresourceRange = {aspect, lv, nlv, layer, nlayer}};
    return (VkImageView)(uintptr_t)(100 + ++image_views);
}

int main(void) {
    TexBackend source = {.format = VK_FORMAT_R32_SFLOAT, .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .width = 4, .height = 4, .depth = 1, .layers = 1, .levels = 1, .samples = VK_SAMPLE_COUNT_1_BIT};
    const VkImageView original = (VkImageView)(uintptr_t)10;
    uint64_t view_key1 = VK_IMAGE_VIEW_TYPE_2D_ARRAY | (uint64_t)VK_FORMAT_R32_SFLOAT << 8 |
        UINT64_C(1) << 48 | (uint64_t)VK_IMAGE_ASPECT_COLOR_BIT << 56;
    uint64_t view_key2 = UINT64_C(1) << 16;
    NvnSamplerDesc sampler = {.compare_mode = 1, .compare_func = 8};
    const NvnSamplerDesc before = sampler;
    assert(vk_always_shadow_backing(original, false, &sampler, &source, view_key1, view_key2) == original);
    assert(vk_always_shadow_backing(original, true, NULL, &source, view_key1, view_key2) == original);
    sampler.compare_mode = 0; assert(vk_always_shadow_backing(original, true, &sampler, &source, view_key1, view_key2) == original);
    sampler.compare_mode = 1;
    for (unsigned op = 0; op < 8; ++op) {
        sampler.compare_func = op; assert(vk_always_shadow_backing(original, true, &sampler, &source, view_key1, view_key2) == original);
    }
    sampler = before;
    source.width = 8; assert(vk_always_shadow_backing(original, true, &sampler, &source, view_key1, view_key2) == original); source.width = 4;
    source.levels = 2; assert(vk_always_shadow_backing(original, true, &sampler, &source, view_key1, view_key2) == original); source.levels = 1;
    source.layers = 2; assert(vk_always_shadow_backing(original, true, &sampler, &source, view_key1, view_key2) == original); source.layers = 1;
    source.samples = 2; assert(vk_always_shadow_backing(original, true, &sampler, &source, view_key1, view_key2) == original); source.samples = 1;
    view_key2++; assert(vk_always_shadow_backing(original, true, &sampler, &source, view_key1, view_key2) == original); view_key2--;
    assert(vk_always_shadow_backing(original, true, &sampler, NULL, view_key1, view_key2) == original);
    assert(allocations == 0);
    supported = false; assert(vk_always_shadow_backing(original, true, &sampler, &source, view_key1, view_key2) == original); supported = true;
    assert(allocations == 0);
    allocation_fails = true; assert(vk_always_shadow_backing(original, true, &sampler, &source, view_key1, view_key2) == original); allocation_fails = false;
    assert(allocations == 1 && clears == 0);
    VkImageView depth = vk_always_shadow_backing(original, true, &sampler, &source, view_key1, view_key2);
    assert(depth != original && depth && allocations == 2 && clears == 1 && barriers == 1);
    assert(created.format == VK_FORMAT_D32_SFLOAT && created.extent.width == 4 && created.extent.height == 4 &&
        created.extent.depth == 1 && created.arrayLayers == 1 && created.mipLevels == 1);
    assert(viewed.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY && viewed.format == VK_FORMAT_D32_SFLOAT &&
        viewed.subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT && viewed.subresourceRange.levelCount == 1 &&
        viewed.subresourceRange.layerCount == 1);
    assert(!memcmp(&before, &sampler, sizeof sampler));
    assert(vk_always_shadow_backing(original, true, &sampler, &source, view_key1, view_key2) && allocations == 2 && clears == 1);
    puts("constant comparison backing: guard, shape, sampler preservation and backing reuse checks passed");
    return 0;
}
