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
    // The frame record (runtime/hle/pc_vr_projection_trace.c): why an eye's view or FOV was not given this frame.
    PC_VR_GUARD_VIEW_NOT_FRESH,              // the eye is not the game's fresh copy of the bound base camera
    PC_VR_GUARD_VIEW_NO_POSE,                // no pose with native views in this LOCAL definition (none held yet)
    PC_VR_GUARD_VIEW_PAIR,                   // the eyes' views could not be built from the pose (numerical contracts)
    PC_VR_GUARD_VIEW_PLACEMENT,              // the game's eye copy is not the base camera moved to its own side
    PC_VR_GUARD_VIEW_WRITE_FAILED,           // the camera fields could not be written (nothing changed)
    PC_VR_GUARD_FOV_PRECONDITION,            // the eye's view not built on this thread, or its FOV does not map
    PC_VR_GUARD_FOV_WRITE_FAILED,            // the FOV fields could not be written (nothing changed)
    PC_VR_GUARD_FOV_CONVERSION_MISMATCH,     // the game's logical matrix does not give the eye's FOV
    PC_VR_GUARD_TRAVERSAL_FRAME_UNREADABLE,  // the eye call's saved frame could not be read, or is neither eye's call
    PC_VR_GUARD_TRAVERSAL_ROOT_IDENTITY,     // the root eye's layer, configuration or sibling is not a valid object
    PC_VR_GUARD_TRAVERSAL_NOT_SIBLING,       // the second eye call is not the open root's sibling call
    PC_VR_GUARD_CERTIFICATE_EXPIRED,         // a recorded draw requested an issued ID whose slot was reused
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
    PC_VR_GUARD_OFFER_STOPPED,
    PC_VR_GUARD_OFFER_RESET,
    PC_VR_GUARD_OFFER_LOCAL,
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
// A reason's name, as the log line spells it.
const char *pc_vr_guard_name(PcVrGuard guard);

#endif
