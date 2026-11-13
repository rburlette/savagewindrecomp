// Asset-free graphics boundary fixture. Fake images/queues only: no driver,
// loader, runtime, headset or game files. Reuse the existing owner fixture.
#define main tracking_fixture_main
#include "test_xr_runtime.c"
#undef main

static struct {
    unsigned creates, destroys, acquires, waits, releases, copies, barriers;
    unsigned submits, fences, fence_asks, retirements, producer_waits, pools, pool_destroys, fence_destroys;
    bool pending_copy, producer_done, unsupported, reset_at_fence, reset_at_release;
    unsigned fail_create, fail_acquire, fail_wait, fail_release, fail_destroy;
    XrResult destroy_error;
    bool wait_always_timeout, wait_hard_error, stop_at_timeout, reset_at_timeout;
    XrSwapchain retry_swapchain;
    VkResult producer_result, submit_result, fence_result;
    uint32_t expected_width;
    VkFormat expected_format;
    bool offer_srgb;
    XrEyeSnapshot expected_eyes[2];
    const XrEyePanel *expected_panel; // D056: frames must carry its quad instead of the projection
    const XrEyePanel *expected_hud;   // D057: frames carry the projection and this quad over it
    bool expected_over;               // quick menus: the panel alone shows the HUD's swapchain, blended by its alpha
    unsigned pair_copies;            // copies recorded for the current pair: left, right, then the HUD's
    // D081 queue: pairs signal timeline 30 at 7 and up; with finished_upto set, those above it have not finished.
    // The pair copied reads images source_base + eye (default 20) and waits for expected_value (default 7).
    uint64_t finished_upto, expected_value;
    uintptr_t source_base;
} gpu_mock;

