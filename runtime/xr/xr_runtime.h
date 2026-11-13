#ifndef SAVAGEWIND_XR_RUNTIME_H
#define SAVAGEWIND_XR_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

// PC VR's OpenXR owner (runtime/xr/xr_runtime.c): the headset's head and eye poses for the game's cameras, and the
// rendered eye images back to the headset (xr_vulkan.h).

// One eye as the XR runtime located it: orientation (x, y, z, w) and position (metres) in LOCAL space, and the FOV's
// four edge angles in radians (left and down negative). A snapshot alone says nothing about which images were
// rendered with it: that is the renderer's certificate (runtime/hle/pc_vr_render_trace.h).
typedef struct {
    float orientation_xyzw[4];
    float position_xyz[3];
    float fov_left_right_up_down[4];
} XrEyeSnapshot;

// D053 calibration: the head's heading (yaw about LOCAL +Y only, xyzw) and
// eye midpoint (LOCAL meters), captured on the first fully tracked frame after
// LOCAL is defined. Stable across tracking loss, focus changes and game
// sections; a new epoch starts only with a new session or an applied LOCAL
// change (the runtime's recenter). epoch 0: not captured yet.
typedef struct {
    uint64_t epoch;
    float heading_xyzw[4];
    float origin_xyz[3];
} XrCalibration;

// One tracking sample, published by the XR owner thread (xr_runtime_get_pose). The head's orientation and both eyes
// at the frame's predicted display time.
typedef struct {
    uint64_t frame_id;         // the XR owner's display frame that located it
    uint64_t game_present_id;  // the game present it was published for (0: a display-rate refresh)
    uint64_t generation;       // changes on a recenter, a session state change, tracking loss or recovery, a reset
    // When it was located, on xr_runtime_pose_clock_ns()'s clock, whose epoch differs from plat_now_ns(): never
    // compare those two clocks directly.
    uint64_t published_monotonic_ns;
    int64_t predicted_display_time_ns;  // OpenXR time: opaque, only for OpenXR calls
    float orientation_xyzw[4];          // the head (the VIEW space), LOCAL
    // Native xrLocateSpace velocity at the same predicted time, expressed in
    // LOCAL/base-space coordinates, right-handed radians/second. Independent
    // of orientation validity; consumers must require this flag before use.
    float angular_velocity_xyz[3];
    bool angular_velocity_valid;
    bool active;  // render-valid tracking in VISIBLE or FOCUSED
    bool focused; // XR input focus metadata; not a tracking/rendering gate
    bool orientation_valid;
    bool orientation_tracked;
    bool views_valid;
    // Stricter image provenance gate; all native per-eye orientation AND
    // position validity/tracking flags must be present. Does not change the
    // existing orientation-only head-look availability contract.
    bool native_views_valid;
    XrEyeSnapshot eyes[2];
    XrCalibration calibration;
} XrPoseSnapshot;

// Pure: the yaw-only heading of head_xyzw (forward -Z projected onto LOCAL's
// horizontal plane) and the midpoint of the two eye positions. Fails, leaving
// out untouched, for invalid input or a head within ~10 degrees of vertical.
bool xr_calibration_capture(const float head_xyzw[4], const float left_xyz[3],
                            const float right_xyz[3], uint64_t epoch, XrCalibration *out);
// Pure: `current` with its eyes moved to the calibration pose (head at the
// origin facing the heading, level), keeping its head-local eye offsets,
// optical rotations, FOV and tracking fields. A render pose for 2D layers
// that stay where the game's forward is. Fails without a calibration or
// native views, leaving out untouched.
bool xr_calibration_anchor(const XrPoseSnapshot *current, XrPoseSnapshot *out);
// Pure (D056): the pose of an upright flat panel `distance` meters ahead of
// the calibration origin along its heading and `rise` meters above it, its
// front (+Z) facing back along the heading. Fails without a calibration or
// for invalid input, leaving the outputs untouched.
bool xr_calibration_panel(const XrCalibration *calibration, float distance, float rise,
                          float position_xyz[3], float orientation_xyzw[4]);

// Benchmark only (SAVAGEWIND_PC_VR_BENCH_HEAD): the scripted head turn `step`
// game presents into the script, in radians. Fails once the script is over.
// Phases: 0-599 yaw sweep +-70 degrees, 600-899 pitch sweep +-25 degrees,
// 900-1499 one full turn, then nothing.
#define XR_BENCH_HEAD_STEPS 1500u
bool xr_bench_head_turn(uint64_t step, double *yaw, double *pitch);
// Pure: turns the head orientation and both eyes (orientation, and position
// about their midpoint) by LOCAL yaw (about +Y) after pitch (about +X). FOV
// is kept. Fails, leaving the inputs untouched, for non-unit orientations.
bool xr_bench_head_apply(double yaw, double pitch, float head_xyzw[4], XrEyeSnapshot eyes[2]);

typedef enum {
    XR_POSE_UNAVAILABLE = 0,
    XR_POSE_VALID = 1,
    XR_POSE_BUSY = 2
} XrPoseReadResult;

// Original guest present only, never desktop re-presentation. Nonblocking,
// coalescing: does not wait for XR and never advances game simulation. The
// display-rate owner publishes its next tracking sample for this present;
// it neither paces XR frames nor authorizes image submission.
void xr_runtime_notify_game_present(uint64_t game_present_id);
// No XR calls and no blocking lock; BUSY must not reset consumer calibration.
// VALID requires visible, tracked orientation and a publication age <=250ms.
XrPoseReadResult xr_runtime_get_pose(XrPoseSnapshot *out);
// Same monotonic clock/epoch used for snapshot publication and freshness.
// Returns zero in an OpenXR-disabled build. No XR call or lock is performed.
uint64_t xr_runtime_pose_clock_ns(void);
// Called after stopping guest threads; drops old notifications and calibration.
void xr_runtime_reset_game(void);
// Joins the sole frame owner, then releases XR before Vulkan may be destroyed.
void xr_runtime_shutdown(void);
// Controlled cleanup/retry: false retains unresolved handles. The void exit
// wrapper instead terminates with failure before loader/GPU teardown proceeds.
bool xr_runtime_shutdown_checked(void);
// Diagnostic attribution for a synchronous validation callback on this thread.
// A runtime's own background thread may correctly report "outside-XR-call".
const char *xr_runtime_current_call(void);

// vkDeviceWaitIdle touches every queue, including the reserved XR queue.
// Pair these only around device-wide idle operations, not game submissions.
void xr_runtime_vulkan_idle_begin(void);
void xr_runtime_vulkan_idle_end(void);

#endif
