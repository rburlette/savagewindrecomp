// PC VR renderer (development): the passes of one eye's scene, where the effect system draws and what it draws into
// (SAVAGEWIND_PC_VR_EFFECT_TRACE=1). Diagnostics only: it changes nothing that is drawn.
//
// When it runs: at each eye shot request (Right Ctrl, the controller's capture button, the control endpoint's
// `eyeshot`; runtime/pc_vr.h pc_vr_eye_shot_requests), at most 8 a run, the frame after the next present is traced.
// Within that frame the left eye (its certificates' native_index 0) is followed from its first certified draw to its
// scene-final draw (the draw tagged PC_VR_LAYER_DRAW_FINAL with that eye's certificate), and logged as
// [pc-vr-effect-trace] lines, in replay order:
//  - a pass: a run of draws into the same render targets, its colour and depth targets (Tn, named in the legend), its
//    draws: all, the eye's, the other eye's and uncertified ones; blended (colour target 0's blend on), depth tested and
//    depth written; the effect system's (tagged PC_VR_LAYER_DRAW_EFFECT, known by the game's call of nn::vfx's
//    System::Draw), how many of those are the field haze's programs (vr_field_cloud_shader.h), blended and depth
//    written; the distinct effect programs by fragment shader hash; the render targets the pass's draws sample; its
//    draws' distinct viewport and scissor rectangles with their draw counts (all, effect), when the pass has effect
//    draws or a rectangle leaves part of its first colour target (or its depth target) uncovered;
//  - between draws: compute dispatches, texture copies and clears (consecutive ones of a kind counted together);
//  - the legend: each target's guest size and format and the renderer's image (size, Vulkan format);
// and at the next present, the whole frame: the effect-tagged draws by eye, and the field haze's draws, tagged or not.
// Each depth target the followed eye's effects test against, and each render target they sample, is saved the first
// time, at that draw (vr_shots.h vr_shot_save_now: build/vr-effect-trace<n>-T<number>-depth.png, grey, its pixel
// census logged, and -sampled.png, through the HDR preview), its render pass ended for it; and each colour target they
// drew into, when a draw first reads it (-effects-buffer.png: the game's composite of a buffer of effects, complete
// then); at most 8 a trace.
// A frame whose left eye never reaches its scene-final draw logs what was seen, marked so.
//
// Not owned here: which draw is an eye's (the certificates, pc_vr_render_trace.h), what a draw is (its tag,
// runtime/nvn/nvn.h nvn_layer_draw), the field haze's programs (exec.c's Prog.vr_field_cloud).
//
// Threading: every call is serialised by gpu_lock (exec.c's draws, dispatches, copies and clears, and vk_vr_present),
// so the module's state needs no lock of its own. The state lives for the process: the request count and the traces
// done are counted for the whole run, through a game reset.
#ifndef SAVAGEWIND_VR_EFFECT_TRACE_H
#define SAVAGEWIND_VR_EFFECT_TRACE_H

#include <stdbool.h>
#include <stdint.h>

#include "../nvn.h"

// True while a traced frame runs (from the present after a request to the next present): the call sites below only
// then gather what they pass. A plain read: the module's calls hold gpu_lock.
extern bool vr_effect_trace_running;
static inline bool vr_effect_trace_active(void) { return vr_effect_trace_running; }
// (SAVAGEWIND_PC_VR_EFFECT_TRACE=2) The reduced targets a traced eye's draws read are saved as well.
bool vr_effect_trace_small_targets(void);

// One draw as the game issued it, with its program known (exec.c draw_, after the program is ready).
typedef struct {
    const CmdRec *command;           // its tag (nvn_layer_draw) and certificate (nvn_render_certificate)
    NvnTexture *const *colors;       // the colour targets bound
    int color_count;
    const NvnTexture *depth;         // the depth target bound, or NULL
    NvnTexture *const *sampled;      // the render targets and display images its fragment stage samples
    int sampled_count;
    uint64_t fragment_hash;          // the fragment shader's hash (exec.c g_cur_fs_hash)
    bool blended, depth_test, depth_write;
    bool field_haze;                 // its program is one of the field haze's
    const float *viewport;           // viewport 0: x, y, width, height, in the game's units (its target's guest size)
    const int32_t *scissor;          // scissor 0: x, y, width, height, the same units (16384 wide: none)
} VrEffectTraceDraw;
void vr_effect_trace_draw(const VrEffectTraceDraw *draw);

typedef enum { VR_EFFECT_TRACE_DISPATCH, VR_EFFECT_TRACE_COPY, VR_EFFECT_TRACE_CLEAR_COLOR,
               VR_EFFECT_TRACE_CLEAR_DEPTH } VrEffectTraceEvent;
// Something between draws: a compute dispatch (no textures), a copy (source, destination) or a clear (the target).
void vr_effect_trace_event(VrEffectTraceEvent kind, const NvnTexture *source, const NvnTexture *destination);

// At each present (exec.c vk_vr_present): ends a traced frame (its totals logged) and arms the next on a new request.
void vr_effect_trace_present(void);

#endif
