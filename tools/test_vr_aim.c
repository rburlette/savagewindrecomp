// Asset-free check of the crosshair's depth samplings (runtime/nvn/vulkan/vr_aim.c, compiled here
// as it is): a sampling taken before vr_aim_game_reset is freed when it completes without setting the distance, the
// reset clears vr_aim_result, and a sampling taken after it sets the distance as before. Covers one sampling in flight
// at the reset, every sampling in flight at it, and the ordinary read before and after. Also checks two captures
// completing together in reused slots, and an older capture completing after a newer out-of-view result. Each copy
// must be made visible to the host and its submission waited before invalidation, even with a neighboring slot busy.
// A submission is waited for only once the counter has passed it, and once for each sampling.
//
// The stubs below stand for what the module calls (the readback buffer's allocation and invalidation, the depth copy,
// the submission numbers, the present count, the game's depth mode): the fixture plays the GPU by advancing the
// completed submission, and the scene by the depths in the readback buffer. No GPU, OpenXR runtime or game is
// involved.
#include "../runtime/nvn/vulkan/vr_aim.c"

#include <stdio.h>
#include <stdlib.h>

VkG g_vk;
VrAimResult vr_aim_result;  // (in the runtime exec.c defines it)

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; if (failures < 10) printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

// ------------------------------------------------------------------------------------------------- what the module calls
enum { MOCK_ATOM_SIZE = 256 };  // larger than one sampling's 224 bytes
static float readback_floats[VR_AIM_SAMPLINGS * MOCK_ATOM_SIZE / sizeof(float)];
static uint64_t pending_seq = 1, completed_seq, present_count = 100;
static unsigned copies_recorded, invalidations, waits;
static VkDeviceSize last_copy_offset, last_copy_size;
static uint64_t barrier_submission[VR_AIM_SAMPLINGS], waited_submission;

VkResult vmaCreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo *buffer_info,
                         const VmaAllocationCreateInfo *memory_info, VkBuffer *buffer, VmaAllocation *memory,
                         VmaAllocationInfo *allocation_info) {
    (void)allocator; (void)memory_info;
    if (buffer_info->size != sizeof readback_floats) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *buffer = (VkBuffer)(uintptr_t)0xb0;
    *memory = (VmaAllocation)(uintptr_t)0xb1;
    *allocation_info = (VmaAllocationInfo){.pMappedData = readback_floats};
    return VK_SUCCESS;
}
VkResult vmaInvalidateAllocation(VmaAllocator allocator, VmaAllocation memory, VkDeviceSize offset, VkDeviceSize size) {
    (void)allocator;
    CHECK(memory == vr_aim.memory && offset % MOCK_ATOM_SIZE == 0 && size > 0 && size <= MOCK_ATOM_SIZE);
    const unsigned slot = (unsigned)(offset / MOCK_ATOM_SIZE);
    CHECK(slot < VR_AIM_SAMPLINGS && barrier_submission[slot] != 0 && barrier_submission[slot] <= waited_submission);
    ++invalidations;
    return VK_SUCCESS;
}
void rec_CmdCopyImageToBuffer(VkCommandBuffer commands, VkImage source, VkImageLayout layout, VkBuffer target,
                              uint32_t count, const VkBufferImageCopy *regions) {
    (void)commands; (void)source; (void)layout;
    CHECK(target == vr_aim.buffer && count > 0 && count <= VR_AIM_POINTS && !last_copy_size);
    if (!count) return;
    last_copy_offset = regions[0].bufferOffset;
    last_copy_size = count * sizeof(float);
    CHECK(last_copy_offset % MOCK_ATOM_SIZE == 0 && last_copy_offset + last_copy_size <= sizeof readback_floats);
    for (unsigned point = 0; point < count; ++point)
        CHECK(regions[point].bufferOffset == last_copy_offset + point * sizeof(float));
    ++copies_recorded;
}
void rec_CmdPipelineBarrier2(VkCommandBuffer commands, const VkDependencyInfo *dependency) {
    (void)commands;
    CHECK(last_copy_size && dependency->bufferMemoryBarrierCount == 1);
    if (dependency->bufferMemoryBarrierCount != 1) return;
    const VkBufferMemoryBarrier2 *barrier = dependency->pBufferMemoryBarriers;
    CHECK(barrier->srcStageMask == VK_PIPELINE_STAGE_2_COPY_BIT && barrier->srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT);
    CHECK(barrier->dstStageMask == VK_PIPELINE_STAGE_2_HOST_BIT && barrier->dstAccessMask == VK_ACCESS_2_HOST_READ_BIT);
    CHECK(barrier->buffer == vr_aim.buffer && barrier->offset == last_copy_offset && barrier->size == last_copy_size);
    CHECK(barrier->srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && barrier->dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    const unsigned slot = (unsigned)(last_copy_offset / MOCK_ATOM_SIZE);
    if (slot < VR_AIM_SAMPLINGS) barrier_submission[slot] = pending_seq;
    last_copy_size = 0;
}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *wait, uint64_t timeout) {
    CHECK(device == g_vk.dev && timeout == 0 && wait->semaphoreCount == 1 && *wait->pSemaphores == g_vk.timeline);
    const uint64_t requested = *wait->pValues;
    CHECK(requested != 0 && requested <= completed_seq);  // (an unfinished one is never asked about)
    waited_submission = requested;
    ++waits;
    return VK_SUCCESS;
}
uint64_t vk_completed_seq(void) { return completed_seq; }
uint64_t vk_pending_seq(void) { return pending_seq; }
uint64_t renderer_present_count(void) { return present_count; }
uint32_t nvn_depth_mode(void) { return 1; }  // (0..1 depth)
void vk_device_fault_report(void) {}
void rt_fatal_trace(void) { abort(); }

