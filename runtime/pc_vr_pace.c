// PC VR: the game's frames paced by the headset, the process's side. What it is for and the grid: runtime/pc_vr_pace.h.
#include "pc_vr_pace.h"

#include <stdatomic.h>

#include "hle.h"
#include "kernel.h"
#include "pc_vr_config.h"
#include "pc_vr_frame_trace.h"
#include "platform.h"
#include "rt.h"
#include "xr/xr_runtime.h"

// The start of the last paced frame, in the XR owner's clock (xr_runtime_pose_clock_ns); 0: none yet. Read and written
// by the game's presenting thread only, as the frame limiter's own state is.
static uint64_t previous_start_ns;
// The same start and the game frame's length, for the pose wait on the thread that builds the eye cameras.
static _Atomic uint64_t shared_start_ns, shared_game_period_ns;

RT_ON_RESET(pc_vr_pace_reset) {
    previous_start_ns = 0;
    atomic_store(&shared_start_ns, 0);
}

bool pc_vr_pace_on(void) {
    const PcVrConfig *config = pc_vr_config();
    return config->enabled && config->diagnostics.headset_pace;
}

bool pc_vr_pace_frame(uint64_t limit_period_ns, PcVrPacedFrame *paced) {
    if (!pc_vr_pace_on()) return false;
    PcVrDisplayClock clock;
    if (!xr_runtime_display_clock(&clock.frame_start_ns, &clock.period_ns)) {
        previous_start_ns = 0;
        atomic_store(&shared_start_ns, 0);
        return false;
    }
    const PcVrConfig *config = pc_vr_config();
    const unsigned display_frames =
        pc_vr_pace_display_frames(config->diagnostics.headset_pace, limit_period_ns, clock.period_ns);
    const uint64_t now_xr_ns = xr_runtime_pose_clock_ns();
    const uint64_t start_xr_ns = pc_vr_pace_frame_start(&clock, config->diagnostics.headset_pace_offset_ns,
        display_frames * clock.period_ns, previous_start_ns, now_xr_ns);
    static bool logged;
    if (!logged) {
        logged = true;
        LOG("[pc-vr] headset pace: a game frame every %u display frames of %.3f ms, starting %+.3f ms from one's start\n",
            display_frames, clock.period_ns / 1e6, config->diagnostics.headset_pace_offset_ns / 1e6);
    }
    previous_start_ns = start_xr_ns;
    atomic_store(&shared_game_period_ns, display_frames * clock.period_ns);
    atomic_store(&shared_start_ns, start_xr_ns);
    // The sleep is in the platform's clock: the same wait from now.
    const uint64_t before_ns = plat_now_ns();
    *paced = (PcVrPacedFrame){.started_ns = before_ns};
    if (start_xr_ns > now_xr_ns) {
        paced->started_ns = before_ns + (start_xr_ns - now_xr_ns);
        kernel_sleep_until_ns(paced->started_ns);
        paced->slept_ns = plat_now_ns() - before_ns;
        paced->on_grid = true;
    }
    // (the late-frame trace: the next frame's start)
    pc_vr_frame_trace_started(paced->started_ns, paced->slept_ns, paced->on_grid, display_frames);
    return true;
}

bool pc_vr_pace_pose_wait_on(void) {
    return pc_vr_pace_on() && pc_vr_config()->diagnostics.pose_wait_budget_ns;
}

void pc_vr_pace_pose_wait(void) {
    const uint64_t work_budget_ns = pc_vr_config()->diagnostics.pose_wait_budget_ns;
    if (!pc_vr_pace_pose_wait_on()) return;
    const uint64_t paced_start_ns = atomic_load(&shared_start_ns);
    PcVrDisplayClock clock;
    if (!paced_start_ns || !xr_runtime_display_clock(&clock.frame_start_ns, &clock.period_ns) || !clock.period_ns) return;
    const uint64_t now_xr_ns = xr_runtime_pose_clock_ns();
    // The game's next present: the start the presenting thread is waiting for, or one game frame after a start that
    // has passed (a late frame did not wait).
    const uint64_t next_present_ns =
        paced_start_ns > now_xr_ns ? paced_start_ns : paced_start_ns + atomic_load(&shared_game_period_ns);
    const uint64_t wait_until_xr_ns = pc_vr_pace_pose_wait_until(&clock, next_present_ns, work_budget_ns, now_xr_ns);
    // (this thread alone: the counts of the line below)
    static uint64_t calls, waits, waited_ns, newer_poses;
    ++calls;
    if (wait_until_xr_ns) {
        XrPoseSnapshot pose = {0};
        xr_runtime_get_pose(&pose);
        const uint64_t display_frame_before = pose.frame_id;
        const uint64_t began_ns = plat_now_ns();
        kernel_sleep_until_ns(began_ns + (wait_until_xr_ns - now_xr_ns));
        // The owner publishes as its display frame starts, a moment after the clock's own grid point.
        for (unsigned polls = 0; polls < 8; ++polls) {
            xr_runtime_get_pose(&pose);
            if (pose.frame_id != display_frame_before) break;
            kernel_sleep_until_ns(plat_now_ns() + 250000);
        }
        ++waits;
        waited_ns += plat_now_ns() - began_ns;
        if (pose.frame_id != display_frame_before) ++newer_poses;
    }
    if (calls % 900 == 0) {
        LOG("[pc-vr] pose wait: budget-ms=%.2f calls=%llu waited=%llu newer-pose=%llu mean-wait-ms=%.3f\n",
            work_budget_ns / 1e6, (unsigned long long)calls, (unsigned long long)waits,
            (unsigned long long)newer_poses, waits ? waited_ns / 1e6 / (double)waits : 0.0);
        calls = waits = waited_ns = newer_poses = 0;
    }
}
