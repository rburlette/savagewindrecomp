// The eye images' path from the renderer to the headset. Private implementation, included once by xr_runtime.c.
//
// The renderer (exec.c vk_vr_present) offers each finished eye pair: images it owns, with the GPU timeline
// value at which they are complete and the pose they were rendered for. xr_runtime_offer_eye_pair checks the offer
// and queues it in a small mailbox (pending, EYE_QUEUE deep). Each display frame the XR owner thread takes the newest
// pair the GPU has finished (eye_take_newest_finished, eye_copy_pair), copies its images into the OpenXR swapchains
// on the queue reserved for XR, hands the pair back to the renderer (its release callback), and keeps the resulting
// layers (layer_views, layer_quad) to submit every display frame until a newer pair replaces them
// (eye_submit_layers).
//
// All OpenXR, command pool, swapchain and reserved-queue operations run under xr_vulkan_lock on the one owner
// thread. Only the mailbox is shared with the renderer.
#define EYE_VK_PROCS(X) \
    X(WaitSemaphores) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
    X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(ResetCommandPool) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImage) X(QueueSubmit)

// Pairs queued behind the one the owner waits on (D081). With the GPU overlap the game presents up to two frames
// before the GPU has drawn them; one pending pair dropped the newer ones. The owner shows the newest finished pair
// (eye_take_newest_finished).
#define EYE_QUEUE 2

static struct {
#define EYE_DECLARE(name) PFN_vk##name name;
    EYE_VK_PROCS(EYE_DECLARE)
#undef EYE_DECLARE
    VkDevice device;
    uint32_t queue_family;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    struct {
        XrSwapchain handle;
        XrSwapchainImageVulkanKHR *images;
        uint32_t count;
    } eyes[3];  // left, right, the HUD's (D057: created with the first HUD pair)
    VkFormat format;
    uint32_t width, height;
    XrOwnedEyePair pending[EYE_QUEUE], current;  // pending: oldest first, pending_count of them
    uint64_t pending_epoch[EYE_QUEUE], current_epoch, current_taken_ns, latest_image_generation, image_epoch;
    unsigned pending_count;
    bool current_valid, producer_complete, copy_pending;
    // The latest copied pair, re-submitted every display frame with the poses
    // it was rendered with until a newer pair, reset, recenter or age ends it.
    // A panel pair (D056) is re-submitted as its quad instead; a HUD pair
    // (D057) as the projection with its quad on top, as is an over-scene
    // panel (the quick menus) over the held scene's projection.
    XrCompositionLayerProjectionView layer_views[2];
    XrCompositionLayerQuad layer_quad;
    uint64_t layer_epoch, layer_generation, layer_published_ns;
    XrTime layer_predicted;
    XrEyeProjectionProvenance layer_projection;
    bool layer_valid, layer_panel, layer_hud, layer_over;  // layer_over: a panel over the held scene
    // The views of the last scene pair, whose images the eye swapchains still
    // hold as their last release: an over-scene panel is shown over them.
    XrCompositionLayerProjectionView scene_views[2];
    uint64_t scene_epoch, scene_generation;
    bool scene_valid;
    uint64_t submitted, copied, skipped;
    FrameOutcome zero_outcome;
    // The copy in flight (copy_pending): what its completion needs once the pair itself has gone back to its producer.
    // The swapchain images first to images - 1 are acquired and waited for it, and released when it completes. It
    // stays in flight across display frames while the GPU has not finished it (eye_copy_finish).
    struct {
        XrCompositionLayerProjectionView views[2];
        XrCompositionLayerQuad quad;
        XrPoseSnapshot render_pose;
        XrEyeProjectionProvenance projection;
        uint64_t epoch, submitted_ns;
        uint32_t first, images;
        bool panel, hud, over;
        bool waited;  // the owner has already spent its one bounded wait on this copy; later frames only ask
    } copy;
    uint64_t late_copies;     // copies that outlasted that wait
    uint64_t late_producers;  // bounded waits for a pair's producer that it outlasted
} eye_gpu;
static pthread_mutex_t eye_mailbox_lock = PTHREAD_MUTEX_INITIALIZER;
// Serializes a game reset with the final image-generation check and EndFrame.
// It is never held across producer waits/copies or for tracking-only frames.
static pthread_mutex_t eye_submission_lock = PTHREAD_MUTEX_INITIALIZER;
// A stalled producer/runtime must not trap the sole frame owner indefinitely.
// Timeout fails closed and retains ownership; only proven completion/device
// loss allows retirement. Shutdown still uses the existing queue-idle guard.
#define EYE_GPU_WAIT_NS UINT64_C(100000000)
// GPU work that has not finished within that wait is late, not lost (a busy GPU: the simulator on the same card, a
// capture's wait, a game frame of a third of a second). The owner's copy of a pair stays in flight, with everything
// it owns, and a pair whose producer has not finished stays the current one; the owner asks again each display frame
// without waiting, showing the pair before meanwhile. Only work still unfinished after this long (a copy since its
// submission, a producer since the owner took its pair) is a stalled GPU, and stops the owner as any timeout did
// before.
#define EYE_COPY_STALL_NS UINT64_C(5000000000)
// Longest re-submission of one rendered pair; the compositor reprojects it.
#define EYE_LAYER_MAX_AGE_NS UINT64_C(500000000)

static bool eye_gpu_bind(VkDevice device, uint32_t queue_family) {
    eye_gpu.device = device;
    eye_gpu.queue_family = queue_family;
#define EYE_LOAD(name) do { \
    eye_gpu.name = (PFN_vk##name)vkGetDeviceProcAddr(device, "vk" #name); \
    if (!eye_gpu.name) return false; \
} while (0);
    EYE_VK_PROCS(EYE_LOAD)
#undef EYE_LOAD
    return true;
}

