// PC VR renderer: the aim ray's distance, from the scene's depth (D073). What it is for, when it is sampled and read,
// units, the depth convention, the buffer's ownership, serialisation and what is open: vr_aim.h. Moved here from
// exec.c; the bodies are exec.c's.
#include <math.h>
#include <string.h>

#include "vk.h"
#include "record.h"
#include "vr_aim.h"
#include "vr_math.h"

uint64_t renderer_present_count(void);  // (device.c) the presents so far
uint32_t nvn_depth_mode(void);          // (objects.c) the game's depth mode: 0 OpenGL's -1..1, 1 0..1

#define VR_AIM_SAMPLINGS 4     // ray samplings in flight at once
#define VR_AIM_COARSE_POINTS 40   // the ray's points from 0.25 to 4000 game units, each 28% further than the last
#define VR_AIM_FINE_POINTS 16     // and points within 15% of the last hit
#define VR_AIM_POINTS (VR_AIM_COARSE_POINTS + VR_AIM_FINE_POINTS)

// The aim's own state, for the process: the game reset bumps its generation (vr_aim_game_reset, vr_aim.h). Its result
// is vr_aim_result.
static struct {
    // The host-visible buffer the depth copies land in: VR_AIM_POINTS floats per sampling.
    VkBuffer buffer;
    VmaAllocation memory;
    const float *readback;
    uint64_t generation;  // the game resets so far: a sampling taken in an older one is discarded when it completes
    // One sampling of the ray, in flight until the GPU passes seq.
    struct {
        uint64_t seq;    // the submission that copies its depths (0: free)
        uint64_t generation;  // vr_aim.generation when it was taken
        unsigned steps;  // how many of the ray's points were in the eye's view, and copied
        // Each copied point: its distance along the ray, and its depth in the eye's view (game units).
        double ray_distance[VR_AIM_POINTS], eye_depth[VR_AIM_POINTS];
        float depth_terms[4];  // the eye projection's depth terms (its certificate), to undo the stored depth
        double units_per_meter;
        // The game's depth mode then was OpenGL's: clip depth -1..1, stored as half of it plus a half.
        bool gl_depth_range;
    } samples[VR_AIM_SAMPLINGS];

    uint64_t logged;  // the readings so far (the log shows the first 8 and every 600th)
    int state;  // 0 not set up, 1 ready, -1 off
} vr_aim;

// Reads the ray samplings the GPU has finished. Where each one meets the scene: vr_aim_ray_hit (vr_math.h), from each
// copied point's stored scene depth against the point's own depth. A ray that meets nothing (the sky) gives
// VR_AIM_MAX_M.
static void vr_aim_read(void) {
    if (vr_aim.state <= 0) return;
    const uint64_t done = vk_completed_seq();
    for (unsigned k = 0; k < VR_AIM_SAMPLINGS; ++k) {
        if (!vr_aim.samples[k].seq || vr_aim.samples[k].seq > done) continue;
        vr_aim.samples[k].seq = 0;
        // (taken before a game reset: its depths are the old game's scene)
        if (vr_aim.samples[k].generation != vr_aim.generation) continue;
        const unsigned steps = vr_aim.samples[k].steps;
        const size_t base = (size_t)k * VR_AIM_POINTS * 4;
        VK_CHECK(vmaInvalidateAllocation(g_vk.vma, vr_aim.memory, base, VR_AIM_POINTS * 4));
        const float *depth = vr_aim.readback + (size_t)k * VR_AIM_POINTS;
        unsigned hit_step;  // the point at the hit (for the log)
        // (the distance along the ray where it meets the scene, 0 when it meets nothing: vr_math.h)
        double hit = vr_aim_ray_hit(depth, vr_aim.samples[k].ray_distance, vr_aim.samples[k].eye_depth, steps,
                                    vr_aim.samples[k].depth_terms, vr_aim.samples[k].gl_depth_range, &hit_step);
        // No farther than VR_AIM_MAX_M at the eye's world scale, and there when the ray meets nothing (the
        // sky, or past the eye's view): further away the two eyes' views of it hardly differ.
        const double cap = VR_AIM_MAX_M * vr_aim.samples[k].units_per_meter;
        if (!hit) ++vr_aim_result.misses;
        if (!hit || hit > cap) hit = cap;
        if (!(hit > 0)) continue;
        vr_aim_result.distance = hit;
        vr_aim_result.frame = renderer_present_count();
        if (vr_aim.logged < 8 || !(vr_aim.logged % 600)) {
            const float *terms = vr_aim.samples[k].depth_terms;
            LOG("[pc-vr-world-hud] aim distance=%.2f (step %u of %u, scene depth %.7f there; depth terms %.6g %.6g %.6g %.6g)\n",
                hit, hit_step, steps, depth[hit_step], terms[0], terms[1], terms[2], terms[3]);
        }
        ++vr_aim.logged;
    }
}

