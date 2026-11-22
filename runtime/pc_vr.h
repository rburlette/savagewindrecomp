// PC VR, switched on by SAVAGEWIND_PC_VR=1 before the process starts: the switch and launch settings shared by the
// launcher, the game hooks and the renderer, the startup into the game's Labo VR mode, and the 2D panels' distances
// (runtime/pc_vr.c). Every setting, and its default: the configuration (runtime/pc_vr_config.h). The other parts: the
// XR owner (runtime/xr/), head look and native eye views (runtime/hle/pc_vr_*), the renderer's eye images and panels
// (runtime/nvn/vulkan/exec.c, the vr_ functions), the world-tied HUD (pc_vr_hud.h).
#pragma once
#include <stdbool.h>
#include "cpu.h"
#include "pc_vr_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Strict, process-latched opt-in shared by launcher, game hooks and renderer (pc_vr_config()->enabled).
bool pc_vr_enabled(void);

// PC VR's own shader translations are used (pc_vr_config()->shader_variants): always in PC VR, and in a run that only
// prepares its shaders, which needs neither the OpenXR runtime nor the headset.
bool pc_vr_shader_variants(void);

// The controller keeps working while the game's desktop window lacks the focus (runtime/hle/input.c): in PC VR the
// headset is the display, and a desktop window brought to the front would otherwise silence the controller mid-play.
// The keyboard and mouse still follow the window's focus.
bool pc_vr_controller_without_focus(void);

// The controller's capture button (the free-fly binding; runtime/hle/input.c, any thread) asks for the eye images the
// headset is given in the next frame, saved at full size as build/vr-shot-mark<n>-submit0/1-<present>.png in the data
// folder (runtime/nvn/vulkan/vr_shots.c). The pair's frame waits for the GPU's copy of them; the files are written by
// threads of their own.
// Each request's log line says where it was taken: the player's place and the game camera (a line the control
// endpoint's `goto` takes) and the head's pose.
void pc_vr_eye_shot_request(void);
// The same request from the keyboard: Right Ctrl, whichever window has the focus (Windows only). Called once a
// frame by the input poll.
void pc_vr_eye_shot_key_poll(void);
// The requests so far; the renderer saves one pair for those it has not served yet.
unsigned pc_vr_eye_shot_requests(void);

// The exact raster size for both eye rendering and submission, PC VR only: SAVAGEWIND_PC_VR_EYE_SIZE=WxH (each axis
// 128..4096, process-latched), else the eye scale's pixels in the OpenXR runtime's view shape once discovery has run
// (pc_vr_eye_size.h). False: the eye targets keep Labo's shape (EyeScale alone, or none). Poses/FOV remain runtime-derived.
bool pc_vr_eye_size(unsigned *width, unsigned *height);

// The game version's PC VR sites are there (runtime/game_sites.h GAME_SITES_PC_VR; a version tools/game_sites.py
// finds them all in) and main is laid out as they were found: the hooks take their addresses from g_site.
bool pc_vr_main_compatible(void);

// Pinned BOTW 1.8.2 lifecycle predicate. The caller must additionally check
// the SDK import's exact return site before changing that call's behavior.
bool pc_vr_labo_mode2_tracking(const Cpu *cpu);

// Optional import wrappers for the pinned original mode-2 tracking lifecycle.
// Returns NULL for all other imports, including ordinary desktop launch.
typedef void (*PcVrHostFn)(Cpu *);
PcVrHostFn pc_vr_lifecycle_lookup(const char *name);

// Called from the existing original title-menu update hook on the guest thread.
// Requests the original VR-object transition once the platform manager exists.
void pc_vr_startup_tick(void);

// Read-only completion/timeout poll from the game's persistent frame-wait
// hook. No original guest function is called from this path.
void pc_vr_startup_poll(void);

// Live world scale of the native eye views (hle/pc_vr_projection_trace.c): factor > 0 multiplies the game units per
// metre (bigger: the world looks smaller, a toy in front of you), 0 restores the launch value. Pairs already built
// keep theirs. Returns the new value, or 0 when the native view experiment is off.
bool pc_vr_world_scale_active(void);
double pc_vr_world_scale(double factor);

// Depth setting of a world-locked 2D panel (pc_vr.c), 0 while that content keeps the projection layer: the
// menus' distance in metres (D056, 0.1..20) or the gameplay HUD's distance as a multiple of the camera target's
// (D057, 0.1..4). The adjustment multiplies it by factor > 0 within that range, or restores the launch value for 0,
// and returns the new value.
typedef enum { PC_VR_PANEL_MENU, PC_VR_PANEL_HUD } PcVrPanel;
double pc_vr_panel_depth(PcVrPanel which);
double pc_vr_panel_depth_adjust(PcVrPanel which, double factor);

// D057: the HUD quad's distance in metres, `depth` times the camera target's distance `focus_m` (near Link, so it
// follows the camera's zoom and the world scale), eased from `previous_m` over `dt_s` seconds in log distance (time
// constant 0.25 s, so a camera cut moves it smoothly; `previous_m` 0 starts at the target), within 0.1..20 m. Without
// a focus distance it keeps `previous_m`, or 0.77 m.
double pc_vr_hud_distance(double depth, double focus_m, double previous_m, double dt_s);

// D057: how far ahead the game camera's target (near Link) appears, in metres at the current world scale; 0 until the
// native views have run (hle/pc_vr_projection_trace.c).
double pc_vr_camera_focus_distance(void);

#ifdef __cplusplus
}
#endif