static bool eye_pose_equal(const XrEyeSnapshot *first, const XrEyeSnapshot *second) {
    // All elements originate in the exact runtime snapshot; tolerance could
    // silently admit a projection/eye-order mismatch. Do not compare padding.
    return !memcmp(first->orientation_xyzw, second->orientation_xyzw, sizeof first->orientation_xyzw) &&
        !memcmp(first->position_xyz, second->position_xyz, sizeof first->position_xyz) &&
        !memcmp(first->fov_left_right_up_down, second->fov_left_right_up_down, sizeof first->fov_left_right_up_down);
}

static bool eye_pose_finite(const XrEyeSnapshot *eye) {
    float norm = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(eye->orientation_xyzw[i]) || !isfinite(eye->fov_left_right_up_down[i])) return false;
        norm += eye->orientation_xyzw[i] * eye->orientation_xyzw[i];
    }
    for (unsigned i = 0; i < 3; ++i) if (!isfinite(eye->position_xyz[i])) return false;
    const float *fov = eye->fov_left_right_up_down;
    return norm > 0.99f && norm < 1.01f && fov[0] < fov[1] && fov[3] < fov[2] &&
        fov[0] > -1.5707963f && fov[1] < 1.5707963f && fov[3] > -1.5707963f && fov[2] < 1.5707963f;
}

// One eye's image and a rectangle inside it; a finite, sized, unit-oriented pose.
static bool eye_panel_valid(const XrEyePanel *panel, uint32_t width, uint32_t height) {
    const int32_t *r = panel->rect;
    if (panel->eye > 1 || r[0] < 0 || r[1] < 0 || r[2] <= 0 || r[3] <= 0 ||
        (int64_t)r[0] + r[2] > (int64_t)width || (int64_t)r[1] + r[3] > (int64_t)height) return false;
    float norm = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(panel->orientation_xyzw[i])) return false;
        norm += panel->orientation_xyzw[i] * panel->orientation_xyzw[i];
    }
    for (unsigned i = 0; i < 3; ++i) if (!isfinite(panel->position_xyz[i])) return false;
    return norm > 0.99f && norm < 1.01f && isfinite(panel->width_m) && isfinite(panel->height_m) &&
        panel->width_m > 0 && panel->height_m > 0;
}

static VkFormat eye_target_format(const XrOwnedEyePair *pair) {
    if (!pair->display_encoded) return pair->format;
    return pair->format == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB :
        pair->format == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_UNDEFINED;
}

bool xr_runtime_eye_images_enabled(void) { return eye_images_enabled; }

// Optional bounded, numeric-only admission attribution. Counts and the log
// budget last for the process; diagnostics never change ownership or guards.
typedef enum {
    OFFER_CONTRACT, OFFER_FORMAT, OFFER_ANCHOR, OFFER_PANEL, OFFER_HUD,
    OFFER_POSE_IDENTITY, OFFER_POSE_FUTURE, OFFER_POSE_AGE, OFFER_EYES, OFFER_EYE_ORDER,
    OFFER_STOPPED, OFFER_RESET, OFFER_GENERATION, OFFER_MAILBOX, OFFER_DEVICE, OFFER_IMAGE_GENERATION,
    OFFER_REASON_COUNT
} EyeOfferReject;
static atomic_bool eye_offer_trace;
static atomic_uint_fast64_t eye_offer_rejects[OFFER_REASON_COUNT], eye_offer_log_count;
#define EYE_OFFER_LOG_CAP 128

static bool eye_offer_reject(const XrOwnedEyePair *pair, EyeOfferReject reason, uint64_t now) {
    if (!atomic_load_explicit(&eye_offer_trace, memory_order_relaxed)) return false;
    const uint64_t count = atomic_fetch_add(&eye_offer_rejects[reason], 1) + 1;
    if (count > 4 && count % 120) return false;
    if (atomic_fetch_add(&eye_offer_log_count, 1) >= EYE_OFFER_LOG_CAP) return false;
    static const char *const names[OFFER_REASON_COUNT] = {
        "contract", "format", "anchor", "panel", "hud", "pose-identity", "pose-future", "pose-age",
        "eyes", "eye-order", "stopped", "reset", "generation", "mailbox", "device", "image-generation"
    };
    const uint64_t published = pair ? pair->render_pose.published_monotonic_ns : 0;
    XRLOG("offer-rejected reason=%s count=%llu present=%llu tracking-frame=%llu generation=%llu anchored=%d pose-age-ns=%llu pose-age-valid=%d offer-age-limit-ns=250000000\n",
        names[reason], (unsigned long long)count, (unsigned long long)(pair ? pair->game_present_id : 0),
        (unsigned long long)(pair ? pair->render_pose.frame_id : 0),
        (unsigned long long)(pair ? pair->render_pose.generation : 0), pair && pair->anchored,
        (unsigned long long)(published && now >= published ? now - published : 0),
        published && now >= published);
    return false;
}

// Under eye_mailbox_lock: a queued pair already holds one of these images (the producer gave them away).
static bool eye_images_queued(const XrOwnedEyePair *pair) {
    for (unsigned i = 0; i < eye_gpu.pending_count; ++i)
        for (unsigned side = 0; side < 2; ++side)
            if (pair->images[side] == eye_gpu.pending[i].images[0] ||
                pair->images[side] == eye_gpu.pending[i].images[1])
                return true;
    return false;
}

