// PC VR renderer: which depth target holds an eye's scene (D073, D092).
//
// Responsible for: the frame's scene depth state (VrSceneDepth), which the crosshair's depth sample reads (vr_aim.c):
// each eye's scene depth target and the eye drawn last. An eye's scene depth is the depth target the game's own
// finishing draw of the eye layer (the eye's scene-final draw, known by the game's tag on it) has attached. Aiming adds
// a pass for Link's outline with a depth target of its own; that pass is not the eye layer's finishing draw.
//
// Not owned here:
//  - which draw is an eye's scene-final draw, and for which eye: exec.c's vr_after_draw sets the eye's depth target
//    at that draw when it carries the eye's render certificate, and logs a certified one with no depth target;
//  - when the state is cleared: exec.c clears it (vr_scene_depth_reset) at each present (vk_vr_present) and at the
//    game reset (vk_exec_reset);
//  - the depth targets themselves: renderer images (TexBackend, vk.h, owned by textures.c).
//
// Conventions:
//  - A target is known by its TexBackend's address, kept only within a frame and never read here.
//  - exec.c keeps one VrSceneDepth, vr_scene_depth, written on the per-draw path.
//  - Eyes are numbered as the render certificates' native_index: 0 the left eye, 1 the right.
#ifndef SAVAGEWIND_VR_SCENE_DEPTH_H
#define SAVAGEWIND_VR_SCENE_DEPTH_H

#include <stddef.h>

// The renderer's record of a GPU image (vk.h). Only its address is kept here, so the state needs no Vulkan headers.
typedef struct TexBackend TexBackend;

// The frame's scene depth state (D073).
typedef struct {
    // Each eye's scene depth target, set at its certified scene-final draw (NULL: none this frame, or the draw had no
    // depth target attached).
    const TexBackend *depth[2];
    // The eye whose scene-final draw came last this frame (-1: none yet). The crosshair's depth is sampled from this eye
    // only, and only while it is certified this frame; there is no fallback to the other eye.
    int last_eye;
} VrSceneDepth;

// Back to the state of a frame before any draw: no eye's target, no last eye. At each present and at the game reset
// (the state starts so: last_eye -1).
static inline void vr_scene_depth_reset(VrSceneDepth *scene_depth) {
    scene_depth->depth[0] = scene_depth->depth[1] = NULL;
    scene_depth->last_eye = -1;
}

#endif
