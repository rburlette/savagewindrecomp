// PC VR renderer: the eye-image producer (D048 onward), runtime/nvn/vulkan/vr_eyes.c.
//
// Responsible for: the images the headset is fed. The game composes each eye's scene, then its HUD and menus, into the
// two halves of its final image, undistorted, before its own Labo lens pass draws that image to the window. At the
// lens pass both halves are blitted into images this module owns (sRGB RGBA8: the blit encodes the final image's
// linear values, and flips the rows when the window present does), with the HUD's own image (D057) beside them, and at
// the next game present they are offered to the XR owner thread as an eye pair (labelled DEV_UNPROVEN, D048), as what
// the frame turned out to be (vr_frame.h): a scene, a scene with its HUD panel, a quick menu's HUD over the held scene,
// or a menu panel; or nothing, and the slot is handed back. Also the panels' place and size (D056, D068).
//
// Not owned here: recognising the frame's draws (exec.c's draw hooks set vr_eyes.frame and vr_eyes.final_backend), the
// HUD's own image (exec.c; its handle and drawn mark are passed in), the render passes (exec.c ends its pass before a
// capture), the shots (vr_shots.c), the crosshair's depth sample (exec.c), and the swapchains and layers (the XR owner,
// runtime/xr).
//
// Life cycle of a slot (VrEyeSlot, VR_EYE_SLOTS of them):
//  1. armed: the first capture configures the producer (vr_eyes.state 1), or switches it off for good (-1);
//  2. claimed: at the frame's lens pass vr_eyes_capture_claim takes a slot whose busy flag is clear (compare and swap)
//     and holds it in vr_eyes.filling;
//  3. filled: vr_eyes_capture_fill records the copies into it; vr_eyes_capture_done notes the GPU submission after
//     which they are complete (vr_eyes.ready_seq);
//  4. offered or released: at the next present (vr_eyes_present) the slot goes to the XR owner
//     with the offer, or is released at once (not a game frame, nothing to offer, or the offer refused). The game reset
//     releases a slot still being filled (vr_eyes_game_reset);
//  5. released by the XR owner: once its copy of an accepted pair has completed, the XR owner thread calls the offer's
//     release callback, which clears busy.
// Every slot claimed is offered or released exactly once; busy is never cleared while the XR owner may read the images.
//
// Threading: the capture and the present are serialised by gpu_lock, not bound to one thread: the capture runs inside
// backend_execute's draws, the present inside backend_present, which game threads and the window loop both call. The
// game reset runs without the lock, on the main thread with the guest threads stopped and the device idle
// (backend_reset), so nothing else can be in here. The one field another thread writes is VrEyeSlot.busy: the XR
// owner clears it (release, memory_order_release) when it is done with a slot.
//
// Images: each slot's three images are allocated on first use at the final image's half size, kept for the process,
// and reallocated (the old ones destroyed through vk_defer_image) only when a window resize changes that size while the
// XR owner holds none of them. The XR owner reads them only between an accepted offer and its release call.
//
// The struct is public (extern VrEyes vr_eyes) because exec.c's draw hooks read the producer's state and write the
// frame's findings on the per-draw path, where they must stay plain loads and stores. It is defined in exec.c, the
// translation unit of those hooks, so that their accesses stay direct (with MinGW, clang reaches a global of another
// object through a pointer it loads first); this module's per-frame code takes that indirection instead.
#ifndef SAVAGEWIND_VR_EYES_H
#define SAVAGEWIND_VR_EYES_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "vk.h"
#include "vr_frame.h"

// D048: the game centres HUD and menus in each half of its final image on the Labo lens centre: the placement
// polygon's centre, measured at 0.125 of the half's NDC width toward the nose. D068: 2D content reaches the headset
// only on quads (D056 menus, D057 HUD) cropped from the left half, so it stays where the game draws it.
#define VR_LENS_CENTER_NDC 0.125f

// D056, D057, D068: 2D content on a world-locked quad, a virtual screen. Menus (frames without a scene) and the
// gameplay HUD (its own image) show the left eye's screen, cropped from its half, to both eyes on one quad layer in
// LOCAL space at the D053 calibration: `distance` metres ahead along the calibrated heading (menus: their setting;
// the HUD: pc_vr_hud_distance of its setting), centred on the eye FOV's vertical centre. Leaning toward it brings it
// closer.
//
// One image for both eyes: the game draws its Labo menus and HUD with their own per-eye offsets (the inventory's text
// about 31 eye pixels, the gameplay hearts about 52, its 3D Link and blurred world backdrop by other amounts), which
// on a flat panel fused each layer at a different depth. Cropping also cuts the layers that reach past the screen.
//
// The screen, in guest pixels of a 960x1080 half: centred on the lens centre, reaching the nasal edge (the outer
// edge, 3 pixels further out, is cut to keep the rectangle centred), rows 293.5 to 786.5 (from title, inventory and
// pause captures). The quad is 16:9 and VR_SCREEN_WIDTH metres wide per metre of distance (about 80 degrees):
// the size the former D050 transform gave the screen on the Quest 3's projection layer, where the menu and HUD
// distances were chosen. (exec.c's world-tied HUD sizes its markers from the same screen.)
#define VR_SCREEN_HALF_W (480.0 * (1.0 - VR_LENS_CENTER_NDC))
#define VR_SCREEN_HALF_H 246.5
#define VR_SCREEN_WIDTH 1.666