bool xr_runtime_offer_eye_pair(const XrOwnedEyePair *pair) {
    if (!pair || !atomic_load(&accepting) || !pair->release ||
        !(pair->projection == XR_EYE_PROJECTION_NATIVE_RENDERED ||
          (pair->projection == XR_EYE_PROJECTION_DEV_UNPROVEN && eye_images_enabled)) ||
        !pair->device || !pair->images[0] || !pair->images[1] || pair->images[0] == pair->images[1] ||
        !pair->ready_timeline || !pair->ready_value || !pair->game_present_id || !pair->image_generation ||
        !pair->width || !pair->height || pair->width > INT32_MAX || pair->height > INT32_MAX ||
        !pair->render_pose.native_views_valid) return eye_offer_reject(pair, OFFER_CONTRACT, 0);
    if (pair->format != VK_FORMAT_R8G8B8A8_UNORM && pair->format != VK_FORMAT_R8G8B8A8_SRGB &&
        pair->format != VK_FORMAT_B8G8R8A8_UNORM && pair->format != VK_FORMAT_B8G8R8A8_SRGB) return eye_offer_reject(pair, OFFER_FORMAT, 0);
    if (eye_target_format(pair) == VK_FORMAT_UNDEFINED) return eye_offer_reject(pair, OFFER_FORMAT, 0);
    if (pair->anchored && pair->projection != XR_EYE_PROJECTION_DEV_UNPROVEN) return eye_offer_reject(pair, OFFER_ANCHOR, 0);
    if (pair->panel.valid && (!pair->anchored || !eye_panel_valid(&pair->panel, pair->width, pair->height))) return eye_offer_reject(pair, OFFER_PANEL, 0);
    if (pair->over_scene && (!pair->panel.valid || pair->hud.valid || !pair->hud_image ||
        pair->hud_image == pair->images[0] || pair->hud_image == pair->images[1])) return eye_offer_reject(pair, OFFER_PANEL, 0);
    if (pair->hud.valid && (pair->anchored || !pair->hud_image || pair->hud_image == pair->images[0] ||
        pair->hud_image == pair->images[1] || !eye_panel_valid(&pair->hud, pair->width, pair->height))) return eye_offer_reject(pair, OFFER_HUD, 0);
    // The render pose need not be the latest publication: the display-rate
    // owner publishes newer samples while the GPU renders. It must still be a
    // fresh, valid sample of the current reference space.
    const XrPoseSnapshot *render = &pair->render_pose;
    const uint64_t now = xr_runtime_pose_clock_ns();
    if (!render->frame_id || !render->generation || !render->active ||
        !render->published_monotonic_ns) return eye_offer_reject(pair, OFFER_POSE_IDENTITY, now);
    if (now < render->published_monotonic_ns) return eye_offer_reject(pair, OFFER_POSE_FUTURE, now);
    if (!pair->anchored && now - render->published_monotonic_ns > 250000000)
        return eye_offer_reject(pair, OFFER_POSE_AGE, now);
    for (unsigned i = 0; i < 2; ++i)
        if (!eye_pose_finite(&pair->rendered_eyes[i]) || !eye_pose_equal(&render->eyes[i], &pair->rendered_eyes[i])) return eye_offer_reject(pair, OFFER_EYES, now);
    if (eye_pose_equal(&pair->rendered_eyes[0], &pair->rendered_eyes[1])) return eye_offer_reject(pair, OFFER_EYE_ORDER, now);
    const uint64_t epoch = atomic_load(&reset_epoch);
    bool accepted = false;
    EyeOfferReject reason = OFFER_STOPPED;
    pthread_mutex_lock(&eye_mailbox_lock);
    if (!atomic_load(&accepting)) reason = OFFER_STOPPED;
    else if (epoch != atomic_load(&reset_epoch)) reason = OFFER_RESET;
    else if (render->generation != atomic_load(&reference_generation)) reason = OFFER_GENERATION;
    else if (eye_gpu.pending_count == EYE_QUEUE || eye_images_queued(pair)) reason = OFFER_MAILBOX;
    else if (pair->device != eye_gpu.device || pair->queue_family != eye_gpu.queue_family) reason = OFFER_DEVICE;
    else if (eye_gpu.image_epoch == epoch && pair->image_generation < eye_gpu.latest_image_generation)
        reason = OFFER_IMAGE_GENERATION;
    else {
        XrOwnedEyePair *queued = &eye_gpu.pending[eye_gpu.pending_count];
        *queued = *pair;
        // An anchored pose places 2D content; its freshness is this offer.
        if (pair->anchored) queued->render_pose.published_monotonic_ns = now;
        eye_gpu.pending_epoch[eye_gpu.pending_count++] = epoch;
        eye_gpu.image_epoch = epoch;
        eye_gpu.latest_image_generation = pair->image_generation;
        accepted = true;
    }
    pthread_mutex_unlock(&eye_mailbox_lock);
    return accepted ? true : eye_offer_reject(pair, reason, now);
}

// timeout zero polls: VK_TIMEOUT then means "not yet", never a failure.
static bool eye_wait_producer(uint64_t timeout) {
    if (eye_gpu.producer_complete) return true;
    const XrOwnedEyePair *pair = &eye_gpu.current;
    VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
        .pSemaphores = &pair->ready_timeline, .pValues = &pair->ready_value};
    const uint64_t began = xr_perf.enabled ? SDL_GetTicksNS() : 0;
    VkResult result = eye_gpu.WaitSemaphores(eye_gpu.device, &wait, timeout);
    if (xr_perf.enabled) xr_perf_add(&xr_perf.producer, SDL_GetTicksNS() - began);
    if (result == VK_TIMEOUT) {
        // Not finished yet: the pair stays the current one and is asked about again, unless its producer has stalled.
        const uint64_t waited_ns = xr_runtime_pose_clock_ns() - eye_gpu.current_taken_ns;
        if (waited_ns < EYE_COPY_STALL_NS) {
            if (xr_perf.enabled) ++xr_perf.pending;
            if (timeout && (eye_gpu.late_producers++ < 8 || !(eye_gpu.late_producers % 64)))
                XRLOG("eye producer not finished %llu ms after its pair was taken; the pair waits (late producers: %llu)\n",
                      (unsigned long long)(waited_ns / 1000000), (unsigned long long)eye_gpu.late_producers);
            return false;
        }
    }
    eye_gpu.producer_complete = result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST;
    if (result != VK_SUCCESS) {
        XRLOG("eye producer wait failed: %d; no image submission\n", result);
        xr.terminal = true;
    }
    return eye_gpu.producer_complete;
}

