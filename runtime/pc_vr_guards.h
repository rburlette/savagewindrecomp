// PC VR: how often each guard on the eye path refuses, counted by reason (step 0 of the validation review, 2026-11-13;
// runtime/pc_vr_guards.c).
//
// What it is for: a guard is replaced only with evidence of what it catches (AGENTS.md). Before these counters the
// camera trace logged only its first few refusals and the XR owner counted refused offers only with
// SAVAGEWIND_PC_VR_OFFER_TRACE=1, so a run could not say which guard turned a frame away. Every refusal is now
// counted, always, and the totals are logged now and then and at exit as one "[pc-vr-guards]" line
// (scripts/check-pc-vr.ps1 prints the last one). Counting changes no decision.
//
// Thread safety: the counters are relaxed atomics; any thread may count, and the log reads them without a lock (a
// line can be one count behind another).
#ifndef SAVAGEWIND_PC_VR_GUARDS_H
#define SAVAGEWIND_PC_VR_GUARDS_H
#include <stdatomic.h>
#include <stdint.h>

typedef enum {
    // The camera trace (runtime/hle/pc_vr_projection_trace.c): why an eye's native view or FOV was not applied, or a
    // traversal (one frame's pair of eye calls) was refused.
    PC_VR_GUARD_VIEW_HEAD_LOOK_NOT_APPLIED,  // head look did not apply its pose to the eye
    PC_VR_GUARD_VIEW_PRECONDITION,           // the eye, its traversal or its pose did not allow the write
    PC_VR_GUARD_VIEW_WRITE_FAILED,           // the camera fields did not read back as written (restored)
    PC_VR_GUARD_VIEW_MATRIX_MISMATCH,        // the game's view matrix after its builder is not the written view
    PC_VR_GUARD_FOV_PRECONDITION,            // the projection's state did not allow the FOV write
    PC_VR_GUARD_FOV_WRITE_FAILED,            // the FOV fields did not read back as written (restored)
    PC_VR_GUARD_FOV_CONVERSION_MISMATCH,     // the game's logical or device matrix is not the FOV written
    PC_VR_GUARD_TRAVERSAL_FRAME_UNREADABLE,  // the eye call's saved frame could not be read
    PC_VR_GUARD_TRAVERSAL_SERIAL_EXHAUSTED,  // (the traversal serial ran out)
    PC_VR_GUARD_TRAVERSAL_ROOT_IDENTITY,     // the root eye's configuration or sibling is not a valid object
    PC_VR_GUARD_TRAVERSAL_NOT_SIBLING,       // the second eye call is not the root's sibling call
    PC_VR_GUARD_TRAVERSAL_LINK_GONE,         // the root's live link to the sibling no longer holds
    PC_VR_GUARD_TRAVERSAL_BOUND_EYE,         // the bound eye's base pose or eye position is not finite
    // The pose check at the traversal's root and pair (projection_pose_current): the headset pose then.
    PC_VR_GUARD_POSE_BUSY,        // the published pose's lock was held (trylock)
    PC_VR_GUARD_POSE_UNAVAILABLE, // no valid published pose
    PC_VR_GUARD_POSE_AGE,         // the frame's or the current pose older than the limit (250 ms)
    PC_VR_GUARD_POSE_INVALID,     // the frame's or the current pose not tracked, not finite or without native views
    PC_VR_GUARD_POSE_GENERATION,  // the current pose of another reference generation (a recenter)
    // A draw's certificate checked against the eye layer's guest state (render_fingerprint): it changed since.
    PC_VR_GUARD_FINGERPRINT_MISMATCH,
    // The renderer's frame (runtime/nvn/vulkan/vr_frame.h): a scene frame not offered, or a menu frame without a panel.
    PC_VR_GUARD_FRAME_UNCERTIFIED_MISSING,   // an eye's scene-final draw without its own certificate
    PC_VR_GUARD_FRAME_UNCERTIFIED_DISAGREE,  // both certified, but of different traversals, frames or poses
    PC_VR_GUARD_FRAME_MISPLACED,
    PC_VR_GUARD_FRAME_MENU_WITHOUT_POSE,
    PC_VR_GUARD_FRAME_MENU_WITHOUT_PANEL,
    // The XR owner's admission of an offered pair (runtime/xr/xr_images_internal.h, EyeOfferReject in its order).
    PC_VR_GUARD_OFFER_CONTRACT,
    PC_VR_GUARD_OFFER_PANEL,
    PC_VR_GUARD_OFFER_HUD,
    PC_VR_GUARD_OFFER_POSE_IDENTITY,
    PC_VR_GUARD_OFFER_POSE_AGE,
    PC_VR_GUARD_OFFER_EYES,
    PC_VR_GUARD_OFFER_STOPPED,
    PC_VR_GUARD_OFFER_RESET,
    PC_VR_GUARD_OFFER_GENERATION,
    PC_VR_GUARD_OFFER_MAILBOX,
    PC_VR_GUARD_COUNT
} PcVrGuard;

extern _Atomic uint64_t pc_vr_guard_counts[PC_VR_GUARD_COUNT];

static inline void pc_vr_guard_count(PcVrGuard guard) {
    atomic_fetch_add_explicit(&pc_vr_guard_counts[guard], 1, memory_order_relaxed);
}

// Logs one "[pc-vr-guards] at=<when> total=<n> <name>=<count> ..." line with every reason counted so far (those at 0
// left out).
void pc_vr_guards_log(const char *when);

#endif
