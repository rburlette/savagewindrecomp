// Part of exec.c (D133): PC VR's HUD image (D057).
// Included where it stands by exec.c, so it compiles in exec.c's translation unit as before.

// PC VR, D057: the gameplay HUD's own image. The draws vr_is_hud_draw selects (the HUD's draws into the final
// image) are drawn into it instead of the final image, so the HUD can go to the headset on its own panel. It has the
// final image's size and holds premultiplied linear colour with coverage in alpha (vr_hud_blend).
#define VR_HUD_IMAGE_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
typedef struct {
    int state;  // 0 not allocated, 1 ready, -1 off (allocation failed)
    VkImage image;
    VmaAllocation memory;
    VkImageView view;
    uint32_t width, height;
    uint64_t drawn;  // renderer_present_count() + 1 of the frame that last drew it
} VrHudImage;
static VrHudImage vr_hud_image;
// vr_draw_to_hud_image: the draw being recorded goes to the HUD image (set per draw by draw_'s routing, cleared after
// its begin_pass), so begin_pass opens a pass into it. vr_pass_to_hud_image: the open pass draws into the HUD image
// (set per pass by begin_pass; read by draw_ for the HUD's blend and by the split of long passes).
static bool vr_draw_to_hud_image, vr_pass_to_hud_image;

// The HUD image at final_image's size, (re)allocated in commands before a pass. Per pass into the HUD image
// (begin_pass, for its first colour target). False: unavailable, draw as before.
static bool vr_hud_image_target(const TexBackend *final_image, VkCommandBuffer commands) {
    if (vr_hud_image.state < 0) return false;
    if (vr_hud_image.image && vr_hud_image.width == final_image->width &&
        vr_hud_image.height == final_image->height)
        return true;
    if (vr_hud_image.image) {  // (a window resize rescaled the final image)
        vk_defer_view(vr_hud_image.view);
        vk_defer_image(vr_hud_image.image, vr_hud_image.memory);
    }
    memset(&vr_hud_image, 0, sizeof vr_hud_image);
    vr_hud_image.state = -1;
    VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = VR_HUD_IMAGE_FORMAT,
                                    .extent = {final_image->width, final_image->height, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo memory_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage image;
    VmaAllocation memory;
    if (vmaCreateImage(g_vk.vma, &image_info, &memory_info, &image, &memory, NULL) != VK_SUCCESS) {
        LOG("[pc-vr-eyes] hud image allocation failed; the HUD stays in the eye images\n");
        return false;
    }
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VR_HUD_IMAGE_FORMAT,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    if (vkCreateImageView(g_vk.dev, &view_info, NULL, &view) != VK_SUCCESS) {
        vmaDestroyImage(g_vk.vma, image, memory);  // (never recorded)
        LOG("[pc-vr-eyes] hud image view failed; the HUD stays in the eye images\n");
        return false;
    }
    vk_image_init_layout(commands, image, VK_IMAGE_ASPECT_COLOR_BIT);
    vr_hud_image = (VrHudImage){.state = 1, .image = image, .memory = memory, .view = view,
        .width = final_image->width, .height = final_image->height};
    LOG("[pc-vr-eyes] hud image %ux%u format=R16G16B16A16_SFLOAT premultiplied\n", final_image->width, final_image->height);
    return true;
}

// The draws between submissions at a pass boundary (SAVAGEWIND_VK_SUBMIT_DRAWS; 300). Per pass (begin_pass) and per
// draw (the split of long passes at the end of draw_).
static int submit_draw_count(void) {
    static int count = -1;
    if (count < 0) count = getenv("SAVAGEWIND_VK_SUBMIT_DRAWS") ? atoi(getenv("SAVAGEWIND_VK_SUBMIT_DRAWS")) : 300;
    return count;
}