// Hands the current pair back to its producer once the GPU has finished with it: the producer's own work, and the
// owner's copy of it when one was submitted. False: not yet (a copy still in flight keeps the pair and everything it
// owns), or a failure that stopped the owner. copy_wait_ns bounds the wait for the copy (0 only asks).
static bool eye_retire_current_within(uint64_t copy_wait_ns) {
    if (!eye_gpu.current_valid) return true;
    if (!eye_wait_producer(EYE_GPU_WAIT_NS)) return false;
    if (eye_gpu.copy_pending) {
        const uint64_t began = xr_perf.enabled ? SDL_GetTicksNS() : 0;
        VkResult result = eye_gpu.WaitForFences(eye_gpu.device, 1, &eye_gpu.fence, VK_TRUE, copy_wait_ns);
        if (xr_perf.enabled) xr_perf_add(&xr_perf.fence, SDL_GetTicksNS() - began);
        if (result == VK_TIMEOUT) {
            const uint64_t in_flight_ns = xr_runtime_pose_clock_ns() - eye_gpu.copy.submitted_ns;
            if (in_flight_ns < EYE_COPY_STALL_NS) {
                if (!eye_gpu.copy.waited) {
                    eye_gpu.copy.waited = true;
                    if (eye_gpu.late_copies++ < 8 || !(eye_gpu.late_copies % 64))
                        XRLOG("eye copy not finished after %llu ms; it stays in flight, the pair before is shown "
                              "(late copies: %llu)\n", (unsigned long long)(in_flight_ns / 1000000),
                              (unsigned long long)eye_gpu.late_copies);
                }
                return false;
            }
        }
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
            XRLOG("eye copy wait failed: %d; retaining owned resources\n", result);
            xr.terminal = true;
            return false;
        }
        if (result == VK_ERROR_DEVICE_LOST) xr.terminal = true;
        eye_gpu.copy_pending = false;
    }
    void (*release)(void *) = eye_gpu.current.release;
    void *context = eye_gpu.current.context;
    memset(&eye_gpu.current, 0, sizeof eye_gpu.current);
    eye_gpu.current_valid = eye_gpu.producer_complete = false;
    release(context);
    return true;
}

static bool eye_retire_current(void) { return eye_retire_current_within(EYE_GPU_WAIT_NS); }

static bool eye_destroy_swapchains(void) {
    bool success = true;
    for (unsigned i = 0; i < 3; ++i) {
        if (eye_gpu.eyes[i].handle &&
            (!xr.DestroySwapchain || !xr_destroy_ok(XR_CALL("xrDestroySwapchain",
                xr.DestroySwapchain(eye_gpu.eyes[i].handle)), "xrDestroySwapchain"))) {
            // Keep the actual handle so a subsequent shutdown can retry. Do
            // not report complete cleanup after a failed child destruction.
            xr.terminal = true;
            success = false;
            continue;
        }
        free(eye_gpu.eyes[i].images);
        memset(&eye_gpu.eyes[i], 0, sizeof eye_gpu.eyes[i]);
    }
    eye_gpu.layer_valid = eye_gpu.scene_valid = false; // A retained layer names these swapchains.
    eye_gpu.width = eye_gpu.height = 0;
    eye_gpu.format = VK_FORMAT_UNDEFINED;
    return success;
}

static bool eye_create_swapchain(unsigned i, const XrSwapchainCreateInfo *info) {
    uint32_t count = 0;
    if (!xr_ok(XR_CALL("xrCreateSwapchain", xr.CreateSwapchain(xr.session, info, &eye_gpu.eyes[i].handle)), "create eye swapchain") ||
        !xr_ok(xr.EnumerateSwapchainImages(eye_gpu.eyes[i].handle, 0, &count, NULL), "eye image count") ||
        !count || count > 64) return false;
    eye_gpu.eyes[i].images = calloc(count, sizeof *eye_gpu.eyes[i].images);
    if (!eye_gpu.eyes[i].images) return false;
    for (uint32_t image = 0; image < count; ++image)
        eye_gpu.eyes[i].images[image].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    eye_gpu.eyes[i].count = count;
    if (!xr_ok(xr.EnumerateSwapchainImages(eye_gpu.eyes[i].handle, count, &count,
        (XrSwapchainImageBaseHeader *)eye_gpu.eyes[i].images), "eye image data") || count != eye_gpu.eyes[i].count) return false;
    for (uint32_t image = 0; image < count; ++image) if (!eye_gpu.eyes[i].images[image].image) return false;
    return true;
}