static VKAPI_ATTR VkResult VKAPI_CALL image_wait_sem(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    CHECK(device == eye_gpu.device && (timeout == EYE_GPU_WAIT_NS || timeout == 0));
    CHECK(info->semaphoreCount == 1 && info->pSemaphores[0] == (VkSemaphore)(uintptr_t)30 && info->pValues[0] >= 7);
    ++gpu_mock.producer_waits;
    const VkResult result = gpu_mock.finished_upto && info->pValues[0] > gpu_mock.finished_upto ? VK_TIMEOUT : gpu_mock.producer_result;
    gpu_mock.producer_done = result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST;
    return result;
}
static VKAPI_ATTR VkResult VKAPI_CALL image_create_pool(VkDevice device, const VkCommandPoolCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkCommandPool *pool) {
    (void)allocator;
    CHECK(device == eye_gpu.device && info->queueFamilyIndex == 4);
    ++gpu_mock.pools; *pool = (VkCommandPool)(uintptr_t)40; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL image_destroy_pool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *allocator) {
    (void)device; (void)pool; (void)allocator;
    CHECK(!gpu_mock.pending_copy); ++gpu_mock.pool_destroys;
}
static VKAPI_ATTR VkResult VKAPI_CALL image_allocate(VkDevice device, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *command) {
    (void)device;
    CHECK(info->commandBufferCount == 1 && info->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    *command = (VkCommandBuffer)(uintptr_t)41; return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL image_create_fence(VkDevice device, const VkFenceCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkFence *fence) {
    (void)device; (void)allocator;
    CHECK(info->flags == 0); *fence = (VkFence)(uintptr_t)42; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL image_destroy_fence(VkDevice device, VkFence fence, const VkAllocationCallbacks *allocator) {
    (void)device; (void)fence; (void)allocator;
    CHECK(!gpu_mock.pending_copy); ++gpu_mock.fence_destroys;
}
static VKAPI_ATTR VkResult VKAPI_CALL image_wait_fence(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout) {
    (void)device;
    // (the owner's one bounded wait for a copy, or 0: asking about a copy already late)
    CHECK(count == 1 && *fences == (VkFence)(uintptr_t)42 && all && (timeout == EYE_GPU_WAIT_NS || timeout == 0));
    CHECK(gpu_mock.pending_copy);
    ++gpu_mock.fences;
    gpu_mock.fence_asks += !timeout;
    if (gpu_mock.reset_at_fence) xr_runtime_reset_game();
    if (gpu_mock.fence_result == VK_SUCCESS || gpu_mock.fence_result == VK_ERROR_DEVICE_LOST) gpu_mock.pending_copy = false;
    return gpu_mock.fence_result;
}
static VKAPI_ATTR VkResult VKAPI_CALL image_reset_fence(VkDevice device, uint32_t count, const VkFence *fences) {
    (void)device; (void)fences;
    CHECK(count == 1 && !gpu_mock.pending_copy); return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL image_reset_pool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    (void)device; (void)pool; (void)flags;
    CHECK(!gpu_mock.pending_copy); return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL image_begin(VkCommandBuffer command, const VkCommandBufferBeginInfo *info) {
    CHECK(command == eye_gpu.command && info->flags == VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    gpu_mock.pair_copies = 0;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL image_end(VkCommandBuffer command) {
    CHECK(command == eye_gpu.command); return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL image_barrier(VkCommandBuffer command, VkPipelineStageFlags src, VkPipelineStageFlags dst,
        VkDependencyFlags dep, uint32_t memory_count, const VkMemoryBarrier *memory, uint32_t buffer_count,
        const VkBufferMemoryBarrier *buffer, uint32_t image_count, const VkImageMemoryBarrier *barrier) {
    (void)command; (void)dep; (void)memory; (void)buffer;
    CHECK(!memory_count && !buffer_count && image_count == 1);
    CHECK(barrier->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && barrier->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    CHECK(barrier->subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && barrier->subresourceRange.levelCount == 1 &&
          barrier->subresourceRange.layerCount == 1);
    CHECK(barrier->image == (VkImage)(uintptr_t)100 || barrier->image == (VkImage)(uintptr_t)101 ||
          (eye_gpu.eyes[2].handle && barrier->image == (VkImage)(uintptr_t)102));
    if (!(gpu_mock.barriers % 2)) {
        CHECK(src == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT && dst == VK_PIPELINE_STAGE_TRANSFER_BIT);
        CHECK(barrier->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && barrier->newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        CHECK(barrier->dstAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
    } else {
        CHECK(src == VK_PIPELINE_STAGE_TRANSFER_BIT && dst == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        CHECK(barrier->oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && barrier->newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        CHECK(barrier->srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    ++gpu_mock.barriers;
}
static VKAPI_ATTR void VKAPI_CALL image_copy(VkCommandBuffer command, VkImage source, VkImageLayout source_layout,
        VkImage destination, VkImageLayout destination_layout, uint32_t count, const VkImageCopy *regions) {
    (void)command;
    // In order: left, right, then 2: the HUD image, 22, into its swapchain's 102 (an over-scene pair's only copy).
    const unsigned eye = (unsigned)((uintptr_t)destination - 100);
    CHECK(eye < 3 && eye >= gpu_mock.pair_copies && (eye < 2 || eye_gpu.eyes[2].handle));
    gpu_mock.pair_copies = eye + 1;
    CHECK(source == (VkImage)((gpu_mock.source_base ? gpu_mock.source_base : 20) + eye));
    CHECK(source_layout == VK_IMAGE_LAYOUT_GENERAL && destination_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CHECK(count == 1 && regions->extent.width == gpu_mock.expected_width && regions->extent.height == 900 && regions->extent.depth == 1);
    CHECK(regions->srcSubresource.mipLevel == 0 && regions->srcSubresource.baseArrayLayer == 0 && regions->srcSubresource.layerCount == 1);
    ++gpu_mock.copies;
}
static VKAPI_ATTR VkResult VKAPI_CALL image_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
    CHECK(queue == xr.queue && count == 1 && fence == eye_gpu.fence && !gpu_mock.pending_copy);
    CHECK(gpu_mock.producer_done && info->commandBufferCount == 1 && info->pCommandBuffers[0] == eye_gpu.command);
    CHECK(info->waitSemaphoreCount == 1 && info->pWaitSemaphores[0] == (VkSemaphore)(uintptr_t)30);
    CHECK(info->pWaitDstStageMask[0] == VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkTimelineSemaphoreSubmitInfo *timeline = info->pNext;
    CHECK(timeline && timeline->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO &&
          timeline->waitSemaphoreValueCount == 1 &&
          timeline->pWaitSemaphoreValues[0] == (gpu_mock.expected_value ? gpu_mock.expected_value : 7));
    ++gpu_mock.submits;
    if (gpu_mock.submit_result == VK_SUCCESS) gpu_mock.pending_copy = true;
    return gpu_mock.submit_result;
}

static PFN_vkVoidFunction image_proc(VkDevice device, const char *name) {
    CHECK(device == (VkDevice)(uintptr_t)10);
#define PROC(name_, fn_) if (!strcmp(name, "vk" #name_)) return (PFN_vkVoidFunction)fn_
    PROC(WaitSemaphores, image_wait_sem); PROC(CreateCommandPool, image_create_pool);
    PROC(DestroyCommandPool, image_destroy_pool); PROC(AllocateCommandBuffers, image_allocate);
    PROC(CreateFence, image_create_fence); PROC(DestroyFence, image_destroy_fence);
    PROC(WaitForFences, image_wait_fence); PROC(ResetFences, image_reset_fence); PROC(ResetCommandPool, image_reset_pool);
    PROC(BeginCommandBuffer, image_begin); PROC(EndCommandBuffer, image_end);
    PROC(CmdPipelineBarrier, image_barrier); PROC(CmdCopyImage, image_copy); PROC(QueueSubmit, image_submit);
#undef PROC
    return NULL;
}

static XrResult XRAPI_CALL image_formats(XrSession session, uint32_t capacity, uint32_t *count, int64_t *formats) {
    CHECK(session == xr.session);
    *count = gpu_mock.offer_srgb ? 2 : 1;
    if (capacity) {
        CHECK(capacity == *count);
        formats[0] = gpu_mock.unsupported ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        if (gpu_mock.offer_srgb) formats[1] = VK_FORMAT_R8G8B8A8_SRGB;
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL image_create(XrSession session, const XrSwapchainCreateInfo *info, XrSwapchain *swapchain) {
    CHECK(session == xr.session && info->width == gpu_mock.expected_width && info->height == 900);
    CHECK(info->format == gpu_mock.expected_format && info->sampleCount == 1 && info->arraySize == 1 &&
          info->faceCount == 1 && info->mipCount == 1);
    CHECK(info->usageFlags == (XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT));
    ++gpu_mock.creates;
    if (gpu_mock.creates == gpu_mock.fail_create) return XR_ERROR_RUNTIME_FAILURE;
    // 50 and 51 for the eyes, 52 for the HUD's: the first slot without a handle yet.
    *swapchain = (XrSwapchain)(uintptr_t)(!eye_gpu.eyes[0].handle ? 50 : !eye_gpu.eyes[1].handle ? 51 : 52);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL image_destroy(XrSwapchain swapchain) {
    CHECK(swapchain && !gpu_mock.pending_copy && atomic_load(&mock.destroy_sessions) == 0);
    ++gpu_mock.destroys;
    return gpu_mock.destroys == gpu_mock.fail_destroy ?
        (gpu_mock.destroy_error ? gpu_mock.destroy_error : XR_ERROR_RUNTIME_FAILURE) : XR_SUCCESS;
}
static XrResult XRAPI_CALL image_enumerate(XrSwapchain swapchain, uint32_t capacity, uint32_t *count, XrSwapchainImageBaseHeader *images) {
    *count = 1;
    if (capacity) {
        CHECK(capacity == 1 && images[0].type == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR);
        ((XrSwapchainImageVulkanKHR *)images)[0].image = (VkImage)(uintptr_t)(100 + ((uintptr_t)swapchain - 50));
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL image_acquire(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *info, uint32_t *index) {
    (void)swapchain;
    CHECK(info->type == XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
    ++gpu_mock.acquires; *index = 0;
    return gpu_mock.acquires == gpu_mock.fail_acquire ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
}
static XrResult XRAPI_CALL image_wait(XrSwapchain swapchain, const XrSwapchainImageWaitInfo *info) {
    CHECK(info->timeout == (XrDuration)EYE_GPU_WAIT_NS);
    ++gpu_mock.waits;
    if (gpu_mock.retry_swapchain) {
        CHECK(swapchain == gpu_mock.retry_swapchain); // no new acquire between waits
        gpu_mock.retry_swapchain = XR_NULL_HANDLE;
    }
    if (gpu_mock.wait_hard_error) return XR_ERROR_RUNTIME_FAILURE;
    if (gpu_mock.wait_always_timeout || gpu_mock.waits == gpu_mock.fail_wait) {
        CHECK(!gpu_mock.copies && !gpu_mock.releases);
        gpu_mock.retry_swapchain = swapchain;
        if (gpu_mock.stop_at_timeout) atomic_store(&stop_requested, true);
        if (gpu_mock.reset_at_timeout) xr_runtime_reset_game();
        return XR_TIMEOUT_EXPIRED;
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL image_release(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *info) {
    (void)swapchain;
    CHECK(info->type == XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO && !gpu_mock.pending_copy);
    CHECK(gpu_mock.fences > 0);
    ++gpu_mock.releases;
    if (gpu_mock.reset_at_release && !(gpu_mock.releases % 2)) xr_runtime_reset_game();
    return gpu_mock.releases == gpu_mock.fail_release ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
}
static void image_retire(void *context) {
    CHECK(context == &gpu_mock && gpu_mock.producer_done && !gpu_mock.pending_copy);
    ++gpu_mock.retirements;
}
static void image_inspect_end(const XrFrameEndInfo *info) {
    if (!info->layerCount) return;
    CHECK(pthread_mutex_trylock(&eye_submission_lock) != 0);
    CHECK(info->layerCount == (gpu_mock.expected_hud ? 2u : 1u) && gpu_mock.releases >= 2 && gpu_mock.retirements >= 1);
    if (gpu_mock.expected_panel || gpu_mock.expected_hud) {
        // A menu panel alone, or the HUD's quad over the projection (checked below), blended by its alpha.
        const bool hud = gpu_mock.expected_hud != NULL, alpha = hud || gpu_mock.expected_over;
        const XrEyePanel *panel = hud ? gpu_mock.expected_hud : gpu_mock.expected_panel;
        const XrCompositionLayerQuad *quad = (const XrCompositionLayerQuad *)info->layers[hud ? 1 : 0];
        CHECK(quad->type == XR_TYPE_COMPOSITION_LAYER_QUAD && quad->space == xr.local &&
              quad->layerFlags == (alpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0));
        // One image for both eyes.
        CHECK(quad->eyeVisibility == XR_EYE_VISIBILITY_BOTH);
        CHECK(quad->subImage.swapchain == eye_gpu.eyes[alpha ? 2 : panel->eye].handle && quad->subImage.imageArrayIndex == 0);
        CHECK(quad->subImage.swapchain != XR_NULL_HANDLE);
        CHECK(quad->subImage.imageRect.offset.x == panel->rect[0] && quad->subImage.imageRect.offset.y == panel->rect[1]);
        CHECK(quad->subImage.imageRect.extent.width == panel->rect[2] && quad->subImage.imageRect.extent.height == panel->rect[3]);
        // World-locked: the panel's pose, not any eye pose.
        CHECK(quad->pose.position.x == panel->position_xyz[0] && quad->pose.position.z == panel->position_xyz[2]);
        CHECK(quad->pose.orientation.y == panel->orientation_xyzw[1] && quad->pose.orientation.w == panel->orientation_xyzw[3]);
        CHECK(quad->size.width == panel->width_m && quad->size.height == panel->height_m);
        if (!hud) return;
    }
    const XrCompositionLayerProjection *layer = (const XrCompositionLayerProjection *)info->layers[0];
    CHECK(layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION && layer->space == xr.local && layer->viewCount == 2);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(layer->views[i].subImage.swapchain == eye_gpu.eyes[i].handle);
        CHECK(layer->views[i].subImage.imageRect.extent.width == (int32_t)gpu_mock.expected_width);
        CHECK(layer->views[i].subImage.imageRect.offset.x == 0 && layer->views[i].subImage.imageArrayIndex == 0);
        // Current locate is deliberately shifted. Layers must describe the
        // native views that rendered these pixels, not the newer locate call.
        CHECK(layer->views[i].pose.position.x == gpu_mock.expected_eyes[i].position_xyz[0]);
        CHECK(layer->views[i].pose.orientation.w == gpu_mock.expected_eyes[i].orientation_xyzw[3]);
        CHECK(layer->views[i].fov.angleLeft == gpu_mock.expected_eyes[i].fov_left_right_up_down[0]);
        CHECK(layer->views[i].pose.position.y == gpu_mock.expected_eyes[i].position_xyz[1] &&
              layer->views[i].pose.position.z == gpu_mock.expected_eyes[i].position_xyz[2]);
        CHECK(layer->views[i].pose.orientation.x == gpu_mock.expected_eyes[i].orientation_xyzw[0] &&
              layer->views[i].pose.orientation.y == gpu_mock.expected_eyes[i].orientation_xyzw[1] &&
              layer->views[i].pose.orientation.z == gpu_mock.expected_eyes[i].orientation_xyzw[2]);
        CHECK(layer->views[i].fov.angleRight == gpu_mock.expected_eyes[i].fov_left_right_up_down[1] &&
              layer->views[i].fov.angleUp == gpu_mock.expected_eyes[i].fov_left_right_up_down[2] &&
              layer->views[i].fov.angleDown == gpu_mock.expected_eyes[i].fov_left_right_up_down[3]);
    }
}

static XrOwnedEyePair image_prepare(void) {
    prepare();
    memset(&gpu_mock, 0, sizeof gpu_mock);
    gpu_mock.expected_width = 800;
    gpu_mock.expected_format = VK_FORMAT_R8G8B8A8_UNORM;
    test_device_proc = image_proc;
    CHECK(eye_gpu_bind((VkDevice)(uintptr_t)10, 4));
    xr.EnumerateSwapchainFormats = image_formats; xr.CreateSwapchain = image_create; xr.DestroySwapchain = image_destroy;
    xr.EnumerateSwapchainImages = image_enumerate; xr.AcquireSwapchainImage = image_acquire;
    xr.WaitSwapchainImage = image_wait; xr.ReleaseSwapchainImage = image_release;
    mock.view_flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
        XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    // SDL's clock starts at its first read; a zero publication time is invalid.
    (void)xr_runtime_pose_clock_ns();
    SDL_Delay(1);
    tracking_frame(1);
    XrOwnedEyePair pair = {.device = (VkDevice)(uintptr_t)10, .queue_family = 4,
        .images = {(VkImage)(uintptr_t)20, (VkImage)(uintptr_t)21}, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .width = 800, .height = 900, .ready_timeline = (VkSemaphore)(uintptr_t)30, .ready_value = 7,
        .game_present_id = 2, .image_generation = 1, .projection = XR_EYE_PROJECTION_NATIVE_RENDERED,
        .release = image_retire, .context = &gpu_mock};
    CHECK(xr_runtime_get_pose(&pair.render_pose) == XR_POSE_VALID && pair.render_pose.native_views_valid);
    memcpy(pair.rendered_eyes, pair.render_pose.eyes, sizeof pair.rendered_eyes);
    memcpy(gpu_mock.expected_eyes, pair.rendered_eyes, sizeof pair.rendered_eyes);
    mock.eye_shift = 1;
    ++mock.predicted;
    test_end_inspect = image_inspect_end;
    return pair;
}

static void image_close(void) {
    // Avoid the worker's exit request in this direct-owner fixture.
    xr.running = false;
    xr_runtime_shutdown();
    CHECK(!eye_gpu.device && !eye_gpu.current_valid && !eye_gpu.pending_count);
    CHECK(gpu_mock.pool_destroys == gpu_mock.pools && !gpu_mock.pending_copy);
}

static void test_runtime_queue_completion_images(void) {
    XrOwnedEyePair pair = image_prepare();
    xr.runtime_queue_completion = true;
    CHECK(xr_runtime_offer_eye_pair(&pair));
    mock.expected_layers = 1;
    tracking_frame(2);
    CHECK(eye_gpu.submitted == 1 && atomic_load(&mock.queue_idles) == 1 && xr.queue_completion_waits == 1 &&
        !xr.queue_completion_zero_layers);
    const unsigned copies = gpu_mock.copies;
    ++mock.predicted;
    tracking_frame(0); // Reproject the retained image, without offering/copying another pair.
    CHECK(eye_gpu.submitted == 2 && gpu_mock.copies == copies && xr.queue_completion_waits == 2 &&
        atomic_load(&mock.queue_idles) == 2 && !xr.queue_completion_zero_layers);
    mock.no_render = true; mock.expected_layers = 0;
    tracking_frame(3);
    CHECK(xr.queue_completion_waits == 3 && xr.queue_completion_zero_layers == 1 &&
        atomic_load(&mock.queue_idles) == 3 && !xr.terminal);
    image_close();
}

static void test_display_only_owned_pose(void) {
    XrOwnedEyePair pair = image_prepare();
    xr.display_rate_tracking = true;
    mock.rich_views = true;
    tracking_frame(0);
    CHECK(xr_runtime_get_pose(&pair.render_pose) == XR_POSE_VALID && !pair.render_pose.game_present_id);
    memcpy(pair.rendered_eyes, pair.render_pose.eyes, sizeof pair.rendered_eyes);
    memcpy(gpu_mock.expected_eyes, pair.rendered_eyes, sizeof pair.rendered_eyes);
    const XrPoseSnapshot retained = pair.render_pose;
    mock.eye_shift = 2;
    mock.rich_views = false; // Every pose/FOV member now differs from the retained render sample.
    xr.runtime_queue_completion = true;
    CHECK(pair.game_present_id && xr_runtime_offer_eye_pair(&pair));
    mock.expected_layers = 1;
    for (unsigned i = 0; i < 4; ++i) {
        ++mock.predicted;
        tracking_frame(0);
        CHECK(eye_gpu.layer_published_ns == retained.published_monotonic_ns &&
              eye_gpu.layer_predicted == retained.predicted_display_time_ns);
        CHECK(!published.game_present_id && published.frame_id > retained.frame_id);
    }
    CHECK(eye_gpu.submitted == 4 && eye_gpu.copied == 1 && gpu_mock.copies == 2 &&
          gpu_mock.retirements == 1 && xr.queue_completion_waits == 4 && !xr.queue_completion_zero_layers);
    image_close();
}

static void test_image_admission(void) {
    XrOwnedEyePair pair = image_prepare(), bad;
    CHECK(!xr_runtime_offer_eye_pair(NULL));
#define REJECT(edit) do { bad = pair; edit; CHECK(!xr_runtime_offer_eye_pair(&bad)); } while (0)
    REJECT(bad.projection = XR_EYE_PROJECTION_UNPROVEN);
    REJECT(bad.images[1] = VK_NULL_HANDLE); REJECT(bad.images[1] = bad.images[0]);
    REJECT(bad.device = (VkDevice)(uintptr_t)11); REJECT(++bad.queue_family);
    REJECT(bad.ready_timeline = VK_NULL_HANDLE); REJECT(bad.ready_value = 0);
    REJECT(bad.width = 0); REJECT(bad.height = UINT32_MAX); REJECT(bad.format = VK_FORMAT_D32_SFLOAT);
    REJECT(bad.game_present_id = 0); REJECT(bad.image_generation = 0); REJECT(bad.release = NULL);
    REJECT(bad.render_pose.frame_id = 0); REJECT(++bad.render_pose.generation);
    REJECT(bad.render_pose.published_monotonic_ns = 0);
    REJECT(bad.render_pose.published_monotonic_ns = xr_runtime_pose_clock_ns() + 1000000000);
    REJECT(bad.render_pose.native_views_valid = false); REJECT(bad.render_pose.active = false);
    REJECT(bad.projection = XR_EYE_PROJECTION_DEV_UNPROVEN); // strict unless the D048 flag is set
    REJECT(bad.display_encoded = true; bad.format = VK_FORMAT_R8G8B8A8_SRGB);
    REJECT(memcpy(bad.render_pose.eyes, bad.rendered_eyes + 1, sizeof bad.rendered_eyes[0]);
           bad.rendered_eyes[0] = bad.rendered_eyes[1]); // identical eyes cannot show order
    REJECT(bad.rendered_eyes[0].position_xyz[0] = 0);
    REJECT(bad.rendered_eyes[1].fov_left_right_up_down[0] = -0.5f);
    REJECT(bad.rendered_eyes[0].orientation_xyzw[0] = NAN);
    REJECT(bad.render_pose.eyes[1] = bad.render_pose.eyes[0]);
#undef REJECT
    CHECK(gpu_mock.retirements == 0 && gpu_mock.creates == 0 && gpu_mock.submits == 0);
    CHECK(xr_runtime_offer_eye_pair(&pair));
    CHECK(!xr_runtime_offer_eye_pair(&pair)); // mailbox never overwrites an owned pair
    CHECK(gpu_mock.retirements == 0);
    xr_runtime_reset_game();
    CHECK(gpu_mock.retirements == 0); // reset does not prematurely free producer work
    eye_retire_obsolete();
    CHECK(gpu_mock.retirements == 1 && gpu_mock.producer_waits == 1 && gpu_mock.submits == 0);
    image_close();
    // A newer publication does not invalidate the pose the eyes rendered with,
    // and the D048 flag admits DEV_UNPROVEN pairs; nothing else changes.
    pair = image_prepare();
    tracking_frame(2);
    pair.projection = XR_EYE_PROJECTION_DEV_UNPROVEN;
    CHECK(!xr_runtime_offer_eye_pair(&pair));
    eye_images_enabled = true;
    CHECK(xr_runtime_eye_images_enabled() && xr_runtime_offer_eye_pair(&pair));
    image_close();
    // Anchored 2D content (menus): an old latched sample is placement, not a render
    // pose. Only DEV_UNPROVEN pairs may be anchored; freshness restarts at the offer.
    pair = image_prepare();
    eye_images_enabled = true;
    SDL_Delay(260);
    pair.projection = XR_EYE_PROJECTION_DEV_UNPROVEN;
    CHECK(!xr_runtime_offer_eye_pair(&pair));
    pair.anchored = true;
    pair.projection = XR_EYE_PROJECTION_NATIVE_RENDERED;
    CHECK(!xr_runtime_offer_eye_pair(&pair));
    pair.projection = XR_EYE_PROJECTION_DEV_UNPROVEN;
    CHECK(xr_runtime_offer_eye_pair(&pair));
    CHECK(eye_gpu.pending[0].render_pose.published_monotonic_ns + 1000000 > xr_runtime_pose_clock_ns());
    mock.expected_layers = 1;
    tracking_frame(0);
    CHECK(eye_gpu.submitted == 1 && eye_gpu.layer_projection == XR_EYE_PROJECTION_DEV_UNPROVEN);
    eye_images_enabled = false;
    image_close();
}

static void test_image_copy_and_resize(void) {
    XrOwnedEyePair pair = image_prepare();
    CHECK(xr_runtime_offer_eye_pair(&pair));
    mock.expected_layers = 1;
    tracking_frame(2);
    CHECK(gpu_mock.creates == 2 && gpu_mock.copies == 2 && gpu_mock.barriers == 4);
    CHECK(gpu_mock.acquires == 2 && gpu_mock.waits == 2 && gpu_mock.releases == 2);
    CHECK(gpu_mock.submits == 1 && gpu_mock.fences == 1 && gpu_mock.retirements == 1 && eye_gpu.submitted == 1);
    CHECK(!eye_gpu.pending_count && !eye_gpu.current_valid && !eye_gpu.copy_pending);
    // Same dimensions reuse targets and fence safely; resized owned images
    // destroy only completed prior swapchains and adopt a new generation.
    for (unsigned n = 0; n < 2; ++n) {
        CHECK(xr_runtime_get_pose(&pair.render_pose) == XR_POSE_VALID);
        memcpy(pair.rendered_eyes, pair.render_pose.eyes, sizeof pair.rendered_eyes);
        memcpy(gpu_mock.expected_eyes, pair.rendered_eyes, sizeof pair.rendered_eyes);
        pair.game_present_id = 3 + n;
        if (n) { pair.width = gpu_mock.expected_width = 1024; ++pair.image_generation; }
        CHECK(xr_runtime_offer_eye_pair(&pair));
        ++mock.predicted;
        tracking_frame(pair.game_present_id);
    }
    CHECK(gpu_mock.creates == 4 && gpu_mock.destroys == 2 && gpu_mock.submits == 3 && gpu_mock.retirements == 3);
    CHECK(xr_runtime_get_pose(&pair.render_pose) == XR_POSE_VALID);
    memcpy(pair.rendered_eyes, pair.render_pose.eyes, sizeof pair.rendered_eyes);
    --pair.image_generation;
    CHECK(!xr_runtime_offer_eye_pair(&pair)); // old allocation generation after resize
    image_close();
    CHECK(gpu_mock.destroys == 4 && gpu_mock.fence_destroys == 1);
}

static void test_image_drops(void) {
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        XrOwnedEyePair pair = image_prepare();
        CHECK(xr_runtime_offer_eye_pair(&pair));
        if (scenario == 0) SDL_Delay(260); // render pose too old by the first display frame
        if (scenario == 1) atomic_fetch_add(&reference_generation, 1);
        if (scenario == 2) --mock.predicted, --mock.predicted; // render pose lies in future
        if (scenario == 3) mock.view_flags &= ~XR_VIEW_STATE_POSITION_TRACKED_BIT;
        if (scenario == 4) gpu_mock.unsupported = true;
        if (scenario == 5) gpu_mock.reset_at_fence = true;
        if (scenario == 6) gpu_mock.reset_at_release = true;
        tracking_frame(pair.game_present_id);
        CHECK(gpu_mock.retirements == 1 && eye_gpu.submitted == 0);
        CHECK(gpu_mock.submits == (scenario >= 5 ? 1u : 0u));
        image_close();
    }
    XrOwnedEyePair pair = image_prepare();
    CHECK(xr_runtime_offer_eye_pair(&pair));
    image_close(); // pending producer work is retired before device/session destruction
    CHECK(gpu_mock.retirements == 1 && gpu_mock.producer_waits == 1 && gpu_mock.creates == 0);
    pair = image_prepare();
    CHECK(xr_runtime_offer_eye_pair(&pair));
    SDL_Delay(260);
    eye_retire_obsolete();
    CHECK(gpu_mock.retirements == 1 && gpu_mock.creates == 0);
    image_close();
}

static void test_image_failures(void) {
    for (unsigned scenario = 0; scenario < 11; ++scenario) {
        XrOwnedEyePair pair = image_prepare();
        CHECK(xr_runtime_offer_eye_pair(&pair));
        switch (scenario) {
            case 0: gpu_mock.fail_create = 2; break;
            case 1: gpu_mock.fail_acquire = 2; break;
            case 2: gpu_mock.wait_always_timeout = true; break;
            case 3: gpu_mock.wait_hard_error = true; break;
            case 4: gpu_mock.submit_result = VK_ERROR_DEVICE_LOST; break;
            case 5: gpu_mock.fail_release = 1; break;
            case 6: gpu_mock.producer_result = VK_ERROR_DEVICE_LOST; break;
            case 7: gpu_mock.producer_result = VK_ERROR_UNKNOWN; break;
            case 8: gpu_mock.fence_result = VK_TIMEOUT; break;
            case 9: gpu_mock.producer_result = VK_TIMEOUT; break;
            case 10: gpu_mock.fence_result = VK_ERROR_DEVICE_LOST; break;
        }
        tracking_frame(2);
        CHECK(eye_gpu.submitted == 0);
        if (scenario == 7 || scenario == 8 || scenario == 9) {
            CHECK(eye_gpu.current_valid && gpu_mock.retirements == 0);
            CHECK(gpu_mock.releases == 0); // wait failure never grants reuse
            gpu_mock.producer_result = gpu_mock.fence_result = VK_SUCCESS;
        } else CHECK(gpu_mock.retirements == 1);
        if (scenario == 1 || scenario == 2 || scenario == 3) CHECK(gpu_mock.submits == 0 && gpu_mock.releases == 0);
        if (scenario == 2) CHECK(xr.terminal && gpu_mock.acquires == 1 && gpu_mock.waits == 2);
        if (scenario == 3) CHECK(xr.terminal && gpu_mock.acquires == 1 && gpu_mock.waits == 1);
        image_close();
        CHECK(gpu_mock.retirements == 1);
    }
    const XrResult destroy_errors[] = {XR_ERROR_RUNTIME_FAILURE, XR_TIMEOUT_EXPIRED, XR_SESSION_LOSS_PENDING};
    for (unsigned i = 0; i < 3; ++i) {
        XrOwnedEyePair pair = image_prepare();
        CHECK(xr_runtime_offer_eye_pair(&pair));
        mock.expected_layers = 1;
        tracking_frame(2);
        gpu_mock.fail_destroy = 1;
        gpu_mock.destroy_error = destroy_errors[i];
        xr.running = false;
        CHECK(!xr_runtime_shutdown_checked());
        CHECK(eye_gpu.device && eye_gpu.eyes[0].handle && !eye_gpu.eyes[1].handle);
        CHECK(xr.session && atomic_load(&mock.destroy_sessions) == 0 && atomic_load(&mock.destroy_instances) == 0);
        CHECK(gpu_mock.pool_destroys == 0 && gpu_mock.fence_destroys == 0);
        gpu_mock.fail_destroy = 0;
        image_close();
        CHECK(gpu_mock.destroys == 3 && gpu_mock.retirements == 1);
    }
}

// A copy the GPU has not finished within the owner's wait stays in flight: nothing is acquired again, released or
// retired, the owner keeps running and showing the pair before, and the display frame that finds it finished completes
// it. Only a copy unfinished after EYE_COPY_STALL_NS stops the owner.
static void test_image_late_copy(void) {
    XrOwnedEyePair pair = image_prepare();
    CHECK(xr_runtime_offer_eye_pair(&pair));
    mock.expected_layers = 1;
    tracking_frame(2);
    CHECK(eye_gpu.submitted == 1 && eye_gpu.copied == 1 && gpu_mock.fence_asks == 0);
    // The next pair's copy is late.
    CHECK(xr_runtime_get_pose(&pair.render_pose) == XR_POSE_VALID);
    memcpy(pair.rendered_eyes, pair.render_pose.eyes, sizeof pair.rendered_eyes);
    pair.game_present_id = 3;
    CHECK(xr_runtime_offer_eye_pair(&pair));
    gpu_mock.fence_result = VK_TIMEOUT;
    ++mock.predicted;
    tracking_frame(3);  // (the pair before is still what is shown: image_inspect_end checks its poses)
    CHECK(!xr.terminal && eye_gpu.copy_pending && eye_gpu.current_valid && eye_gpu.late_copies == 1);
    CHECK(eye_gpu.submitted == 2 && eye_gpu.copied == 1 && eye_gpu.layer_valid);
    CHECK(gpu_mock.submits == 2 && gpu_mock.acquires == 4 && gpu_mock.releases == 2 && gpu_mock.retirements == 1);
    CHECK(gpu_mock.fences == 2 && gpu_mock.fence_asks == 0);
    // Still late: the owner only asks, and offers behind it wait in the queue.
    ++mock.predicted;
    tracking_frame(0);
    CHECK(!xr.terminal && eye_gpu.copy_pending && eye_gpu.submitted == 3 && eye_gpu.copied == 1);
    CHECK(gpu_mock.submits == 2 && gpu_mock.acquires == 4 && gpu_mock.releases == 2 && gpu_mock.retirements == 1);
    CHECK(gpu_mock.fences == 3 && gpu_mock.fence_asks == 1 && eye_gpu.late_copies == 1);
    // Finished: its images are released, the pair goes back, and its layers are shown.
    gpu_mock.fence_result = VK_SUCCESS;
    memcpy(gpu_mock.expected_eyes, pair.rendered_eyes, sizeof pair.rendered_eyes);
    ++mock.predicted;
    tracking_frame(0);
    CHECK(!xr.terminal && !eye_gpu.copy_pending && !eye_gpu.current_valid);
    CHECK(eye_gpu.submitted == 4 && eye_gpu.copied == 2 && gpu_mock.releases == 4 && gpu_mock.retirements == 2);
    CHECK(gpu_mock.fences == 4 && gpu_mock.fence_asks == 2 && gpu_mock.submits == 2);
    image_close();
    // A producer that outlasts a bounded wait (the retirement of an obsolete pair) keeps its pair, and the owner.
    pair = image_prepare();
    CHECK(xr_runtime_offer_eye_pair(&pair));
    gpu_mock.producer_result = VK_TIMEOUT;
    SDL_Delay(260);  // (the pair's pose is too old to show)
    eye_retire_obsolete();
    CHECK(!xr.terminal && eye_gpu.current_valid && gpu_mock.retirements == 0 && eye_gpu.late_producers == 1);
    tracking_frame(0);
    CHECK(!xr.terminal && eye_gpu.current_valid && gpu_mock.retirements == 0 && gpu_mock.submits == 0);
    gpu_mock.producer_result = VK_SUCCESS;
    tracking_frame(0);
    CHECK(!xr.terminal && !eye_gpu.current_valid && gpu_mock.retirements == 1 && gpu_mock.submits == 0);
    image_close();
    // A producer still unfinished after the stall limit stops the owner.
    pair = image_prepare();
    CHECK(xr_runtime_offer_eye_pair(&pair));
    gpu_mock.producer_result = VK_TIMEOUT;
    tracking_frame(0);
    CHECK(!xr.terminal && eye_gpu.current_valid);
    eye_gpu.current_taken_ns -= EYE_COPY_STALL_NS;
    tracking_frame(0);
    CHECK(xr.terminal && eye_gpu.current_valid && gpu_mock.retirements == 0);
    gpu_mock.producer_result = VK_SUCCESS;
    image_close();
    CHECK(gpu_mock.retirements == 1);
    // A copy still unfinished after the stall limit stops the owner, keeping what it owns.
    pair = image_prepare();
    CHECK(xr_runtime_offer_eye_pair(&pair));
    gpu_mock.fence_result = VK_TIMEOUT;
    tracking_frame(2);
    CHECK(!xr.terminal && eye_gpu.copy_pending);
    eye_gpu.copy.submitted_ns -= EYE_COPY_STALL_NS;
    tracking_frame(0);
    CHECK(xr.terminal && eye_gpu.copy_pending && eye_gpu.current_valid && !eye_gpu.layer_valid);
    CHECK(gpu_mock.releases == 0 && gpu_mock.retirements == 0);
    gpu_mock.fence_result = VK_SUCCESS;
    image_close();
    CHECK(gpu_mock.retirements == 1);
}

static void test_visible_and_wait_recovery(void) {
    XrOwnedEyePair pair = image_prepare();
    xr.state = XR_SESSION_STATE_VISIBLE;
    tracking_frame(2);
    CHECK(xr_runtime_get_pose(&pair.render_pose) == XR_POSE_VALID && !pair.render_pose.focused);
    memcpy(pair.rendered_eyes, pair.render_pose.eyes, sizeof pair.rendered_eyes);
    memcpy(gpu_mock.expected_eyes, pair.rendered_eyes, sizeof pair.rendered_eyes);
    CHECK(xr_runtime_offer_eye_pair(&pair));
    mock.expected_layers = 1;
    tracking_frame(0);
    CHECK(eye_gpu.submitted == 1 && gpu_mock.copies == 2);
    CHECK(xr.outcomes[FRAME_IMAGE] == 1 && xr.visible_images == 1);
    image_close();
    // Timeout each eye (and HUD) in turn; retry the same acquired image,
    // with no copy/release before every wait succeeds.
    for (unsigned slot = 1; slot <= 3; ++slot) {
        pair = image_prepare();
        if (slot == 3) {
            pair.hud_image = (VkImage)(uintptr_t)22;
            pair.hud = (XrEyePanel){.valid = true, .rect = {0, 0, 800, 900},
                .orientation_xyzw = {0, 0, 0, 1}, .width_m = 1, .height_m = 1};
            gpu_mock.expected_hud = &pair.hud;
        }
        gpu_mock.fail_wait = slot;
        CHECK(xr_runtime_offer_eye_pair(&pair));
        mock.expected_layers = slot == 3 ? 2 : 1;
        tracking_frame(0);
        CHECK(!xr.terminal && eye_gpu.submitted == 1 && gpu_mock.retirements == 1);
        const unsigned count = slot == 3 ? 3 : 2;
        CHECK(gpu_mock.acquires == count && gpu_mock.waits == count + 1 && gpu_mock.releases == count);
        image_close();
    }
    for (unsigned reset = 0; reset < 2; ++reset) {
        pair = image_prepare();
        gpu_mock.fail_wait = 1;
        gpu_mock.stop_at_timeout = !reset;
        gpu_mock.reset_at_timeout = reset;
        CHECK(xr_runtime_offer_eye_pair(&pair));
        tracking_frame(0);
        CHECK(eye_gpu.submitted == 0 && gpu_mock.retirements == 1);
        CHECK(gpu_mock.waits == (reset ? 3u : 1u));
        image_close();
    }
}

static void test_image_resubmission(void) {
    XrOwnedEyePair pair = image_prepare();
    // A producer that has not signalled is polled, not waited on or dropped.
    gpu_mock.producer_result = VK_TIMEOUT;
    CHECK(xr_runtime_offer_eye_pair(&pair));
    tracking_frame(0);
    CHECK(eye_gpu.current_valid && gpu_mock.retirements == 0 && !xr.terminal && gpu_mock.copies == 0);
    CHECK(xr.outcomes[FRAME_PRODUCER_PENDING] == 1);
    gpu_mock.producer_result = VK_SUCCESS;
    mock.expected_layers = 1;
    tracking_frame(0);
    CHECK(gpu_mock.copies == 2 && gpu_mock.retirements == 1 && eye_gpu.copied == 1 && eye_gpu.submitted == 1);
    // Display frames without a new pair re-submit the same images and render
    // poses (image_inspect_end checks them against the shifted locate).
    for (unsigned n = 0; n < 3; ++n) { ++mock.predicted; tracking_frame(0); }
    CHECK(eye_gpu.submitted == 4 && gpu_mock.copies == 2 && gpu_mock.acquires == 2 && gpu_mock.releases == 2);
    // Lost tracking skips a frame's layer without dropping the pair.
    mock.view_flags &= ~XR_VIEW_STATE_POSITION_TRACKED_BIT;
    mock.expected_layers = 0;
    tracking_frame(0);
    mock.view_flags |= XR_VIEW_STATE_POSITION_TRACKED_BIT;
    mock.expected_layers = 1;
    tracking_frame(0);
    CHECK(eye_gpu.submitted == 5);
    // Recenter and reset end re-submission; so does age.
    atomic_fetch_add(&reference_generation, 1);
    mock.expected_layers = 0;
    tracking_frame(0);
    CHECK(!eye_gpu.layer_valid && eye_gpu.submitted == 5);
    CHECK(xr.outcomes[FRAME_REFERENCE] == 1 && xr.outcomes[FRAME_TRACKING] == 1);
    image_close();
    pair = image_prepare();
    CHECK(xr_runtime_offer_eye_pair(&pair));
    mock.expected_layers = 1;
    tracking_frame(0);
    xr_runtime_reset_game();
    mock.expected_layers = 0;
    tracking_frame(0);
    CHECK(!eye_gpu.layer_valid && eye_gpu.submitted == 1);
    image_close();
    pair = image_prepare();
    CHECK(xr_runtime_offer_eye_pair(&pair));
    mock.expected_layers = 1;
    tracking_frame(0);
    SDL_Delay(510);
    mock.expected_layers = 0;
    tracking_frame(0);
    CHECK(!eye_gpu.layer_valid && eye_gpu.submitted == 1);
    CHECK(xr.outcomes[FRAME_LAYER_AGE] == 1);
    tracking_frame(0);
    CHECK(xr.outcomes[FRAME_LAYER_AGE] == 1 && xr.outcomes[FRAME_NO_PAIR] == 2);
    xr_runtime_reset_game();
    tracking_frame(0);
    CHECK(xr.outcomes[FRAME_LAYER_AGE] == 1 && xr.outcomes[FRAME_NO_PAIR] == 3);
    image_close();
}

// D081: pairs queue behind the one the owner waits on; each display frame shows the newest finished pair and returns
// the older finished ones unshown, never copied. Unfinished pairs stay queued; a full queue, or images already queued,
// refuse the offer instead of overwriting an owned pair.
static void test_image_queue(void) {
    XrOwnedEyePair pair[4];
    pair[0] = image_prepare();
    for (unsigned i = 1; i < 4; ++i) {
        pair[i] = pair[0];
        pair[i].images[0] = (VkImage)(uintptr_t)(20 + 10 * i);
        pair[i].images[1] = (VkImage)(uintptr_t)(21 + 10 * i);
        pair[i].ready_value = 7 + i;
        pair[i].game_present_id = 2 + i;
    }
    gpu_mock.finished_upto = 6;  // none finished yet
    CHECK(xr_runtime_offer_eye_pair(&pair[0]));
    tracking_frame(0);  // taken and polled: its producer has not finished
    CHECK(eye_gpu.current_valid && !eye_gpu.pending_count && gpu_mock.copies == 0);
    CHECK(xr.outcomes[FRAME_PRODUCER_PENDING] == 1);
    CHECK(xr_runtime_offer_eye_pair(&pair[1]) && xr_runtime_offer_eye_pair(&pair[2]));
    CHECK(!xr_runtime_offer_eye_pair(&pair[3]));  // full: refused, the queue unchanged
    CHECK(eye_gpu.pending_count == 2 && eye_gpu.pending[1].ready_value == 9 && gpu_mock.retirements == 0);
    // The first two finish: the newer is copied, the one waited on goes back unshown; the third stays queued.
    gpu_mock.finished_upto = 8;
    gpu_mock.source_base = 30;
    gpu_mock.expected_value = 8;
    mock.expected_layers = 1;
    ++mock.predicted;
    tracking_frame(0);
    CHECK(gpu_mock.copies == 2 && eye_gpu.copied == 1 && eye_gpu.skipped == 1 && gpu_mock.retirements == 2);
    CHECK(eye_gpu.pending_count == 1 && eye_gpu.pending[0].ready_value == 9 && !eye_gpu.current_valid);
    CHECK(!xr_runtime_offer_eye_pair(&pair[2]));  // its images are queued already
    // Two queued, both finished: the newer is shown, the older returned without a copy.
    CHECK(xr_runtime_offer_eye_pair(&pair[3]));
    gpu_mock.finished_upto = 10;
    gpu_mock.source_base = 50;
    gpu_mock.expected_value = 10;
    ++mock.predicted;
    tracking_frame(0);
    CHECK(gpu_mock.copies == 4 && eye_gpu.copied == 2 && eye_gpu.skipped == 2 && gpu_mock.retirements == 4);
    CHECK(!eye_gpu.pending_count && !eye_gpu.current_valid);
    image_close();
    CHECK(gpu_mock.retirements == 4);
}

// D056: an anchored pair may carry a panel, then re-submitted as one world-locked
// quad showing one eye's image to both eyes; the next scene pair returns to the
// projection layer.
static void test_image_panel(void) {
    XrOwnedEyePair pair = image_prepare(), bad;
    eye_images_enabled = true;
    pair.projection = XR_EYE_PROJECTION_DEV_UNPROVEN;
    pair.anchored = true;
    pair.panel = (XrEyePanel){.valid = true, .eye = 1, .rect = {100, 200, 600, 340},
        .position_xyz = {0.1f, 0.4f, -1.5f}, .orientation_xyzw = {0, 0.0998f, 0, 0.995f}, .width_m = 2.5f, .height_m = 1.4f};
#define REJECT(edit) do { bad = pair; edit; CHECK(!xr_runtime_offer_eye_pair(&bad)); } while (0)
    REJECT(bad.anchored = false); // only 2D content without a render pose
    REJECT(bad.panel.eye = 2);
    REJECT(bad.panel.rect[0] = 201); REJECT(bad.panel.rect[1] = 561); // beyond the 800x900 image
    REJECT(bad.panel.rect[1] = -1); REJECT(bad.panel.rect[2] = 0); REJECT(bad.panel.rect[3] = -4);
    REJECT(bad.panel.width_m = 0); REJECT(bad.panel.height_m = INFINITY);
    REJECT(bad.panel.position_xyz[2] = NAN); REJECT(bad.panel.orientation_xyzw[3] = 0.5f);
#undef REJECT
    CHECK(gpu_mock.retirements == 0 && gpu_mock.creates == 0);
    CHECK(xr_runtime_offer_eye_pair(&pair));
    gpu_mock.expected_panel = &pair.panel;
    mock.expected_layers = 1;
    tracking_frame(0);
    CHECK(eye_gpu.submitted == 1 && eye_gpu.layer_panel && gpu_mock.copies == 2);
    ++mock.predicted;
    tracking_frame(0); // re-submitted without a new copy
    CHECK(eye_gpu.submitted == 2 && gpu_mock.copies == 2);
    CHECK(xr_runtime_get_pose(&pair.render_pose) == XR_POSE_VALID);
    memcpy(pair.rendered_eyes, pair.render_pose.eyes, sizeof pair.rendered_eyes);
    memcpy(gpu_mock.expected_eyes, pair.rendered_eyes, sizeof pair.rendered_eyes);
    pair.anchored = pair.panel.valid = false;
    pair.game_present_id = 3;
    gpu_mock.expected_panel = NULL;
    mock.expected_layers = 1;
    CHECK(xr_runtime_offer_eye_pair(&pair));
    ++mock.predicted;
    tracking_frame(3);
    CHECK(eye_gpu.submitted == 3 && !eye_gpu.layer_panel && gpu_mock.copies == 4);
    eye_images_enabled = false;
    image_close();
}

// D057: a scene pair may carry the HUD's own image: copied into a third
// swapchain made with the first HUD pair, shown as a quad over the projection.
// A pair without one submits the projection alone and keeps the swapchains.
static void test_image_hud(void) {
    XrOwnedEyePair pair = image_prepare(), bad;
    pair.hud_image = (VkImage)(uintptr_t)22;
    pair.hud = (XrEyePanel){.valid = true, .rect = {100, 200, 600, 340}, .position_xyz = {0.1f, 0.4f, -0.77f},
        .orientation_xyzw = {0, 0.0998f, 0, 0.995f}, .width_m = 1.28f, .height_m = 0.72f};
#define REJECT(edit) do { bad = pair; edit; CHECK(!xr_runtime_offer_eye_pair(&bad)); } while (0)
    REJECT(bad.hud_image = VK_NULL_HANDLE); REJECT(bad.hud_image = bad.images[1]);
    REJECT(bad.hud.rect[2] = 701); REJECT(bad.hud.width_m = NAN);
    // (menus: no HUD)
    REJECT(eye_images_enabled = true; bad.projection = XR_EYE_PROJECTION_DEV_UNPROVEN; bad.anchored = true);
#undef REJECT
    eye_images_enabled = false;
    CHECK(gpu_mock.retirements == 0 && gpu_mock.creates == 0);
    CHECK(xr_runtime_offer_eye_pair(&pair));
    gpu_mock.expected_hud = &pair.hud;
    mock.expected_layers = 2;
    tracking_frame(2);
    CHECK(gpu_mock.creates == 3 && gpu_mock.copies == 3 && eye_gpu.eyes[2].handle && eye_gpu.layer_hud);
    CHECK(eye_gpu.submitted == 1);
    ++mock.predicted;
    tracking_frame(0); // re-submitted: the projection and the quad, no new copy
    CHECK(eye_gpu.submitted == 2 && gpu_mock.copies == 3);
    for (unsigned n = 0; n < 2; ++n) {
        CHECK(xr_runtime_get_pose(&pair.render_pose) == XR_POSE_VALID);
        memcpy(pair.rendered_eyes, pair.render_pose.eyes, sizeof pair.rendered_eyes);
        memcpy(gpu_mock.expected_eyes, pair.rendered_eyes, sizeof pair.rendered_eyes);
        pair.game_present_id = 3 + n;
        pair.hud.valid = n == 1;
        gpu_mock.expected_hud = n ? &pair.hud : NULL;
        mock.expected_layers = n ? 2 : 1;
        CHECK(xr_runtime_offer_eye_pair(&pair));
        ++mock.predicted;
        tracking_frame(pair.game_present_id);
        CHECK(eye_gpu.layer_hud == (n == 1));
    }
    CHECK(gpu_mock.creates == 3 && gpu_mock.copies == 8 && gpu_mock.retirements == 3 && eye_gpu.submitted == 4);
    image_close();
    CHECK(gpu_mock.destroys == 3);
}

// The quick menus: an anchored panel pair may show the HUD's image, blended by its alpha, over the last scene pair's
// projection, which stays the eye swapchains' last release: only the panel image is copied. A plain panel pair replaces
// the held scene; an over-scene panel is then shown alone. The next scene pair returns to the projection.
static void test_image_over_scene(void) {
    XrOwnedEyePair pair = image_prepare(), menu, bad;
    CHECK(xr_runtime_offer_eye_pair(&pair));
    mock.expected_layers = 1;
    tracking_frame(2);
    CHECK(eye_gpu.submitted == 1 && gpu_mock.copies == 2 && eye_gpu.scene_valid);
    eye_images_enabled = true;
    menu = pair;
    menu.projection = XR_EYE_PROJECTION_DEV_UNPROVEN;
    menu.anchored = menu.over_scene = true;
    menu.hud_image = (VkImage)(uintptr_t)22;
    menu.panel = (XrEyePanel){.valid = true, .rect = {100, 200, 600, 340}, .position_xyz = {0.1f, 0.4f, -0.77f},
        .orientation_xyzw = {0, 0.0998f, 0, 0.995f}, .width_m = 1.28f, .height_m = 0.72f};
    CHECK(xr_runtime_get_pose(&menu.render_pose) == XR_POSE_VALID);
    memcpy(menu.rendered_eyes, menu.render_pose.eyes, sizeof menu.rendered_eyes);
    menu.game_present_id = 3;
#define REJECT(edit) do { bad = menu; edit; CHECK(!xr_runtime_offer_eye_pair(&bad)); } while (0)
    REJECT(bad.panel.valid = false); REJECT(bad.hud_image = VK_NULL_HANDLE); REJECT(bad.hud_image = bad.images[0]);
    REJECT(bad.hud = bad.panel); // (a HUD is a scene pair's)
#undef REJECT
    CHECK(xr_runtime_offer_eye_pair(&menu));
    gpu_mock.expected_hud = &menu.panel;  // over the projection with the scene pair's views (expected_eyes)
    mock.expected_layers = 2;
    ++mock.predicted;
    tracking_frame(3);
    CHECK(gpu_mock.creates == 3 && gpu_mock.copies == 3 && gpu_mock.acquires == 3 && gpu_mock.releases == 3);
    CHECK(eye_gpu.submitted == 2 && eye_gpu.layer_over && eye_gpu.layer_hud && !eye_gpu.layer_panel);
    ++mock.predicted;
    tracking_frame(0); // re-submitted without a copy
    CHECK(eye_gpu.submitted == 3 && gpu_mock.copies == 3);
    for (unsigned n = 0; n < 2; ++n) {  // a plain panel pair, then an over-scene one with no held scene
        menu.over_scene = n == 1;
        menu.hud_image = n ? (VkImage)(uintptr_t)22 : VK_NULL_HANDLE;
        CHECK(xr_runtime_get_pose(&menu.render_pose) == XR_POSE_VALID);
        memcpy(menu.rendered_eyes, menu.render_pose.eyes, sizeof menu.rendered_eyes);
        menu.game_present_id = 4 + n;
        CHECK(xr_runtime_offer_eye_pair(&menu));
        gpu_mock.expected_hud = NULL;
        gpu_mock.expected_panel = &menu.panel;
        gpu_mock.expected_over = n == 1;
        mock.expected_layers = 1;
        ++mock.predicted;
        tracking_frame(menu.game_present_id);
        CHECK(eye_gpu.layer_panel && !eye_gpu.layer_over && !eye_gpu.scene_valid);
    }
    CHECK(gpu_mock.copies == 6 && eye_gpu.submitted == 5);
    CHECK(xr_runtime_get_pose(&pair.render_pose) == XR_POSE_VALID);
    memcpy(pair.rendered_eyes, pair.render_pose.eyes, sizeof pair.rendered_eyes);
    memcpy(gpu_mock.expected_eyes, pair.rendered_eyes, sizeof pair.rendered_eyes);
    pair.game_present_id = 6;
    gpu_mock.expected_panel = NULL;
    gpu_mock.expected_over = false;
    CHECK(xr_runtime_offer_eye_pair(&pair));
    ++mock.predicted;
    tracking_frame(6);
    CHECK(gpu_mock.copies == 8 && eye_gpu.scene_valid && !eye_gpu.layer_panel && !eye_gpu.layer_over);
    eye_images_enabled = false;
    image_close();
    CHECK(gpu_mock.destroys == 3);
}

// Display-encoded UNORM pixels keep their code values in an sRGB swapchain;
// without one the pair is dropped, never shown through a linear swapchain.
static void test_image_display_encoded(void) {
    XrOwnedEyePair pair = image_prepare();
    pair.display_encoded = true;
    CHECK(xr_runtime_offer_eye_pair(&pair));
    tracking_frame(0);
    CHECK(gpu_mock.creates == 0 && gpu_mock.retirements == 1 && !eye_gpu.layer_valid);
    image_close();
    pair = image_prepare();
    pair.display_encoded = true;
    gpu_mock.offer_srgb = true;
    gpu_mock.expected_format = VK_FORMAT_R8G8B8A8_SRGB;
    CHECK(xr_runtime_offer_eye_pair(&pair));
    mock.expected_layers = 1;
    tracking_frame(0);
    CHECK(gpu_mock.creates == 2 && gpu_mock.copies == 2 && eye_gpu.format == VK_FORMAT_R8G8B8A8_SRGB && eye_gpu.submitted == 1);
    image_close();
}

static void test_offer_rejection_diagnostics(void) {
    XrOwnedEyePair pair = image_prepare(), bad;
    CHECK(!atomic_load(&eye_offer_trace));
    const uint64_t disabled_before = atomic_load(&eye_offer_rejects[OFFER_CONTRACT]);
    CHECK(!xr_runtime_offer_eye_pair(NULL));
    CHECK(atomic_load(&eye_offer_rejects[OFFER_CONTRACT]) == disabled_before);
    atomic_store(&eye_offer_trace, true);
#define DIAGNOSTIC(reason, edit) do { \
    bad = pair; edit; const XrOwnedEyePair before = bad; \
    const uint64_t count = atomic_load(&eye_offer_rejects[reason]); \
    CHECK(!xr_runtime_offer_eye_pair(&bad)); \
    CHECK(atomic_load(&eye_offer_rejects[reason]) == count + 1); \
    CHECK(!memcmp(&before, &bad, sizeof bad) && !eye_gpu.pending_count); \
} while (0)
    DIAGNOSTIC(OFFER_POSE_AGE, bad.render_pose.published_monotonic_ns = xr_runtime_pose_clock_ns() - 300000000);
    DIAGNOSTIC(OFFER_POSE_FUTURE, bad.render_pose.published_monotonic_ns = xr_runtime_pose_clock_ns() + 1000000000);
    DIAGNOSTIC(OFFER_GENERATION, ++bad.render_pose.generation);
    DIAGNOSTIC(OFFER_DEVICE, ++bad.queue_family);
    DIAGNOSTIC(OFFER_EYES, bad.rendered_eyes[0].position_xyz[0] += 1);
    DIAGNOSTIC(OFFER_POSE_IDENTITY, bad.render_pose.active = false);
    // Exhausting the reporting budget never changes the rejection itself.
    atomic_store(&eye_offer_log_count, EYE_OFFER_LOG_CAP);
    DIAGNOSTIC(OFFER_POSE_AGE, bad.render_pose.published_monotonic_ns = xr_runtime_pose_clock_ns() - 300000000);
    CHECK(atomic_load(&eye_offer_log_count) == EYE_OFFER_LOG_CAP + 1);
#undef DIAGNOSTIC
    CHECK(xr_runtime_offer_eye_pair(&pair));
    const XrOwnedEyePair pending = eye_gpu.pending[0];
    const uint64_t mailbox_before = atomic_load(&eye_offer_rejects[OFFER_MAILBOX]);
    CHECK(!xr_runtime_offer_eye_pair(&pair));
    CHECK(atomic_load(&eye_offer_rejects[OFFER_MAILBOX]) == mailbox_before + 1);
    CHECK(!memcmp(&pending, &eye_gpu.pending[0], sizeof pending) && eye_gpu.pending_count == 1);
    image_close();
    CHECK(gpu_mock.retirements == 1);
    atomic_store(&eye_offer_trace, false);
}

int main(void) {
    test_runtime_queue_completion_images();
    test_display_only_owned_pose();
    test_image_admission();
    test_image_panel();
    test_image_hud();
    test_image_over_scene();
    test_image_display_encoded();
    test_image_resubmission();
    test_image_queue();
    test_image_copy_and_resize();
    test_image_drops();
    test_image_failures();
    test_image_late_copy();
    test_visible_and_wait_recovery();
    test_offer_rejection_diagnostics();
    printf("OpenXR owned-image fake-boundary fixture: %u checks passed (no runtime/device/game data)\n", checks);
    return 0;
}
