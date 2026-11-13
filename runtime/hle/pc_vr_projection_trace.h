#ifndef SAVAGEWIND_PC_VR_PROJECTION_TRACE_H
#define SAVAGEWIND_PC_VR_PROJECTION_TRACE_H
#include <stdbool.h>
#include <stdint.h>
#include "pc_vr_native_view_math.h"
#include "../xr/xr_runtime.h"

// With PC VR on (its play configuration, runtime/pc_vr_config.h): the pinned-build observer, the native FOV and view
// and the render certificates, continuously. Called from head look's init.
void pc_vr_projection_trace_init(void);
// Read-only classification of the exact pinned root stack. This grants no
// camera/certificate eligibility; the caller still validates the live binding
// and freshly copied eye. A sibling must never acquire a new head pose.
bool pc_vr_projection_trace_root(uint64_t guest_frame_pointer);
// Each call of the game's eye camera builder for a layer, before head look's camera binding is asked for: the Labo
// display's own root and sibling calls for its two eye layers (the same stack and link tests as a traversal's) make
// those layers known as the eye layers until an eye note exists (render_scope_eye_layer; the same calls also show the
// pause menu's own pair, which only a note's binding tells apart). Grants no certificate and writes
// nothing to the guest.
void pc_vr_projection_trace_eye_layer(uint64_t layer, uint64_t guest_frame_pointer);
void pc_vr_projection_trace_eye(uint64_t layer, uint64_t bound_base,
    uint64_t latch_sequence, uint64_t frame_id, uint64_t generation, bool latch_valid,
    uint64_t guest_frame_pointer, const XrPoseSnapshot *head_pose);
// Acknowledge only the existing accepted head-look result, without changing
// its transform or calibration. Called under the head-look lock; this module
// must never acquire that lock in the opposite direction.
void pc_vr_projection_trace_head_result(uint64_t layer, uint64_t latch_sequence, bool applied);

typedef enum {
    PC_VR_NATIVE_VIEW_LEGACY = 0,
    PC_VR_NATIVE_VIEW_SKIPPED,
    PC_VR_NATIVE_VIEW_REJECTED,
    PC_VR_NATIVE_VIEW_APPLIED
} PcVrNativeViewResult;

// Local to one invocation of the original camera builder, never a global
// latest-camera token. A nonzero note requires post-original verification.
typedef struct {
    uint64_t note, traversal_serial, layer, sequence;
    PcVrNativeViewReference reference;
} PcVrNativeViewToken;

// Called only after the existing fresh-camera/head-look guards, under its
// lock. NULL reference means calibration warmup: skip the WHOLE traversal.
// APPLIED owns the 36-byte camera write; the caller must not compose legacy
// head look again. Other outcomes never leave a native camera mutation.
PcVrNativeViewResult pc_vr_projection_trace_native_view(uint64_t layer, uint64_t sequence,
    const PcVrHeadLookCamera *center, const PcVrHeadLookCamera *original_eye,
    const PcVrNativeViewReference *reference, PcVrNativeViewToken *token);
// Run after the original builder exactly once. calibration_current is checked
// by the head-look owner under its lock, without reversing lock acquisition.
bool pc_vr_projection_trace_native_view_built(const PcVrNativeViewToken *token,
    bool calibration_current);
#endif