static bool eye_create_targets(const XrOwnedEyePair *pair) {
    const VkFormat target = eye_target_format(pair);
    XrSwapchainCreateInfo info = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
        .format = target, .sampleCount = 1, .width = pair->width, .height = pair->height,
        .faceCount = 1, .arraySize = 1, .mipCount = 1};
    if (eye_gpu.eyes[0].handle && eye_gpu.eyes[1].handle && eye_gpu.width == pair->width &&
        eye_gpu.height == pair->height && eye_gpu.format == target) {
        if (!(pair->hud.valid || pair->over_scene) || eye_gpu.eyes[2].handle) return true;
        if (!eye_create_swapchain(2, &info)) goto fail;  // the first HUD pair (D057)
        XRLOG("hud swapchain created %ux%u format=%d images=%u\n", pair->width, pair->height, (int)target, eye_gpu.eyes[2].count);
        return true;
    }
    if (!eye_destroy_swapchains()) return false; // Every prior copy was fenced and released.
    if (xr.max_eye_width && (pair->width > xr.max_eye_width || pair->height > xr.max_eye_height)) {
        static unsigned reported;
        if (reported++ < 4) XRLOG("eye images %ux%u exceed the runtime's maximum %ux%u; pair dropped\n",
                                  pair->width, pair->height, xr.max_eye_width, xr.max_eye_height);
        return false;
    }
    uint32_t count = 0;
    if (!xr_ok(xr.EnumerateSwapchainFormats(xr.session, 0, &count, NULL), "enumerate eye formats") ||
        !count || count > 256) return false;
    int64_t formats[256];
    if (!xr_ok(xr.EnumerateSwapchainFormats(xr.session, count, &count, formats), "eye format data") || count > 256) return false;
    bool supported = false;
    for (uint32_t i = 0; i < count; ++i) if (formats[i] == target) supported = true;
    // Raw copy only: no hidden color conversion/scaling. A display-encoded
    // UNORM source keeps its code values in the size-compatible sRGB target.
    if (!supported) {
        static unsigned reported;
        if (reported++ < 4) XRLOG("eye swapchain format %d unsupported by runtime; pair dropped\n", (int)target);
        return false;
    }
    for (unsigned i = 0; i < (pair->hud.valid || pair->over_scene ? 3u : 2u); ++i) if (!eye_create_swapchain(i, &info)) goto fail;
    eye_gpu.width = pair->width; eye_gpu.height = pair->height; eye_gpu.format = target;
    XRLOG("eye swapchains created %ux%u format=%d images=%u/%u source-format=%d display-encoded=%d\n",
          pair->width, pair->height, (int)target, eye_gpu.eyes[0].count, eye_gpu.eyes[1].count,
          (int)pair->format, pair->display_encoded);
    if (!eye_gpu.pool) {
        VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = eye_gpu.queue_family};
        if (eye_gpu.CreateCommandPool(eye_gpu.device, &pool, NULL, &eye_gpu.pool) != VK_SUCCESS) goto fail;
        VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = eye_gpu.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (eye_gpu.AllocateCommandBuffers(eye_gpu.device, &alloc, &eye_gpu.command) != VK_SUCCESS) goto fail;
        VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (eye_gpu.CreateFence(eye_gpu.device, &fence, NULL, &eye_gpu.fence) != VK_SUCCESS) goto fail;
    }
    return true;
fail:
    eye_destroy_swapchains();
    if (eye_gpu.fence) eye_gpu.DestroyFence(eye_gpu.device, eye_gpu.fence, NULL);
    if (eye_gpu.pool) eye_gpu.DestroyCommandPool(eye_gpu.device, eye_gpu.pool, NULL);
    eye_gpu.fence = VK_NULL_HANDLE;
    eye_gpu.pool = VK_NULL_HANDLE;
    eye_gpu.command = VK_NULL_HANDLE;
    return false;
}

static bool eye_take_pending(uint64_t *epoch) {
    if (eye_gpu.current_valid) return false;
    pthread_mutex_lock(&eye_mailbox_lock);
    if (eye_gpu.pending_count) {  // the oldest
        eye_gpu.current = eye_gpu.pending[0];
        *epoch = eye_gpu.pending_epoch[0];
        eye_gpu.current_taken_ns = xr_runtime_pose_clock_ns();
        eye_gpu.current_valid = true;
        const unsigned rest = --eye_gpu.pending_count;
        memmove(eye_gpu.pending, eye_gpu.pending + 1, rest * sizeof *eye_gpu.pending);
        memmove(eye_gpu.pending_epoch, eye_gpu.pending_epoch + 1, rest * sizeof *eye_gpu.pending_epoch);
        memset(&eye_gpu.pending[rest], 0, sizeof eye_gpu.pending[rest]);
    }
    pthread_mutex_unlock(&eye_mailbox_lock);
    return eye_gpu.current_valid;
}

static bool eye_pair_current(uint64_t epoch, XrTime display_time) {
    const XrOwnedEyePair *pair = &eye_gpu.current;
    const uint64_t now = xr_runtime_pose_clock_ns();
    return !xr.terminal && !atomic_load(&stop_requested) && epoch == atomic_load(&reset_epoch) &&
        pair->render_pose.generation == atomic_load(&reference_generation) &&
        pair->render_pose.predicted_display_time_ns <= display_time &&
        now >= pair->render_pose.published_monotonic_ns && now - pair->render_pose.published_monotonic_ns <= 250000000;
}

static void eye_retire_obsolete(void) {
    for (unsigned slot = 0; slot < EYE_QUEUE; ++slot) {  // oldest first, while the owner waits on none
        bool obsolete = false;
        pthread_mutex_lock(&eye_mailbox_lock);
        if (eye_gpu.pending_count) {
            const XrOwnedEyePair *oldest = &eye_gpu.pending[0];
            const uint64_t now = xr_runtime_pose_clock_ns();
            obsolete = eye_gpu.pending_epoch[0] != atomic_load(&reset_epoch) ||
                oldest->render_pose.generation != atomic_load(&reference_generation) ||
                now < oldest->render_pose.published_monotonic_ns ||
                now - oldest->render_pose.published_monotonic_ns > 250000000;
        }
        pthread_mutex_unlock(&eye_mailbox_lock);
        uint64_t epoch = 0;
        if (!obsolete || !eye_take_pending(&epoch) || !eye_retire_current()) return;
    }
}

// D081: of the pair the owner waits on and those queued behind it, the newest whose producer has finished is shown;
// the older ones go back to their producer unshown, never copied. A finished pair proves the older ones finished only
// when they signal the same timeline at lower values (one queue, in submission order); otherwise each is taken in turn.
// A single pair keeps the usual poll in eye_copy_pair.
static void eye_take_newest_finished(void) {
    if (eye_gpu.copy_pending) return;
    XrOwnedEyePair queued[EYE_QUEUE];
    pthread_mutex_lock(&eye_mailbox_lock);
    const unsigned count = eye_gpu.pending_count;  // (only this owner removes pairs: these stay queued)
    memcpy(queued, eye_gpu.pending, count * sizeof *queued);
    pthread_mutex_unlock(&eye_mailbox_lock);
    if (count + eye_gpu.current_valid < 2) return;
    int newest = -1;
    for (int i = (int)count - 1; i >= 0 && newest < 0; --i) {
        const XrOwnedEyePair *pair = &queued[i];
        bool ordered = !eye_gpu.current_valid || (eye_gpu.current.ready_timeline == pair->ready_timeline &&
                                                  eye_gpu.current.ready_value <= pair->ready_value);
        for (int j = 0; j < i && ordered; ++j)
            ordered = queued[j].ready_timeline == pair->ready_timeline && queued[j].ready_value <= pair->ready_value;
        if (!ordered) continue;
        VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
            .pSemaphores = &pair->ready_timeline, .pValues = &pair->ready_value};
        if (eye_gpu.WaitSemaphores(eye_gpu.device, &wait, 0) == VK_SUCCESS) newest = i;
    }
    if (newest < 0) return;
    // The older ones finished before it: their producer waits return at once, then their release.
    const bool waited_on = eye_gpu.current_valid;
    if (!eye_retire_current()) return;
    unsigned skipped = waited_on;
    uint64_t epoch = 0;
    for (int i = 0; i < newest; ++i, ++skipped)
        if (!eye_take_pending(&epoch) || !eye_retire_current()) return;
    eye_gpu.skipped += skipped;
    if (xr_perf.enabled) xr_perf.skipped += skipped;
    if (eye_take_pending(&eye_gpu.current_epoch)) eye_gpu.producer_complete = true;
}