// ----------------------------------------------------------------------------------------------------------- the frame
// The eye: at the calibration origin, looking along it (LOCAL -Z), 10 game units per metre. Its depth terms make a
// stored depth d a view z of -1/d: with every stored depth 0.5 the scene is 2 game units ahead, which the ray meets.
static const double units_per_meter = 10.0;
static PcVrRenderCertificate certificate;
static TexBackend scene_depth_image;
static const TexBackend *scene_depths[2] = {&scene_depth_image, &scene_depth_image};

static void set_scene(float stored_depth) {
    for (unsigned i = 0; i < sizeof readback_floats / sizeof readback_floats[0]; ++i) readback_floats[i] = stored_depth;
}

static void record_lens_pass(void) {
    vr_aim_sample((VkCommandBuffer)(uintptr_t)0xc0, 0, &certificate, scene_depths);
    ++present_count;
}

// Most cases submit after every lens pass. Two captures can also be recorded before one submission.
static void lens_pass(void) {
    record_lens_pass();
    ++pending_seq;
}

static unsigned samplings_in_flight(void) {
    unsigned in_flight = 0;
    for (unsigned k = 0; k < VR_AIM_SAMPLINGS; ++k) in_flight += vr_aim.samples[k].seq != 0;
    return in_flight;
}

static bool distance_is(double expected) { return fabs(vr_aim_result.distance - expected) < 1e-6; }

static void set_slot_depth(unsigned slot, float stored_depth) {
    for (unsigned point = 0; point < VR_AIM_POINTS; ++point)
        readback_floats[slot * MOCK_ATOM_SIZE / sizeof(float) + point] = stored_depth;
}

static void check_neighbor_in_flight(void) {
    const uint64_t first_submission = pending_seq;
    lens_pass();
    lens_pass();
    set_slot_depth(0, 0.5f);
    set_slot_depth(1, 0.25f);
    const unsigned invalidations_before = invalidations;
    completed_seq = first_submission;
    vr_aim_read();
    CHECK(distance_is(2.0) && samplings_in_flight() == 1 && invalidations == invalidations_before + 1);
    CHECK(vr_aim.samples[1].seq == first_submission + 1);
    completed_seq = first_submission + 1;
    vr_aim_read();
    CHECK(distance_is(4.0) && samplings_in_flight() == 0 && invalidations == invalidations_before + 2);
}

