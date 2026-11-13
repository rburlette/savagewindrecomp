// PC VR renderer: diagnostic image shots (D048 onward), runtime/nvn/vulkan/vr_shots.c.
//
// A shot saves one GPU image of the PC VR path as a PNG under build/ (in the data folder), to see without a headset
// what the eye images, the HUD's own image and the panels hold at a chosen moment. Diagnostics only: nothing here is
// part of what the headset shows, and with neither setting below a call costs a few string compares and a counter.
//
// When shots are taken:
//  - SAVAGEWIND_PC_VR_SHOT=<present>[,<present>...] (at most 8 presents): at the first frame at or after
//    each listed present, each kind of image is saved once, as build/vr-shot-<kind>-<present>.png. The kinds, named by
//    the call sites in exec.c: submit0/1 (the slot's eye images, as the XR owner copies them), combined (the final
//    image), hud and hud-alpha (the slot's HUD image, and its coverage as grey), hud-full (the HUD's whole image),
//    hdr-eye0/1 and eye0/1 (a certified eye's HDR scene and finished image, at its scene-final draw), hdr-scene0/1 and
//    scene0/1 (the same without certificates, the eyes named only by the order of their images).
//  - SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS=1: what the headset shows while a quick menu or a shop holds the scene (D072),
//    for the first 4 such menus of the run, at the 30th present over the held scene in a row (vr_quick_menu_shot).
// "Present" is the renderer's present count plus one, the frame being recorded (renderer_present_count, device.c).
//
// Cost: each saved image is copied, the frame's commands so far are submitted, and the render thread waits for the GPU
// (a GPU that does not finish within 10 seconds stops the process) and then encodes and writes the file. A shot frame
// stalls; frames without one are not touched.
//
// Threading and preconditions: every call is serialised by gpu_lock (from backend_execute's draws and the lens pass,
// and from backend_present, which game threads and the window loop both call), so the module's state needs no lock of
// its own. No render pass may be open
// when a shot can save: exec.c ends its pass first (vr_shot_after_draw after a draw; vr_capture_eyes has ended it before
// its copies). The images are in VK_IMAGE_LAYOUT_GENERAL, as every image of this backend is, and stay alive through
// the call, which waits for the GPU before it returns.
//
// State and reset: the module keeps its state in vr_shots.c and has no reset owner. It lives for the process, which is
// right for all of it: the schedule comes from the configuration (read once, pc_vr_config.h) and names presents of
// the renderer's count, which runs on through a game reset (device.c never restarts it), so a requested present means the same moment across a
// reboot; the quick menu count caps the shots of a whole run; the held-scene count starts again at the first game
// present without a quick menu, and the eye naming of the scene shots at the next present.
#ifndef SAVAGEWIND_VR_SHOTS_H
#define SAVAGEWIND_VR_SHOTS_H

#include <stdbool.h>
#include <stdint.h>

#include "vk.h"

// Whether vr_shot can save this image: a single-sample 2D image of at most 4096x4096 in A2B10G10R10, B10G11R11, sRGB
// RGBA8 or D32 depth. exec.c asks before ending its render pass for a shot, so that a pass ends exactly when an image
// will be saved.
bool vr_shot_image_savable(const TexBackend *src);

// Whether vr_shot(name) would save this frame: a requested present has been reached and this kind is not saved for it
// yet. For call sites that must prepare something first (a copy, a log line, the end of a render pass).
bool vr_shot_due(const char *name);

// Saves image as the shot `name` for each requested present it is due for. HDR inputs use labo_premask_rgb8's finite
// x/(1+x) preview mapping, LDR and composed images the headset's sRGB encoding: compare what HDR and LDR images cover,
// not their brightness. alpha_as_grey: save an RGBA8 image's alpha (the HUD's coverage) as grey instead of its colour.
void vr_shot(TexBackend *image, const char *name, bool alpha_as_grey);

// Which side (0 or 1) the finished image output_serial is in this frame's uncertified scene shots, the eyes being known
// only by the order of their images; -1 when both sides already have other images. Called at each scene-final draw
// without a certificate, which then shoots hdr-scene<side> and scene<side>.
int vr_shot_scene_side(uint64_t output_serial);

// The shots of the slot's images at the lens pass, after vr_capture_eyes filled them (slot_images: the left eye, the
// right eye, the left half of the HUD's image; each half_width x height, sRGB RGBA8): submit0 and submit1, ...
void vr_shot_slot_eyes(const VkImage slot_images[3], uint32_t half_width, uint32_t height);
// ... and, when the slot holds this frame's HUD, hud and hud-alpha.
void vr_shot_slot_hud(const VkImage slot_images[3], uint32_t half_width, uint32_t height);
// hud-full: the HUD's whole image (hud_image, the final image's size: combined), both halves, unflipped like the
// combined shot, through a copy made for it. Call only when vr_shot_due("hud-full") and the HUD was drawn this frame.
void vr_shot_hud_full(VkImage hud_image, const TexBackend *combined);

// At each game present, whether the frame was a quick menu or a shop over the held scene: counts the presents over the
// held scene in a row, and arms the next run's shot when a run ends.
void vr_quick_menu_shots_present(bool over_world);
// At the lens pass, after the slot's images were filled: saves the quick menu shots (final0, final1, panel,
// panel-alpha) when they are due (SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS). over_world and slot_has_hud: this frame's;
// world_left_out: this frame's world-tied HUD draws left out over the held scene, for the log line.
void vr_quick_menu_shot(const VkImage slot_images[3], bool over_world, bool slot_has_hud, uint32_t half_width,
                        uint32_t height, unsigned world_left_out);

#endif