// The copy in flight, once the GPU has finished it: releases its swapchain images, hands the pair back to its
// producer and makes its layers the ones shown. A copy the GPU has not finished yet stays in flight (copy_pending):
// this display frame shows the layers of the pair before, and the next one asks again.
static void eye_copy_finish(void) {
    const uint64_t epoch = eye_gpu.copy.epoch;
    const XrPoseSnapshot *render_pose = &eye_gpu.copy.render_pose;
    const bool panel = eye_gpu.copy.panel, hud = eye_gpu.copy.hud, over = eye_gpu.copy.over;
    // Completion before xrReleaseSwapchainImage also protects source ownership. The first time the owner waits, within
    // its bound; a copy already late is only asked about.
    if (!eye_retire_current_within(eye_gpu.copy.waited ? 0 : EYE_GPU_WAIT_NS)) {
        if (xr.terminal) eye_gpu.layer_valid = false;
        return;
    }
    if (eye_gpu.copy.waited && (eye_gpu.late_copies <= 8 || !(eye_gpu.late_copies % 64)))
        XRLOG("late eye copy finished after %llu ms\n",
              (unsigned long long)((xr_runtime_pose_clock_ns() - eye_gpu.copy.submitted_ns) / 1000000));
    const bool copied = !xr.terminal;
    bool success = copied && epoch == atomic_load(&reset_epoch) &&
        render_pose->generation == atomic_load(&reference_generation) && !atomic_load(&stop_requested);
    // [pc-vr-audit] (docs/vr-resolution-audit.md; SAVAGEWIND_AUDIT_SUBMIT=1 only): the projection views as submitted,
    // for the first two pairs and every size change.
    static int audit_submit = -1;
    static unsigned audit_logged, audit_w, audit_h;
    if (audit_submit < 0) audit_submit = getenv("SAVAGEWIND_AUDIT_SUBMIT") && !strcmp(getenv("SAVAGEWIND_AUDIT_SUBMIT"), "1");
    if (success && audit_submit && !panel && (audit_logged < 2 || audit_w != eye_gpu.width || audit_h != eye_gpu.height)) {
        ++audit_logged;
        audit_w = eye_gpu.width;
        audit_h = eye_gpu.height;
        const XrCompositionLayerProjectionView *views = eye_gpu.copy.views;
        const XrRect2Di *hud_rect = &eye_gpu.copy.quad.subImage.imageRect;
        for (unsigned i = 0; i < 2; ++i)
            XRLOG("audit submit eye=%u swapchain=%ux%u mips=1 layers=1 rect=%d,%d,%dx%d array-index=%u "
                  "fov_lrud=%.4f,%.4f,%.4f,%.4f hud-quad=%d hud-rect=%d,%d,%dx%d\n", i, eye_gpu.width, eye_gpu.height,
                  views[i].subImage.imageRect.offset.x, views[i].subImage.imageRect.offset.y,
                  views[i].subImage.imageRect.extent.width, views[i].subImage.imageRect.extent.height,
                  views[i].subImage.imageArrayIndex, views[i].fov.angleLeft, views[i].fov.angleRight,
                  views[i].fov.angleUp, views[i].fov.angleDown, (int)hud, hud ? hud_rect->offset.x : 0,
                  hud ? hud_rect->offset.y : 0, hud ? hud_rect->extent.width : 0, hud ? hud_rect->extent.height : 0);
    }
    // Never release an image whose copy remains live: only here, after the copy completed.
    for (unsigned i = eye_gpu.copy.first; i < eye_gpu.copy.images && copied; ++i) {
        if (i < 2) eye_gpu.scene_valid = false;  // (the eye swapchains' last release is no longer the held scene)
        XrSwapchainImageReleaseInfo release = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (!xr_ok(XR_CALL("xrReleaseSwapchainImage", xr.ReleaseSwapchainImage(eye_gpu.eyes[i].handle, &release)), "release eye")) {
            xr.terminal = true;
            success = false;
        }
    }
    if (success) {
        if (!panel) {  // a scene pair: its views stay with the eye images for later over-scene panels
            memcpy(eye_gpu.scene_views, eye_gpu.copy.views, sizeof eye_gpu.scene_views);
            eye_gpu.scene_epoch = epoch;
            eye_gpu.scene_generation = render_pose->generation;
            eye_gpu.scene_valid = true;
        }
        const bool held = over && eye_gpu.scene_valid && eye_gpu.scene_epoch == epoch &&
            eye_gpu.scene_generation == render_pose->generation;
        memcpy(eye_gpu.layer_views, over ? eye_gpu.scene_views : eye_gpu.copy.views, sizeof eye_gpu.layer_views);
        eye_gpu.layer_quad = eye_gpu.copy.quad;
        eye_gpu.layer_panel = panel && !held;
        eye_gpu.layer_hud = hud || held;
        eye_gpu.layer_over = held;
        eye_gpu.layer_epoch = epoch;
        eye_gpu.layer_generation = render_pose->generation;
        eye_gpu.layer_published_ns = render_pose->published_monotonic_ns;
        eye_gpu.layer_predicted = render_pose->predicted_display_time_ns;
        eye_gpu.layer_projection = eye_gpu.copy.projection;
        eye_gpu.layer_valid = true;
        ++eye_gpu.copied;
    } else eye_gpu.layer_valid = false;
}