// One set of owned images. VR_EYE_SLOTS of them: the one being filled, the one XR shows, and two queued behind
// it (D081: with the GPU overlap on, the game can run a frame ahead).
enum { VR_EYE_SLOTS = 4 };
typedef struct {
    VkImage image[3];  // the left eye, the right eye, and the left half of the HUD's own image (D057)
    VmaAllocation memory[3];
    bool hud;          // image[2] holds this frame's HUD (set at each fill)
    atomic_bool busy;  // set while it is being filled or the XR owner holds it (until vr_eye_slot_release)
} VrEyeSlot;

// The producer's state. Each field says who writes it; "survives the game reset" means vr_eyes_game_reset leaves it.
typedef struct {
    // Written by this module. Survive the game reset: the GPU's blit support and the window's row order do not change
    // with the game.
    int state;  // 0 not configured yet, 1 armed, -1 off (read by exec.c's draw hooks)
    bool flip;  // flip rows when copying (the window present's row order)
    // Written by this module at a capture that follows a resize. Survive the game reset with the images they describe.
    uint32_t width, height;     // one half of the final image, in allocated pixels: the size of each owned image
    uint64_t image_generation;  // counts reallocations (a window resize rescales the final image)
    // Written by this module (busy also by the XR owner thread). Survive the game reset: the device and the images live
    // on, and the XR owner may still hold a slot it will release.
    VrEyeSlot slots[VR_EYE_SLOTS];

    // The frame being recorded; handed over and cleared at each present (vr_eyes_present). The slot is the frame's
    // claim on owned images: it is offered or released, never just forgotten, so it is not one of the findings.
    // Written by this module; filling is released and cleared by the game reset, and read by exec.c's draw hooks (a
    // capture happens once per frame).
    VrEyeSlot *filling;   // the slot this frame's images are copied into (NULL until the lens pass)
    uint64_t ready_seq;   // the GPU submission after which the copies into filling are complete
    // Written by exec.c's draw hooks during the frame (vr_frame.h says which hook sets which field); copied and reset
    // by this module at each present and at the game reset.
    VrFrameFindings frame;  // what this frame's draws established (vr_frame.h); reset at each present and game reset

    // Written by this module when an offer is accepted; 0 again at the game reset. Read by exec.c's
    // vr_quick_menu_over_world.
    uint64_t scene_held;  // the pose generation of the last accepted scene pair; 0 after a menu panel pair
    // Written by exec.c's draw hooks (the eye placement, the display draw, the window mirror), read by its
    // vr_is_final_image. Survives the game reset, as it always has: the next placement or lens pass renews it, and until
    // then it is only compared, never dereferenced.
    // The final image: the image the lens pass samples (NULL until the first lens pass). Kept as the backend image,
    // not the texture object: textures made over the same memory share it (textures.c).
    const void *final_backend;
    // Written by this module at a scene present with the HUD (D057); survive the game reset, so the next scene's HUD
    // eases from where it was.
    double hud_distance_m;   // the HUD panel's eased distance, metres (0: never placed); a quick menu's HUD keeps it
    uint64_t hud_placed_ns;  // when it was last eased (SDL_GetTicksNS; 0: never)
    // Written by this module: the kind log (each change of kind, for the first 200). Survive the game reset.
    const char *last_kind;  // the previous offer's kind name (NULL before the first)
    unsigned kind_changes;  // the changes logged so far
    // Counts, for the log; written by this module, for the whole run.
    uint64_t captured, offered, accepted, rejected, busy_skips, uncertified, misplaced_frames, menu_pairs, resized;
    uint64_t over_pairs;
} VrEyes;

extern VrEyes vr_eyes;  // (defined in exec.c: see above)

// ---- At the lens pass (exec.c vr_capture_eyes), which reads the finished final image `combined`, in this order:
// Arms the producer on first use, follows a window resize (reallocating the slots' images only while the XR owner holds
// none of them), and claims a slot the XR owner does not hold, into vr_eyes.filling. Records no GPU command. NULL:
// nothing is captured this frame (producer off, no size, images still held after a resize, or every slot busy).
VrEyeSlot *vr_eyes_capture_claim(const TexBackend *combined);
// Copies both halves of combined into the claimed slot's eye images and, when the HUD's own image (hud_image) was drawn
// this frame at combined's size (hud_drawn: the present count plus one of its last drawing; hud_width, hud_height: its
// size), its left half into the slot's HUD image; sets slot->hud. Precondition: no render pass open. False: the slot's
// images could not be allocated; the producer is off for good and the slot is released.
bool vr_eyes_capture_fill(VrEyeSlot *slot, const TexBackend *combined, VkImage hud_image, uint64_t hud_drawn,
                          uint32_t hud_width, uint32_t hud_height);
// After the hub's other lens pass work (shots, the crosshair's depth): the copies complete at the pending submission.
void vr_eyes_capture_done(void);

// ---- At each present (exec.c vk_vr_present, from backend_present after the present's submission):
// Hands over this frame's slot and findings and clears both for the next frame; then, for a captured game frame,
// classifies it (vr_frame.h) and offers the slot to the XR owner, or releases it: submits the copies first when they are
// not submitted yet (submitted_seq: the executor's last submitted number, read by the caller), queries the pose for a
// frame without a scene, builds the panel the kind needs, eases the HUD distance on a scene with its HUD, counts and
// logs. A UI re-presentation (game_present_id 0) has its slot released unoffered. Returns whether the frame was a
// captured game frame (a slot filled at a game present, offered or released here), the frames the quick menu shots
// count. The caller reads anything it needs of vr_eyes.frame before the call, which resets it.
bool vr_eyes_present(uint64_t game_present_id, uint64_t submitted_seq);

// ---- At the in-place reboot (exec.c vk_exec_reset, GPU idle): releases a slot still being filled and forgets the
// frame in progress and the held scene. Pairs the XR owner accepted come back through its release callback.
void vr_eyes_game_reset(void);

#endif