static void check_reused_slot_order(bool shared_submission) {
    const uint64_t first_submission = pending_seq;
    lens_pass();                         // slot 0
    set_slot_depth(0, 0.5f);
    record_lens_pass();                  // slot 1, still waiting on the GPU
    set_slot_depth(1, 0.25f);            // 4 game units
    if (!shared_submission) ++pending_seq;
    completed_seq = first_submission;
    lens_pass();                         // consume slot 0, then reuse it for the newest capture
    CHECK(distance_is(2.0) && samplings_in_flight() == 2);
    CHECK((vr_aim.samples[0].seq == vr_aim.samples[1].seq) == shared_submission);
    set_slot_depth(0, 0.125f);           // 8 game units
    completed_seq = pending_seq - 1;     // both slots are now ready
    vr_aim_read();
    printf("Aim depths 4 then 8 (%s): published %.1f\n",
           shared_submission ? "one submission" : "separate submissions", vr_aim_result.distance);
    CHECK(distance_is(8.0) && samplings_in_flight() == 0);
}

static void check_newer_offscreen_result(void) {
    lens_pass();
    set_slot_depth(0, 0.5f);             // 2 game units, readback still pending
    XrEyeSnapshot *eye = &certificate.native_snapshot.eyes[0];
    memcpy(eye->orientation_xyzw, (const float[4]){0, 1, 0, 0}, sizeof eye->orientation_xyzw);
    lens_pass();                         // no point on the ray is in view
    const uint64_t offscreen_frame = vr_aim_result.frame;
    CHECK(distance_is(VR_AIM_MAX_M * units_per_meter) && offscreen_frame == present_count - 1);
    completed_seq = pending_seq - 1;
    vr_aim_sample((VkCommandBuffer)(uintptr_t)0xc0, -1, NULL, NULL);  // read with no new capture
    printf("Aim depth after older readback completed: %.1f (offscreen result %.1f)\n",
           vr_aim_result.distance, VR_AIM_MAX_M * units_per_meter);
    CHECK(distance_is(VR_AIM_MAX_M * units_per_meter) && vr_aim_result.frame == offscreen_frame);
    CHECK(samplings_in_flight() == 0);
    memcpy(eye->orientation_xyzw, (const float[4]){0, 0, 0, 1}, sizeof eye->orientation_xyzw);
}