// Copies a newly admitted pair once into fresh swapchain images. A producer
// that has not signalled yet keeps its pair for the next display frame, and so does a copy the GPU has not finished.
static void eye_copy_pair(XrTime display_time, bool native_tracking_valid) {
    if (eye_gpu.copy_pending) {  // a copy from an earlier display frame, still in flight
        eye_copy_finish();
        return;
    }
    eye_take_newest_finished();
    if (!eye_gpu.current_valid && !eye_take_pending(&eye_gpu.current_epoch)) return;
    const uint64_t epoch = eye_gpu.current_epoch;
    if (!eye_wait_producer(0)) return;  // (the display-rate owner asks, without waiting)
    XrCompositionLayerProjectionView views[2];
    XrCompositionLayerQuad quad = {.type = XR_TYPE_COMPOSITION_LAYER_QUAD};
    bool panel = false, hud = false, over = false;
    uint32_t index[3] = {0, 0, 0}, first = 0, images = 2;
    if (!native_tracking_valid || !eye_pair_current(epoch, display_time)) goto done;
    const XrOwnedEyePair *pair = &eye_gpu.current;
    panel = pair->panel.valid;
    hud = pair->hud.valid;
    over = pair->over_scene;  // (only its panel image, into the HUD's swapchain; the eyes keep the held scene)
    first = over ? 2 : 0;
    images = hud || over ? 3 : 2;
    if (!eye_create_targets(pair)) goto done;
    for (unsigned i = first; i < images; ++i) {
        XrSwapchainImageAcquireInfo acquire = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!xr_ok(XR_CALL("xrAcquireSwapchainImage", xr.AcquireSwapchainImage(eye_gpu.eyes[i].handle, &acquire, &index[i])), "acquire eye")) goto failed_xr;
        XrSwapchainImageWaitInfo wait = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = (XrDuration)EYE_GPU_WAIT_NS};
        const uint64_t began = xr_perf.enabled ? SDL_GetTicksNS() : 0;
        XrResult result = XR_CALL("xrWaitSwapchainImage", xr.WaitSwapchainImage(eye_gpu.eyes[i].handle, &wait));
        // A timeout keeps the same acquisition pending. Retry it once, never
        // acquire another image or copy/release before a successful wait.
        // Keep the owner bounded: persistent stalls still fail closed.
        if (result == XR_TIMEOUT_EXPIRED && !atomic_load(&stop_requested)) {
            XRLOG("eye swapchain wait timeout eye=%u; retrying same acquisition once\n", i);
            result = XR_CALL("xrWaitSwapchainImage(retry)", xr.WaitSwapchainImage(eye_gpu.eyes[i].handle, &wait));
        }
        // XR_TIMEOUT_EXPIRED is a success-class code but does NOT grant access.
        if (xr_perf.enabled) xr_perf_add(&xr_perf.swapchain, SDL_GetTicksNS() - began);
        // (an acquired image whose wait failed is left to session destruction: never copied into or released)
        if (result != XR_SUCCESS || index[i] >= eye_gpu.eyes[i].count) goto failed_xr;
    }
    if (eye_gpu.ResetCommandPool(eye_gpu.device, eye_gpu.pool, 0) != VK_SUCCESS ||
        eye_gpu.ResetFences(eye_gpu.device, 1, &eye_gpu.fence) != VK_SUCCESS) goto failed_gpu;
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (eye_gpu.BeginCommandBuffer(eye_gpu.command, &begin) != VK_SUCCESS) goto failed_gpu;
    for (unsigned i = first; i < images; ++i) {
        VkImage image = eye_gpu.eyes[i].images[index[i]].image;
        VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
        eye_gpu.CmdPipelineBarrier(eye_gpu.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &barrier);
        VkImageCopy region = {.srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
            .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
            .extent = {pair->width, pair->height, 1}};
        eye_gpu.CmdCopyImage(eye_gpu.command, i < 2 ? pair->images[i] : pair->hud_image, VK_IMAGE_LAYOUT_GENERAL,
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        eye_gpu.CmdPipelineBarrier(eye_gpu.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, NULL, 0, NULL, 1, &barrier);
        if (i == 2) continue;  // (the HUD: its quad below)
        const XrEyeSnapshot *eye = &pair->rendered_eyes[i];
        views[i] = (XrCompositionLayerProjectionView){.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
            .pose = {.orientation = {eye->orientation_xyzw[0], eye->orientation_xyzw[1], eye->orientation_xyzw[2], eye->orientation_xyzw[3]},
                .position = {eye->position_xyz[0], eye->position_xyz[1], eye->position_xyz[2]}},
            .fov = {eye->fov_left_right_up_down[0], eye->fov_left_right_up_down[1], eye->fov_left_right_up_down[2], eye->fov_left_right_up_down[3]},
            .subImage = {.swapchain = eye_gpu.eyes[i].handle,
                .imageRect = {.extent = {(int32_t)pair->width, (int32_t)pair->height}}, .imageArrayIndex = 0}};
    }
    if (panel || hud) {
        // A menu panel shows one eye image; the HUD its own image, premultiplied, over the projection (D057), as does
        // an over-scene panel.
        const XrEyePanel *shown = hud ? &pair->hud : &pair->panel;
        const bool alpha = hud || over;
        quad = (XrCompositionLayerQuad){.type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = xr.local,
            .layerFlags = alpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0,
            .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
            .subImage = {.swapchain = eye_gpu.eyes[alpha ? 2 : shown->eye].handle, .imageArrayIndex = 0,
                .imageRect = {.offset = {shown->rect[0], shown->rect[1]}, .extent = {shown->rect[2], shown->rect[3]}}},
            .pose = {.orientation = {shown->orientation_xyzw[0], shown->orientation_xyzw[1], shown->orientation_xyzw[2],
                                     shown->orientation_xyzw[3]},
                .position = {shown->position_xyz[0], shown->position_xyz[1], shown->position_xyz[2]}},
            .size = {shown->width_m, shown->height_m}};
    }
    if (eye_gpu.EndCommandBuffer(eye_gpu.command) != VK_SUCCESS) goto failed_gpu;
    // The GPU wait provides cross-queue memory visibility even though the CPU
    // already waited to make drop/retirement paths safe. Host waiting alone is
    // not a substitute for the image read's semaphore memory dependency.
    VkTimelineSemaphoreSubmitInfo timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &pair->ready_value};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &pair->ready_timeline, .pWaitDstStageMask = &stage,
        .commandBufferCount = 1, .pCommandBuffers = &eye_gpu.command};
    if (eye_gpu.QueueSubmit(xr.queue, 1, &submit, eye_gpu.fence) != VK_SUCCESS) goto failed_gpu;
    eye_gpu.copy_pending = true;
    // What the copy's completion needs: from here the pair's images are the GPU's until the fence, and every swapchain
    // image first to images - 1 is acquired and waited.
    memcpy(eye_gpu.copy.views, views, sizeof views);
    eye_gpu.copy.quad = quad;
    eye_gpu.copy.render_pose = pair->render_pose;
    eye_gpu.copy.projection = pair->projection;
    eye_gpu.copy.epoch = epoch;
    eye_gpu.copy.submitted_ns = xr_runtime_pose_clock_ns();
    eye_gpu.copy.first = first;
    eye_gpu.copy.images = images;
    eye_gpu.copy.panel = panel;
    eye_gpu.copy.hud = hud;
    eye_gpu.copy.over = over;
    eye_gpu.copy.waited = false;
    eye_copy_finish();
    return;
