// Asset-free check of the crosshair's depth samplings across a game reset (runtime/nvn/vulkan/vr_aim.c, compiled here
// as it is): a sampling taken before vr_aim_game_reset is freed when it completes without setting the distance, the
// reset clears vr_aim_result, and a sampling taken after it sets the distance as before. Covers one sampling in flight
// at the reset, every sampling in flight at it, and the ordinary read before and after.
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
static float readback_floats[VR_AIM_SAMPLINGS * VR_AIM_POINTS];
static uint64_t pending_seq = 1, completed_seq, present_count = 100;
static unsigned copies_recorded, invalidations;

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
    (void)allocator; (void)memory; (void)offset; (void)size;
    ++invalidations;
    return VK_SUCCESS;
}
void rec_CmdCopyImageToBuffer(VkCommandBuffer commands, VkImage source, VkImageLayout layout, VkBuffer target,
                              uint32_t count, const VkBufferImageCopy *regions) {
    (void)commands; (void)source; (void)layout; (void)target; (void)regions;
    if (count) ++copies_recorded;
}
uint64_t vk_pending_seq(void) { return pending_seq; }
uint64_t vk_completed_seq(void) { return completed_seq; }
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

// One lens pass: reads what the GPU finished, then samples into the next submission, which is then submitted.
static void lens_pass(void) {
    vr_aim_sample((VkCommandBuffer)(uintptr_t)0xc0, 0, &certificate, scene_depths);
    ++pending_seq;
    ++present_count;
}

static unsigned samplings_in_flight(void) {
    unsigned in_flight = 0;
    for (unsigned k = 0; k < VR_AIM_SAMPLINGS; ++k) in_flight += vr_aim.samples[k].seq != 0;
    return in_flight;
}

static bool distance_is(double expected) { return fabs(vr_aim_result.distance - expected) < 1e-6; }

int main(void) {
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
    completed_seq = pending_seq - 1;
    lens_pass();
    CHECK(distance_is(2.0) && vr_aim_result.frame == present_count - 1 && vr_aim_result.misses == 0);
    CHECK(invalidations == 1 && samplings_in_flight() == 1);

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
    while (samplings_in_flight() < VR_AIM_SAMPLINGS) lens_pass();
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

    if (failures) {
        printf("vr_aim: %u of %u checks failed\n", failures, checks);
        return 1;
    }
    printf("PASS: %u vr_aim checks; samplings across the game reset\n", checks);
    return 0;
}
