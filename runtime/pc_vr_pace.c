// PC VR: the game's frames paced by the headset, the process's side. What it is for and the grid: runtime/pc_vr_pace.h.
#include "pc_vr_pace.h"

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

RT_ON_RESET(pc_vr_pace_reset) {
    previous_start_ns = 0;
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
