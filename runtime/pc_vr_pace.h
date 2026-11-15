// PC VR: the game's frames paced by the headset. An experiment (2026-11-15), off unless SAVAGEWIND_PC_VR_HEADSET_PACE
// is set; without it the frame limiter (runtime/nvn/objects.c present_pace) is as upstream has it.
//
// Why. The frame limiter keeps a grid of its own, a frame every 1 / frame rate from wherever the first one fell, and
// waits at the present, before the frame's image is offered to the headset. With the headset at 90 Hz and the game at
// 45 frames a second each image is shown for two display frames, but a finished image waits out the limiter before
// the XR owner can take it, and where a frame starts within a display frame is chance. Measured on Link with the
// frame time to spare (eye scale 2.4 and 2.7): each image's head pose 57 ms old when shown, five display frames.
//
// What this does instead:
//  - the game's frame starts on the headset's grid: a whole number of display frames after the last one, and a set
//    time (the offset) after a display frame's start, which is when the XR owner takes the finished images and
//    publishes the head pose the next frame reads;
//  - the wait comes after the present has been handed on (objects.c nvnQueuePresentTexture), so a finished frame's
//    image goes to the headset at once and the spare time is spent before the next frame reads its pose;
//  - a late frame never waits: it starts at once, and the grid carries on from the display frame it fell in.
// The game's clock follows the frames' real lengths as before (framepace_presented). The head pose should reach the
// game every display frame with it (SAVAGEWIND_PC_VR_DISPLAY_RATE_TRACKING=1), or the pose read after the wait is the
// one published for the present before it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The headset's display clock as the XR owner last saw it, in one monotonic clock (nanoseconds).
typedef struct {
    uint64_t frame_start_ns;  // when its latest display frame started (xrWaitFrame returned)
    uint64_t period_ns;       // one display period; 0: no display frame yet
} PcVrDisplayClock;

// How many display frames one game frame takes: at least `least` (the setting), and enough that the game runs no
// faster than the frame limiter allows (limit_period_ns: its period; 0: none). A limit up to 1% of a display period
// over a whole number of them is that number: 45 frames a second at 90 Hz is two, the default 60 is two as well (it
// cannot be one and a half), an event held at 30 is three.
static inline unsigned pc_vr_pace_display_frames(unsigned least, uint64_t limit_period_ns, uint64_t display_period_ns) {
    unsigned display_frames = least ? least : 1;
    if (limit_period_ns && display_period_ns) {
        const uint64_t tolerance = display_period_ns / 100;
        const uint64_t limited = limit_period_ns > tolerance ? limit_period_ns - tolerance : 0;
        const uint64_t needed = (limited + display_period_ns - 1) / display_period_ns;  // (rounded up)
        if (needed > display_frames) display_frames = needed > 1000 ? 1000 : (unsigned)needed;
    }
    return display_frames;
}

// (the grid point at or before `time`: grid points are origin + a whole number of periods, either side of origin)
static inline int64_t pc_vr_pace_grid_floor(int64_t time, int64_t origin, int64_t period) {
    int64_t steps = (time - origin) / period;
    if ((time - origin) % period < 0) --steps;
    return origin + steps * period;
}

// When the game's next frame starts, in the display clock's time. Grid points lie offset_ns after each display frame's
// start. The answer is the grid point nearest to game_period_ns after the previous frame's start (previous_start_ns;
// 0: no previous frame, the nearest to now_ns) when that is still ahead; otherwise the grid point now_ns falls in,
// which is at or before now_ns: a late frame does not wait. Needs clock->period_ns above 0.
static inline uint64_t pc_vr_pace_frame_start(const PcVrDisplayClock *clock, uint64_t offset_ns,
                                              uint64_t game_period_ns, uint64_t previous_start_ns, uint64_t now_ns) {
    const int64_t period = (int64_t)clock->period_ns;
    const int64_t origin = (int64_t)(clock->frame_start_ns + offset_ns % clock->period_ns);
    const int64_t wanted = previous_start_ns ? (int64_t)(previous_start_ns + game_period_ns) : (int64_t)now_ns;
    int64_t start = pc_vr_pace_grid_floor(wanted + period / 2, origin, period);  // (the nearest grid point)
    if (start < (int64_t)now_ns) start = pc_vr_pace_grid_floor((int64_t)now_ns, origin, period);
    return (uint64_t)start;
}

// ---- The process's pace (runtime/pc_vr_pace.c), for the frame limiter's two call sites in runtime/nvn/objects.c.

// PC VR with the headset pace set: the frame's wait comes after its present is handed on, not before.
bool pc_vr_pace_on(void);

// What the wait did, for the frame limiter's own bookkeeping.
typedef struct {
    uint64_t started_ns;  // when the next frame starts (plat_now_ns): the grid point waited for, or now when late
    uint64_t slept_ns;    // how long this call slept
    bool on_grid;         // the frame waited for its grid point (a late one did not)
} PcVrPacedFrame;

// Waits for the next frame's start on the headset's grid (limit_period_ns: the frame limiter's period, 0 for none).
// False, having done nothing: the pace is off or the headset has shown no display frame yet; the frame limiter then
// paces as without it. The game's presenting thread only.
bool pc_vr_pace_frame(uint64_t limit_period_ns, PcVrPacedFrame *paced);

#ifdef __cplusplus
}
#endif
