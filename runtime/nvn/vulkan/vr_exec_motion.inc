// Part of exec.c (D133): PC VR's per-object motion vectors for the eyes' scenes.
// Included where it stands by exec.c, before begin_pass and draw_, which call it, so it compiles in exec.c's
// translation unit.

// Upstream makes per-object motion vectors for the flat scene (mvec.c): it appends its motion target to the passes
// drawing into the flat scene's depth (begin_pass), records each of their draws and matches it to the same draw of the
// previous frame, and a draw that moved runs its vertex shader on the previous frame's inputs too, writing its own
// vector. Here the passes drawing into the eyes' scene depth (the target the game's scene-final draws of the eye layers
// attach, D073; both eyes share it) get the eyes' motion target instead (vr_dlaa.h vr_dlaa_motion_target), cleared at
// each eye scene's first such pass: the eyes draw their scenes one after the other, and each eye's placement, where
// DLSS reads the target, comes before the next eye's scene. The draws since the frame's last eye scene-final draw are
// the next eye's: in the matching each eye's draws are told apart by that segment (vr_eye_motion_key), so that a draw
// is matched to the same eye's draw of the previous frame, whose copied camera block is that eye's previous camera.
#include "vr_dlaa.h"

static struct {
    const TexBackend *eye_depth;  // the eyes' scene depth target, as the last eye scene-final draw attached it
    uint64_t segment_frame;       // renderer_present_count() of segment
    unsigned segment;             // eye scene-final draws seen this frame: the draws now are that eye scene's (0, 1)
    uint64_t cleared_frame;       // the frame and segment the motion target was last cleared for
    unsigned cleared_segment;
    uint64_t written_frame[2];    // the frame whose scene of eye e the motion target holds (UINT64_MAX: none)
    bool eye_pass;                // the pass begun last draws into the eyes' scene depth with the motion target
    uint64_t order_mismatches;    // eye scenes finished in the other eye's segment (logged): no object vectors
} vr_eye_motion = {.cleared_frame = UINT64_MAX, .written_frame = {UINT64_MAX, UINT64_MAX}};

// The segment of the draws now: how many eye scenes this frame has finished.
static unsigned vr_eye_motion_segment(void) {
    const uint64_t frame = renderer_present_count();
    if (vr_eye_motion.segment_frame != frame) vr_eye_motion.segment_frame = frame, vr_eye_motion.segment = 0;
    return vr_eye_motion.segment;
}

// begin_pass, for a pass with a depth target and the pass's size width x height while upstream's motion vectors are
// on: the eyes' motion target when the pass draws into the whole of the eyes' scene depth, with load the attachment's
// load (a clear at the eye scene's first such pass); NULL for other passes (upstream's mvec_pass_target decides then).
static TexBackend *vr_eye_motion_target(const NvnTexture *depth, uint32_t width, uint32_t height,
                                        VkAttachmentLoadOp *load) {
    vr_eye_motion.eye_pass = false;
    const TexBackend *depth_target = depth ? (const TexBackend *)depth->backend : NULL;
    if (!vr_dlaa_object_vectors() || !depth_target || depth_target != vr_eye_motion.eye_depth ||
        width != depth_target->width || height != depth_target->height)
        return NULL;
    TexBackend *target = vr_dlaa_motion_target(width, height);
    const uint64_t frame = renderer_present_count();
    const unsigned segment = vr_eye_motion_segment();
    const bool cleared = vr_eye_motion.cleared_frame == frame && vr_eye_motion.cleared_segment == segment;
    *load = cleared ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    vr_eye_motion.cleared_frame = frame;
    vr_eye_motion.cleared_segment = segment;
    vr_eye_motion.eye_pass = true;
    return target;
}

// draw_, for a draw upstream's motion vectors record (program_key: its shaders' identity, mvec.h MvecHead.pkey): in a
// pass drawing an eye's scene, the eye scene's segment mixed in, so that it is matched only to the same eye's draws.
static inline void vr_eye_motion_key(uint64_t *program_key) {
    if (vr_eye_motion.eye_pass) *program_key = mvec_mix(*program_key, 0x657965u + vr_eye_motion_segment());
}

// At an eye's scene-final draw (vr_exec_hub.inc vr_after_draw; eye: its certificate's, -1 without one; depth: the
// depth target it attached): the eye scene's segment ends, and the motion target holds that eye's scene when it was
// cleared for this segment and the segment is the eye's.
static void vr_eye_motion_scene_finished(int eye, const TexBackend *depth) {
    const uint64_t frame = renderer_present_count();
    const unsigned segment = vr_eye_motion_segment();
    if (depth) vr_eye_motion.eye_depth = depth;
    if (eye >= 0 && vr_eye_motion.cleared_frame == frame && vr_eye_motion.cleared_segment == segment) {
        if ((unsigned)eye == segment) vr_eye_motion.written_frame[eye] = frame;
        else if (vr_eye_motion.order_mismatches++ < 8)
            LOG("[pc-vr-dlaa] eye %d's scene finished as the frame's eye scene %u: no object vectors for it "
                "present=%llu (line %llu of 8)\n", eye, segment, (unsigned long long)frame,
                (unsigned long long)vr_eye_motion.order_mismatches);
    }
    vr_eye_motion.segment = segment + 1;
}

// At an eye's placement: the motion target when it still holds this frame's scene of that eye, else NULL.
static TexBackend *vr_eye_motion_image(unsigned eye) {
    const uint64_t frame = renderer_present_count();
    if (eye >= 2 || vr_eye_motion.written_frame[eye] != frame ||
        (vr_eye_motion.cleared_frame == frame && vr_eye_motion.cleared_segment != eye))
        return NULL;
    return vr_dlaa_motion_target(vr_eye_motion.eye_depth->width, vr_eye_motion.eye_depth->height);
}

// (vr_dlaa.c) An eye's vectors: the motion target's where a draw that moved wrote one, the camera's motion elsewhere
// by the reprojection, the offsets' difference taken out (upstream's mvec_fill.frag, with the constants mvec_scene_view
// gives it for the flat scene).
static void post_pass_pc(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst,
                         const float *pc, uint32_t pc_bytes, bool clear);
void vk_object_motion_vectors(VkCommandBuffer cb, TexBackend *motion, TexBackend *depth, const float reprojection[16],
                              const float jitter[2], TexBackend *vectors) {
    static const VkComponentMapping identity = {0};
    VkImageView depth_view = vk_image_view(depth, VK_IMAGE_VIEW_TYPE_2D, depth->format, 0, 1, 0, 1, identity,
                                           VK_IMAGE_ASPECT_DEPTH_BIT);
    float constants[24] = {0};
    memcpy(constants, reprojection, 16 * sizeof constants[0]);
    constants[16] = nvn_window_origin() == 0 ? -1.0f : 1.0f;
    constants[17] = nvn_depth_mode() == 0 ? 1.0f : 0.0f;
    constants[18] = (float)mvec_camera_only();
    constants[20] = jitter[0], constants[21] = jitter[1];
    VkImageView inputs[2] = {post_view(motion), depth_view};
    gp_mark(cb, "PC VR: an eye's object motion vectors");
    post_pass_pc(cb, "mvec_fill.frag", inputs, 2, vectors, constants, sizeof constants, false);
}
