// Part of exec.c (D133): PC VR's rune targets at the aim ray's hit (D078) and the field haze's view.
// Included where it stands by exec.c, so it compiles in exec.c's translation unit as before.

// ------------------------------------------------------------------------------------ PC VR: rune targets (D078)
// Rune targets (magnesis, stasis) are picked on the GPU. One draw renders one pixel (a 4x4 target, a 1x1 viewport)
// from an eye's scene, at the screen position in the first two floats of a 32-byte fragment constant buffer (in Labo
// mode 0.5625, 0.5: about the image's centre; BetterVR moves the same position). Each eye's image follows the head,
// so the target did too, away from the crosshair, which is on the game camera's aim ray (D073). So the position is
// replaced by where that ray meets the scene (vr_aim.c's vr_aim_result) in the image of the eye whose scene the draw
// samples: in play, the frame's first eye (0), drawn just before the test into the scene targets both eyes share.
// The test's draw is known by the game call that issues it (PC_VR_LAYER_DRAW_RUNE_TEST, tools/game_sites.py
// pcvr_rune_test_draw), and the block with the position by the game's own setup of the test, which reports the block
// it fills (runtime/pc_vr_hud.c rt_rune_test).
// SAVAGEWIND_PC_VR_RUNE_AIM=0: off; =flip: the position's y counted from the bottom.

// For each fragment constant buffer about to be bound (cbuf_info, per draw): when it is the rune target test's, sets
// *out to a copy with the position moved onto the aim ray, and returns true. False: bind the game's buffer as it is.
static bool vr_rune_aim_patch(Range buffer, VkDescriptorBufferInfo *out) {
    // (a draw of the test's, and its constants the block the game's own setup of the test reported, two vectors of
    // four floats; the draw's mode first, as this runs for every fragment constant buffer)
    if (vr_draw_rune_aim == PC_VR_RUNE_AIM_OFF || buffer.size < 32 || !pc_vr_rune_test_constants(buffer.addr) || !st.tpool)
        return false;
    const float *constants = (const float *)(uintptr_t)buffer.addr;
    // The test's draw is recorded inside an eye's layer and carries that eye's certificate: which eye's scene it
    // samples and the pose that scene was drawn with, this frame's. Without one the test stays as the game set it.
    PcVrRenderCertificate own;
    if (!vr_draw_certificate || !pc_vr_render_trace_resolve(vr_draw_certificate, &own) || own.native_index >= 2) {
        static uint64_t uncertified;
        if (uncertified++ % 300 == 0)
            LOG("[pc-vr-rune-aim] test at %.4f %.4f left as it is: its draw carries no eye's certificate (%llu times) "
                "present %llu\n", constants[0], constants[1], (unsigned long long)uncertified,
                (unsigned long long)renderer_present_count());
        return false;
    }
    const int eye = (int)own.native_index;
    const PcVrRenderCertificate *cert = &own;
    const XrPoseSnapshot *pose = &cert->native_snapshot;
    const XrEyeSnapshot *eye_pose = &pose->eyes[eye];
    const double units_per_meter = cert->units_per_meter;
    if (!pose->calibration.epoch || !(units_per_meter > 0)) return false;
    // The game camera's forward in the calibrated space, from its origin, to the aim depth (D073; the farthest
    // crosshair depth until the first read).
    const double aim =
        vr_aim_result.distance > 0 ? vr_aim_result.distance : VR_AIM_MAX_M * units_per_meter;
    const double heading[4] = {pose->calibration.heading_xyzw[0], pose->calibration.heading_xyzw[1],
                               pose->calibration.heading_xyzw[2], pose->calibration.heading_xyzw[3]};
    const double ahead[3] = {0, 0, -1};
    double direction[3], from_eye[3], view[3], image[2];
    vr_rotate(heading, ahead, direction);
    vr_aim_ray_point_from_eye(pose->calibration.origin_xyz, direction, aim, units_per_meter, eye_pose->position_xyz,
                              from_eye);
    // Where the eye sees the aim point, 0 to 1 across its image (vr_math.h).
    const double left = tan(eye_pose->fov_left_right_up_down[0]), right = tan(eye_pose->fov_left_right_up_down[1]);
    const double up = tan(eye_pose->fov_left_right_up_down[2]), down = tan(eye_pose->fov_left_right_up_down[3]);
    if (!vr_eye_image_position(from_eye, eye_pose->orientation_xyzw, (const double[4]){left, right, up, down}, 1e-6,
                               view, image))
        return false;  // (behind this eye: the game's own position)
    const double image_x = image[0], image_y = image[1];
    if (!(image_x >= 0 && image_x <= 1 && image_y >= 0 && image_y <= 1)) {  // (off this eye's image)
        static unsigned off_image;
        if (off_image++ % 300 == 0)
            LOG("[pc-vr-rune-aim] aim point off eye %d's image (%.3f %.3f): test left at %.4f %.4f (%u times)\n", eye, image_x,
                image_y, constants[0], constants[1], off_image);
        return false;
    }
    float patched[8];
    memcpy(patched, constants, sizeof patched);
    patched[0] = (float)image_x;
    patched[1] = (float)(vr_draw_rune_aim == PC_VR_RUNE_AIM_FLIP ? 1 - image_y : image_y);
    static uint64_t logged;
    if (logged < 12 || !(logged % 600)) {
        LOG("[pc-vr-rune-aim] eye %d (its own draw's certificate) test %.4f %.4f -> %.4f %.4f (aim %.1f game units) "
            "constants at %llx present %llu (%llu moved)\n", eye, constants[0], constants[1], patched[0], patched[1],
            aim, (unsigned long long)buffer.addr, (unsigned long long)renderer_present_count(),
            (unsigned long long)logged + 1);
    }
    ++logged;
    *out = ubo_span(vk_transient_copy(patched, sizeof patched, 256), sizeof patched);
    return true;
}

#include "vr_field_cloud_view.h"