void vr_aim_sample(VkCommandBuffer commands, int eye, const PcVrRenderCertificate *eye_certificate,
                   const TexBackend *const scene_depths[2]) {
    vr_aim_read();
    if (eye < 0 || vr_aim.state < 0) return;
    const TexBackend *scene_depth = scene_depths[eye];
    if (!scene_depth || !scene_depth->image || scene_depth->samples != 1 ||
        (scene_depth->format != VK_FORMAT_D32_SFLOAT && scene_depth->format != VK_FORMAT_D32_SFLOAT_S8_UINT)) return;
    if (!vr_aim.state) {
        VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = VR_AIM_SAMPLINGS * VR_AIM_POINTS * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo memory_info = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        vr_aim.state = vmaCreateBuffer(g_vk.vma, &buffer_info, &memory_info, &vr_aim.buffer,
                                                &vr_aim.memory, &info) == VK_SUCCESS ? 1 : -1;
        if (vr_aim.state < 0) return;
        vr_aim.readback = info.pMappedData;
    }
    static bool told;
    if (!told) {
        told = true;
        LOG("[pc-vr-world-hud] aim depth from eye %d; the eyes' scene depth targets are %s\n", eye,
            scene_depths[0] && scene_depths[1] && scene_depths[0] != scene_depths[1] ?
            "separate" : "the same");
    }
    unsigned free_slot = 0;  // a free sampling
    while (free_slot < VR_AIM_SAMPLINGS && vr_aim.samples[free_slot].seq) ++free_slot;
    if (free_slot == VR_AIM_SAMPLINGS) return;  // (all still in flight)
    const PcVrRenderCertificate *cert = eye_certificate;
    const XrPoseSnapshot *pose = &cert->native_snapshot;
    const XrEyeSnapshot *eye_pose = &pose->eyes[eye];
    const double units_per_meter = cert->units_per_meter;
    if (!pose->calibration.epoch || !(units_per_meter > 0)) return;
    // The ray: from the calibration origin along the calibrated heading (LOCAL space, metres).
    const double heading[4] = {pose->calibration.heading_xyzw[0], pose->calibration.heading_xyzw[1],
                               pose->calibration.heading_xyzw[2], pose->calibration.heading_xyzw[3]};
    const double ahead[3] = {0, 0, -1};
    double direction[3];
    vr_rotate(heading, ahead, direction);
    // The eye's FOV as tangents: left, right, up, down.
    const double left = tan(eye_pose->fov_left_right_up_down[0]), right = tan(eye_pose->fov_left_right_up_down[1]);
    const double up = tan(eye_pose->fov_left_right_up_down[2]), down = tan(eye_pose->fov_left_right_up_down[3]);
    const double fov_tangents[4] = {left, right, up, down};
    VkBufferImageCopy copies[VR_AIM_POINTS];
    unsigned steps = 0;
    // The points' distances along the ray (game units), sorted: the read takes the first point behind the scene.
    double distances[VR_AIM_POINTS];
    unsigned distance_count = 0;
    for (unsigned i = 0; i < VR_AIM_COARSE_POINTS; ++i)
        distances[distance_count++] = 0.25 * pow(16000.0, (double)i / (VR_AIM_COARSE_POINTS - 1));
    for (unsigned i = 0; vr_aim_result.distance > 0 && i < VR_AIM_FINE_POINTS; ++i)
        distances[distance_count++] = vr_aim_result.distance * (0.85 + 0.3 * i / (VR_AIM_FINE_POINTS - 1));
    for (unsigned i = 1; i < distance_count; ++i) {  // (insertion sort)
        for (unsigned j = i; j > 0 && distances[j - 1] > distances[j]; --j) {
            const double swap = distances[j];
            distances[j] = distances[j - 1];
            distances[j - 1] = swap;
        }
    }
    for (unsigned i = 0; i < distance_count; ++i) {
        const double along = distances[i];
        // The point relative to the eye (LOCAL), in the eye's view, and where in its image.
        double from_eye[3], view[3], image[2];
        vr_aim_ray_point_from_eye(pose->calibration.origin_xyz, direction, along, units_per_meter,
                                  eye_pose->position_xyz, from_eye);
        if (!vr_eye_image_position(from_eye, eye_pose->orientation_xyzw, fov_tangents, 1e-6, view, image))
            continue;  // (behind the eye)
        // The pixel where the eye sees it.
        const double pixel_x = image[0] * scene_depth->width;
        const double pixel_y = image[1] * scene_depth->height;
        if (!(pixel_x >= 0 && pixel_x < scene_depth->width && pixel_y >= 0 && pixel_y < scene_depth->height)) continue;
        copies[steps] =
            (VkBufferImageCopy){.bufferOffset = ((VkDeviceSize)free_slot * VR_AIM_POINTS + steps) * 4,
                                .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                                .imageOffset = {(int32_t)pixel_x, (int32_t)pixel_y, 0},
                                .imageExtent = {1, 1, 1}};
        if (!steps) memcpy(vr_aim.samples[free_slot].depth_terms, cert->depth_terms, sizeof cert->depth_terms);
        vr_aim.samples[free_slot].ray_distance[steps] = along;
        // (the point's depth in the eye's view)
        vr_aim.samples[free_slot].eye_depth[steps] = -view[2] * units_per_meter;
        ++steps;
    }
    vr_aim.samples[free_slot].units_per_meter = units_per_meter;
    vr_aim.samples[free_slot].gl_depth_range = nvn_depth_mode() == 0;
    if (!steps) {  // (the whole ray is out of this eye's view: nothing to meet)
        vr_aim_result.distance = VR_AIM_MAX_M * units_per_meter;
        vr_aim_result.frame = renderer_present_count();
        return;
    }
    rec_CmdCopyImageToBuffer(commands, scene_depth->image, VK_IMAGE_LAYOUT_GENERAL, vr_aim.buffer, steps,
                             copies);
    vr_aim.samples[free_slot].steps = steps;
    vr_aim.samples[free_slot].generation = vr_aim.generation;
    vr_aim.samples[free_slot].seq = vk_pending_seq();
}

void vr_aim_game_reset(void) {
    // Samplings still marked in flight keep their slots until vr_aim_read sees them complete (the GPU is idle here,
    // so at the next lens pass), and are discarded then.
    ++vr_aim.generation;
    vr_aim_result = (VrAimResult){0};
}
