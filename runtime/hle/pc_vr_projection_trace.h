#ifndef SAVAGEWIND_PC_VR_PROJECTION_TRACE_H
#define SAVAGEWIND_PC_VR_PROJECTION_TRACE_H
#include <stdbool.h>
#include <stdint.h>
#include "pc_vr_native_view_math.h"
#include "../xr/xr_runtime.h"

// With PC VR on (its play configuration, runtime/pc_vr_config.h): the frame record, the native FOV and view and the
// render certificates, continuously. Called from head look's init.
void pc_vr_projection_trace_init(void);
// Whether this eye call is the root's (the frame's first eye call) by its guest stack: head look takes the frame's
// pose only there.
bool pc_vr_projection_trace_root(uint64_t guest_frame_pointer);
// Each call of the game's eye camera builder for a layer, before head look's camera binding is asked for: the Labo
// display's own root and sibling calls for its two eye layers (the same stack and link tests as the record's) make
// those layers known as the eye layers until the record knows one (render_scope_eye_layer; the same calls also show
// the pause menu's own pair, which only the record's binding tells apart). Grants no certificate and writes nothing.
void pc_vr_projection_trace_eye_layer(uint64_t layer, uint64_t guest_frame_pointer);
// An eye call of the composition head look is bound to (the layer copies the bound base camera), before the game's
// look-at builder runs, under head look's lock (lock order: head look's, then this module's). base_camera: the base
// as head look read it now; fresh: the eye is the game's fresh copy of it, moved sideways. At the root's call pose is
// the frame's pose (NULL: none in this LOCAL definition) and a new frame record starts; at the sibling's the record's
// pose is used (pose ignored). True: the eye's camera now holds its native view, and
// pc_vr_projection_trace_eye_built follows the original builder.
bool pc_vr_projection_trace_eye(uint64_t layer, uint64_t base, const PcVrHeadLookCamera *base_camera, bool fresh,
    const XrPoseSnapshot *pose, uint64_t guest_frame_pointer);
// After the game's look-at builder ran for an eye that pc_vr_projection_trace_eye wrote.
void pc_vr_projection_trace_eye_built(uint64_t layer);
// Resolve the exact row-major 3x4 matrix from the game's eye camera or a consumer's copy of it to the
// game-space midpoint of its own native eye pair. An absent or ambiguous match leaves midpoint untouched.
bool pc_vr_projection_view_midpoint(const float view[12], float midpoint[3]);
#endif