int main(void) {
    g_vk.props.limits.nonCoherentAtomSize = MOCK_ATOM_SIZE;
    g_vk.dev = (VkDevice)(uintptr_t)0xa0;
    g_vk.timeline = (VkSemaphore)(uintptr_t)0xa1;
    certificate.units_per_meter = units_per_meter;
    memcpy(certificate.depth_terms, (const float[4]){0, 1, -1, 0}, sizeof certificate.depth_terms);
    certificate.native_snapshot.calibration = (XrCalibration){.epoch = 1, .heading_xyzw = {0, 0, 0, 1}};
    for (unsigned eye = 0; eye < 2; ++eye) {
        XrEyeSnapshot *eye_pose = &certificate.native_snapshot.eyes[eye];
        memcpy(eye_pose->orientation_xyzw, (const float[4]){0, 0, 0, 1}, sizeof eye_pose->orientation_xyzw);
        memcpy(eye_pose->fov_left_right_up_down, (const float[4]){-0.7f, 0.7f, 0.7f, -0.7f},
               sizeof eye_pose->fov_left_right_up_down);
    }
    scene_depth_image = (TexBackend){.image = (VkImage)(uintptr_t)0xd0, .samples = 1, .format = VK_FORMAT_D32_SFLOAT,
                                     .width = 64, .height = 64};
    set_scene(0.5f);

    // The ordinary read: a sampling sets the distance once the GPU has passed it.
    lens_pass();
    CHECK(copies_recorded == 1 && samplings_in_flight() == 1 && vr_aim_result.distance == 0);
    vr_aim_read();
    CHECK(samplings_in_flight() == 1 && invalidations == 0 && waits == 0 && vr_aim_result.distance == 0);
    completed_seq = pending_seq - 1;
    lens_pass();
    CHECK(distance_is(2.0) && vr_aim_result.frame == present_count - 1 && vr_aim_result.misses == 0);
    CHECK(invalidations == 1 && waits == 1 && samplings_in_flight() == 1);
    vr_aim_read();
    CHECK(waits == 1);  // (the freed sampling is not waited for again, nor the one in flight)

    // One sampling in flight at the reset: the reset clears the result, and the sampling, once complete, is freed
    // without setting it.
    const uint64_t generation_before = vr_aim.generation;
    vr_aim_game_reset();
    CHECK(vr_aim.generation == generation_before + 1);
    CHECK(vr_aim_result.distance == 0 && vr_aim_result.frame == 0 && vr_aim_result.misses == 0);
    CHECK(samplings_in_flight() == 1);  // (still in flight until the read sees it complete)
    completed_seq = pending_seq - 1;  // (the reset idles the GPU)
    lens_pass();
    CHECK(vr_aim_result.distance == 0 && vr_aim_result.misses == 0 && invalidations == 1);
    CHECK(samplings_in_flight() == 1);  // (the old one freed, the new game's taken)
    for (unsigned k = 0; k < VR_AIM_SAMPLINGS; ++k)
        if (vr_aim.samples[k].seq) CHECK(vr_aim.samples[k].generation == vr_aim.generation);

    // The new game's sampling reads as before: the sky this time (stored depth 0), so the farthest depth.
    set_scene(0.0f);
    completed_seq = pending_seq - 1;
    lens_pass();
    CHECK(distance_is(VR_AIM_MAX_M * units_per_meter) && vr_aim_result.misses == 1 && invalidations == 2);

    // Every sampling in flight at the reset: none of them sets the distance, and all their slots come back.
    set_scene(0.5f);
    completed_seq = 0;  // (the GPU stalls: the samplings pile up)
    for (unsigned capture = 0; capture < VR_AIM_SAMPLINGS; ++capture) lens_pass();
    CHECK(samplings_in_flight() == VR_AIM_SAMPLINGS);
    const unsigned copies_when_full = copies_recorded;
    lens_pass();
    CHECK(copies_recorded == copies_when_full);  // (no free sampling: nothing copied)
    vr_aim_game_reset();
    CHECK(vr_aim_result.distance == 0 && vr_aim_result.misses == 0);
    completed_seq = pending_seq - 1;
    const unsigned invalidations_before = invalidations;
    lens_pass();
    CHECK(vr_aim_result.distance == 0 && invalidations == invalidations_before);
    CHECK(samplings_in_flight() == 1 && copies_recorded == copies_when_full + 1);
    completed_seq = pending_seq - 1;
    lens_pass();
    CHECK(distance_is(2.0) && vr_aim_result.misses == 0);

    // Two resets in a row: still one generation newer than the sampling taken before them.
    vr_aim_game_reset();
    vr_aim_game_reset();
    completed_seq = pending_seq - 1;
    lens_pass();
    CHECK(vr_aim_result.distance == 0);

    vr_aim_game_reset();
    completed_seq = pending_seq - 1;
    vr_aim_read();                       // drain the last capture from the reset cases
    check_neighbor_in_flight();
    check_reused_slot_order(false);
    check_reused_slot_order(true);
    check_newer_offscreen_result();

    if (failures) {
        printf("vr_aim: %u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("PASS: %u vr_aim checks across game resets and capture order\n", checks);
    return 0;
}