failed_gpu:
    XRLOG("eye copy Vulkan operation failed; restart required\n");
failed_xr:
    xr.terminal = true;
done:
    // Nothing was submitted: the pair goes back uncopied, and no swapchain image is released (one acquired before a
    // failure is left to session destruction).
    eye_retire_current();
    if (xr.terminal) eye_gpu.layer_valid = false;
}

// The retained layers for this display frame, as a count: the projection
// with the poses/FOV the eyes were rendered with (never the newer locate, so
// the compositor reprojects them) and a HUD pair's quad on top, or a menu
// panel's quad alone.
static uint32_t eye_layer_current(XrTime display_time, bool native_tracking_valid,
                                  XrCompositionLayerProjection *projection, const XrCompositionLayerBaseHeader *layers[2]) {
    if (!eye_gpu.layer_valid) {
        // Age/reference count the frame that retires the retained layer.
        // Later frames have no current pair; do not carry an obsolete cause
        // across a new producer attempt, reset or reference-space definition.
        eye_gpu.zero_outcome = eye_gpu.current_valid ? FRAME_PRODUCER_PENDING : FRAME_NO_PAIR;
        return 0;
    }
    const uint64_t now = xr_runtime_pose_clock_ns();
    if (xr.terminal || atomic_load(&stop_requested)) {
        eye_gpu.zero_outcome = xr.terminal ? FRAME_TERMINAL : FRAME_CANCELLED;
        eye_gpu.layer_valid = false;
        return 0;
    }
    if (eye_gpu.layer_epoch != atomic_load(&reset_epoch) ||
        eye_gpu.layer_generation != atomic_load(&reference_generation)) {
        eye_gpu.zero_outcome = FRAME_REFERENCE;
        eye_gpu.layer_valid = false;
        return 0;
    }
    if (now < eye_gpu.layer_published_ns || now - eye_gpu.layer_published_ns > EYE_LAYER_MAX_AGE_NS) {
        eye_gpu.zero_outcome = FRAME_LAYER_AGE;
        eye_gpu.layer_valid = false;
        return 0;
    }
    if (!native_tracking_valid || eye_gpu.layer_predicted > display_time) {
        eye_gpu.zero_outcome = !native_tracking_valid ? FRAME_TRACKING : FRAME_LAYER_FUTURE;
        return 0;
    }
    eye_gpu.zero_outcome = FRAME_NO_PAIR;
    if (eye_gpu.layer_panel) {
        layers[0] = (const XrCompositionLayerBaseHeader *)&eye_gpu.layer_quad;
        return 1;
    }
    *projection = (XrCompositionLayerProjection){.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        .space = xr.local, .viewCount = 2, .views = eye_gpu.layer_views};
    layers[0] = (const XrCompositionLayerBaseHeader *)projection;
    if (!eye_gpu.layer_hud) return 1;
    layers[1] = (const XrCompositionLayerBaseHeader *)&eye_gpu.layer_quad;
    return 2;
}

static uint32_t eye_submit_layers(XrTime display_time, bool native_tracking_valid,
                                  XrCompositionLayerProjection *projection, const XrCompositionLayerBaseHeader *layers[2]) {
    eye_copy_pair(display_time, native_tracking_valid);
    return eye_layer_current(display_time, native_tracking_valid, projection, layers);
}

static bool eye_gpu_shutdown(void) {
    if (!eye_retire_current()) return false;
    uint64_t epoch = 0;
    while (eye_take_pending(&epoch)) if (!eye_retire_current()) return false;
    if (!eye_destroy_swapchains()) return false;
    if (eye_gpu.fence) eye_gpu.DestroyFence(eye_gpu.device, eye_gpu.fence, NULL);
    if (eye_gpu.pool) eye_gpu.DestroyCommandPool(eye_gpu.device, eye_gpu.pool, NULL);
    pthread_mutex_lock(&eye_mailbox_lock);
    memset(&eye_gpu, 0, sizeof eye_gpu);
    pthread_mutex_unlock(&eye_mailbox_lock);
    return true;
}
