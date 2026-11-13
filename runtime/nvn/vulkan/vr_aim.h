// PC VR renderer: the aim ray's distance, from the scene's depth (D073), runtime/nvn/vulkan/vr_aim.c.
//
// What it is for: where the game camera's aim ray meets the scene, so that the crosshair is drawn at that depth in
// each eye (exec.c vr_world_hud_draw, a reticle's pane) and the rune target test picks what the crosshair is on
// (exec.c vr_rune_aim_patch, D078).
//
// The ray: from the D053 calibration origin along the calibrated heading's forward, level (the bow's camera keeps its
// projection centred while it zooms). It is the camera's forward, not an eye's own: each eye sits half the eye
// distance to the side, and over Link's shoulder its own forward can graze him.
//
// When it is sampled and read: at the frame's lens pass (exec.c vr_capture_eyes calls vr_aim_sample) the scene depth
// target (VrSceneDepth, vr_scene_depth.h) of the last eye drawn is copied, only while that eye is certified this frame
// (there is no fallback to the other eye: an uncertified last eye means no sample that frame), at points along the
// ray: 40 spaced
// geometrically from 0.25 to 4000 game units, plus 16 within 15% of the last hit, each read at the pixel where that eye
// sees it, into one of 4 samplings of a host-visible buffer. That copy is the frame's depth after its scene was drawn.
// At each later lens pass, before sampling again, the samplings the GPU has finished (a frame or two later) are read:
// the first point that lies behind the scene, interpolated from the one before it, is where the ray meets it. So the
// distance arrives one to a few frames after the depth it comes from. A ray that meets nothing (the sky) gives the
// farthest depth, VR_AIM_MAX_M at the eye's world scale.
//
// Units and spaces: the ray's points and the result are game units along the ray from the calibration origin; the
// eye's world scale (its certificate's units_per_meter) turns them into LOCAL metres. Pixels are the depth image's own.
//
// Depth convention: the stored D32 value is turned back into a view-space z through the eye projection's depth terms
// from its certificate, device depth = (a z + b) / (c z + e); in the game's OpenGL depth mode (nvn_depth_mode 0) the
// stored 0..1 is first mapped to -1..1. A stored 0 or 1, or a view z of 0 or more (not ahead of the eye), counts as no
// scene there.
//
// Buffer: the module owns the readback buffer (VR_AIM_SAMPLINGS x VR_AIM_POINTS floats, host-visible, mapped),
// allocated at the first sample and kept for the process. Each sampling is in flight from its copy until the GPU passes
// its submission; it is read and freed then.
//
// Serialisation: vr_aim_sample (and the read inside it) runs at the lens pass inside backend_execute's draws, on the
// thread translating the game's commands (a game thread or the asynchronous queue's), serialised by gpu_lock (game
// threads and the window loop take it), so the state needs no lock of its own. Precondition: no render pass open (the
// copies go into the command buffer passed in). vr_aim_game_reset runs without the lock, on the main thread, from
// exec.c vk_exec_reset in device.c backend_reset (runtime/main.c teardown): the guest threads have stopped
// (kernel_stop_threads), the queue has drained (nvn_queue_idle), the recorder has drained and the device is idle, so
// no sample or read can run meanwhile; those waits order the last sample before the reset, and the next game's
// threads, created after it, order the reset before the next sample.
//
// Game reset: each sampling carries the reset generation it was taken in. vr_aim_game_reset starts a new one and
// clears vr_aim_result; a sampling of an older generation is freed when it completes, without setting the distance.
// The buffer and the log count are kept for the process.
#ifndef SAVAGEWIND_VR_AIM_H
#define SAVAGEWIND_VR_AIM_H

#include <stdint.h>

#include "vk.h"
#include "../../hle/pc_vr_render_trace.h"

#define VR_AIM_MAX_M 30.0  // the crosshair's farthest depth, metres in the headset

// The aim's result, written by this module and read by exec.c: by the world-tied HUD's reticle on the draw path and by
// the rune aim on the constant buffer path. Defined in exec.c, the translation unit of those readers, so that their
// reads stay direct (vr_eyes.h says why); this module's per-frame writes take the indirection.
typedef struct {
    double distance;  // game units ahead of the game camera along its forward where the ray meets the scene (0: none yet)
    uint64_t frame;   // the present count when distance was last set (the reticle uses a distance at most 8 old)
    uint64_t misses;  // the readings whose ray met nothing (for the log)
} VrAimResult;

extern VrAimResult vr_aim_result;  // (defined in exec.c: see above)

// At the frame's lens pass, no render pass open: reads the samplings the GPU has finished (setting vr_aim_result), then
// copies eye's scene depth target (scene_depths[eye]) at the aim ray's points, where that eye sees them, into a free
// sampling, recorded into commands. eye: the last eye drawn this frame (VrSceneDepth.last_eye) when it is certified
// this frame, with its certificate (eye_certificate: its pose snapshot, depth terms and world scale); otherwise -1
// (nothing is sampled; the other eye is not tried). scene_depths: both eyes' scene depth
// targets this frame (NULL: none), for the depth image and a one-time log of whether the eyes' targets are separate.
void vr_aim_sample(VkCommandBuffer commands, int eye, const PcVrRenderCertificate *eye_certificate,
                   const TexBackend *const scene_depths[2]);

// At the in-place reboot (exec.c vk_exec_reset; main thread, guest threads stopped, GPU idle): a new reset
// generation, so the samplings taken before it are discarded when they complete, and vr_aim_result cleared.
void vr_aim_game_reset(void);

#endif
