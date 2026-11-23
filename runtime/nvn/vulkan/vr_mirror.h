// PC VR renderer: the desktop window's picture (D085), runtime/nvn/vulkan/vr_mirror.c.
//
// What the window shows: the left eye as the headset gets it, the final image's left half (the scene with the
// crosshair and the world-tied markers drawn in; the HUD and menus are panels in the headset only), scaled to fit the
// window's presented image and centred in it with black bars (vr_letterbox, vr_math.h). Why: with the PC VR eye
// images on, the game's own Labo lens pass would show both halves through the lens distortion, which is no use on a
// monitor; the left eye lets someone beside the player follow the game.
//
// When it runs in the frame: at the game's display draw, the draw of its final image into the image it presents (the
// lens pass), known by the game code that issues it. exec.c's vr_window_mirror, called before every draw is recorded,
// decides inline that a draw is that one (its tag, the render target, the image it samples), and only then calls this
// module, in this order: vr_mirror_takes (does the mirror replace the display draw?), then exec.c's capture of the
// eyes (vr_capture_eyes) if the frame has none yet, vr_mirror_due (the frame's first display draw?), exec.c ends its
// render pass, vr_mirror_copy. What it replaces: a display draw the mirror takes is not recorded (exec.c returns from
// draw_ before recording it), every one of them in the frame (the lens pass can be more than one draw); the copy is
// made once a frame.
//
// The copy: the presented image (RGBA8 UNORM) is written by the game through an sRGB view, so it holds
// sRGB-encoded bytes. The half is blitted into an sRGB image of the fitted size (the blit encodes the final image's
// linear values, as the lens pass did), and those bytes are then copied into the presented image, cleared to black
// first.
//
// Serialisation: the calls run inside backend_execute's draws, serialised by gpu_lock (game threads and the window
// loop take it), so the module's state needs no lock of its own.
//
// Images: the module owns one image, of the fitted size and the presented image's sRGB format, allocated on first use
// and reallocated when either changes (the old one destroyed through vk_defer_image, as submitted work may still use
// it). It reads the final image and writes the presented image, both exec.c's (textures.c's), only in the commands it
// records. Its state lives for the process and has no reset owner: the game reset leaves it as it always has (the
// device and the window live on; the image follows the presented image's size and format; the once-a-frame mark names
// a present of the renderer's count, which runs on through a reboot).
#ifndef SAVAGEWIND_VR_MIRROR_H
#define SAVAGEWIND_VR_MIRROR_H

#include <stdbool.h>

#include "vk.h"
#include "vr_math.h"

// At the display draw, presented being its render target and final_image the image it samples: whether the mirror
// replaces the draw. It does when the presented image's format has an sRGB twin the GPU can blit into, decided at the
// first display draw of the run and logged then. False: the game's lens pass is drawn as before.
bool vr_mirror_takes(const TexBackend *presented, const TexBackend *final_image);

// After vr_mirror_takes returned true (and exec.c's capture): whether this is the frame's first display draw and the
// left half fits the presented image in at least one pixel. Marks the frame as mirrored, and fills *box with where the
// half goes. True: the caller ends its render pass and calls vr_mirror_copy.
bool vr_mirror_due(const TexBackend *presented, const TexBackend *final_image, VrLetterbox *box);
// Whether the window gets a picture at this present (the count plus one of the frame being recorded or presented):
// always, but with SAVAGEWIND_PC_VR_NO_DESKTOP_IMAGE=1 (an experiment), when only the unattended endpoint's present is
// drawn, since the run's end is the window's own shot. For device.c's frame_begin and vr_mirror_due.
bool vr_mirror_window_drawn(uint64_t present);
// For device.c's present in a frame that does not draw to the window (undrawn by the experiment above, minimised or
// covered): sets the final image's, the scene's and the shadow maps' scales as a drawn frame does, in PC VR only.
// Without it a window never drawn leaves the final image at the console's 1920x1080 and the headset's eye images at
// its halves, 960x1080 (every run of the experiment before 2026-11-23 evening).
void vr_mirror_undrawn_frame_scales(uint64_t present);

// Records the copy of the final image's left half into the presented image, as *box places it: the fitted image
// (re)allocated if needed, the presented image cleared to black, the half blitted into the fitted image, that copied
// into the presented image, with full barriers between. Precondition: no render pass open; vr_mirror_due returned true
// for this frame with this box. When a new fitted image cannot be allocated, the copy is skipped (the old image has
// already gone to deferred destruction) and the window keeps the last frame's picture.
void vr_mirror_copy(const TexBackend *presented, const TexBackend *final_image, const VrLetterbox *box);

#endif
