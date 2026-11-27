// PC VR renderer: NVIDIA DLAA or DLSS on each eye image (vr_antialiasing, vr_dlss_ratio and
// vr_dlss_eye_scale in savagewind.ini, pc_vr_config.h; DLSS is the play default since D132).
//
// Responsible for: each eye's temporal anti-aliasing, with an NGX feature and a history per eye (dlss.h
// VK_DLSS_SLOT_LEFT_EYE, _RIGHT_EYE), at the eye's placement into the final image (exec.c scene_post, after the eye's
// scene is finished and graded, before its HUD is drawn): DLSS from the eye image to its place's size in the final
// image. At the same size that is DLAA; with the ratio the eye targets render that many times smaller per axis
// (textures.c takes vr_dlaa_render_ratio) and DLSS brings each eye up to the size the headset is given, the game's own
// textures in the eye's draws biased to keep that size's detail (exec.c takes vr_dlaa_mip_bias). (DLAA on the eye's HDR
// scene before its effects was tried and dropped on 2026-11-25: the effects drawn after it are not anti-aliased, and
// distant ones and the fog cover much of the picture; tag archive/pc-vr-2026-11-25 keeps it.)
// Its inputs:
//  - the eye's own camera, from the placement's render certificate (the eye layer's view and device projection as the
//    game built them, pc_vr_render_trace.h), now and at the eye's previous frame, for the camera's motion vectors
//    (exec.c cam_motion, through vk_camera_motion_vectors; Link's box moves as he does, as upstream's DLSS has it, and
//    anything else that moves has the vectors of a still object there);
//  - in their place when this eye's scene drew them this frame, the eye scene's per-object motion vectors
//    (vr_exec_motion.inc, upstream's mvec.c): a draw that moved carries its own, the camera's motion fills the rest
//    by the eye's reprojection (vr_math.h vr_eye_reprojection, from the certificates' views and projections);
//  - the eye's scene depth (vr_scene_depth.h): the one depth target the eyes share, which still holds this eye's scene
//    at its placement (the other eye's scene comes after);
//  - the sub-pixel offset the projection trace moved the eye's projection by (the certificate's jitter_ndc). This
//    module asks for that offset (pc_vr_render_trace_jitter, over 8 times the upscale's area) while DLAA runs, and
//    takes it away when DLSS cannot run here.
// The history starts over at a cut (vr_math.h vr_dlaa_cut), when the eye's previous frame was not the one before, and
// at the game's reset.
//
// Not owned here: the offset's sequence and its place in the projection (runtime/hle/pc_vr_projection_trace.c), NGX and
// its features (dlss.c), the camera vectors' pass (exec.c), the eyes' object vectors: their target's passes, the draws'
// keys and their fill pass (vr_exec_motion.inc, with upstream's mvec.c), the game's FXAA (exec.c turns it off with
// this), which draw is an eye's placement (exec.c pc_vr_scene_placement, by the game's tag on it).
//
// Conventions: eyes as the certificates' native_index (0 left, 1 right); images are the renderer's (TexBackend, vk.h);
// called on the thread that replays the game's commands, with the recorder's ordering (vk.h).
#ifndef SAVAGEWIND_VR_DLAA_H
#define SAVAGEWIND_VR_DLAA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vk.h"
#include "../../hle/pc_vr_render_trace.h"

// The modes, cycled in this order by the '9' key (runtime/hle/input.c) while the switch is on and DLSS runs here (an
// NVIDIA RTX GPU with NGX, and NVIDIA's library next to savagewind.exe): none (the eye images as the game makes them,
// its FXAA on), DLAA at the configured eye scale, DLSS from vr_dlss_ratio times smaller (1.5) up to
// vr_dlss_eye_scale (3.0). The run starts in the mode vr_antialiasing names (dlss, or dlaa). A change takes
// effect at the next present: the eye size (pc_vr_eye_scale_override) and the eye targets' scale change with it, the
// renderer makes its scaled targets and the XR owner its swapchains again at the new sizes, and the histories start
// over.
typedef enum { VR_DLAA_MODE_NONE, VR_DLAA_MODE_DLAA, VR_DLAA_MODE_DLSS, VR_DLAA_MODES } VrDlaaMode;

// A mode other than none is on (DLSS on the eye images, the game's FXAA off): exec.c scene_post calls vr_dlaa_eye.
bool vr_dlaa_enabled(void);

// The '9' key (any thread): the next mode, from the next present. Logged once when the switch is off or DLSS does not
// run here.
void vr_dlaa_next_mode(void);

// At each present (exec.c vk_vr_present): the first decides the mode to start in, later ones apply a requested mode.
void vr_dlaa_present(void);

// The eye targets' render size is the eye size divided by this per axis: the DLSS mode's ratio, else 1.
double vr_dlaa_render_ratio(void);

// The LOD bias for the game's own textures in the eye's draws (log2 of render over output size; 0 without DLSS's
// upscale). Read per texture: a stored value.
float vr_dlaa_mip_bias(void);

// At an eye's placement: DLSS on color (the eye's finished image, graded) with that eye's scene depth (depth, at
// color's size) and the placement's certificate, to output_width x output_height (the eye's place in the final image;
// within a pixel of color's size, DLAA at that size). Returns the anti-aliased eye image (an image of this module's,
// RGBA16F at the output size, valid until the next eye's call) for the placement to draw, or NULL: not run this frame
// (the eye image as it is), why counted and logged now and then.
// object_motion: the eyes' motion target when this eye's scene drew it this frame (vr_exec_motion.inc), else NULL; with
// it and the eye's previous frame, the object vectors replace the camera's.
TexBackend *vr_dlaa_eye(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, TexBackend *object_motion,
                        const PcVrRenderCertificate *certificate, uint32_t output_width, uint32_t output_height);

// Per-object motion vectors for the eyes' scenes are wanted (any thread): a mode other than none, as last applied at a
// present. Upstream's motion vectors (mvec.c mvec_enabled) switch on with it.
bool vr_dlaa_object_vectors(void);

// The eyes' scenes' motion target at this size (RGBA16F, this module's image, made again when the size changes),
// which upstream's motion programs draw into as the flat scene's (exec.c begin_pass): each eye's scene in turn, read
// at that eye's placement before the other eye's scene clears it.
TexBackend *vr_dlaa_motion_target(uint32_t width, uint32_t height);

// The game's reset: every eye's history starts over.
void vr_dlaa_reset(void);

#endif
