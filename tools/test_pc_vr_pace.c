// The headset pace's grid (runtime/pc_vr_pace.h), asset-free: how many display frames a game frame takes for a frame
// limit, and where the next frame starts. A game faster than its pace keeps an exact grid on the headset's display
// frames; one slower than its pace never waits; a hitch, a first frame, a display clock that jitters or lies ahead of
// the time asked about all land on a grid point.
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/pc_vr_pace.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "PC VR pace check failed line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

#define MILLISECOND 1000000ull
static const uint64_t period_90hz = 11111111, period_72hz = 13888889;

static bool on_grid(const PcVrDisplayClock *clock, uint64_t offset_ns, uint64_t time_ns) {
    const int64_t from_origin = (int64_t)time_ns - (int64_t)(clock->frame_start_ns + offset_ns % clock->period_ns);
    return from_origin % (int64_t)clock->period_ns == 0;
}

static void display_frames_for_a_limit(void) {
    CHECK(pc_vr_pace_display_frames(2, 16666667, period_90hz) == 2);   // the default 60: two display frames at 90 Hz
    CHECK(pc_vr_pace_display_frames(2, 22222222, period_90hz) == 2);   // 45
    CHECK(pc_vr_pace_display_frames(1, 22222222, period_90hz) == 2);   // 45 with no least: still two
    CHECK(pc_vr_pace_display_frames(2, 33333333, period_90hz) == 3);   // an event held at 30
    CHECK(pc_vr_pace_display_frames(2, 50000000, period_90hz) == 5);   // 20: four and a half, rounded up
    CHECK(pc_vr_pace_display_frames(1, period_90hz, period_90hz) == 1);
    CHECK(pc_vr_pace_display_frames(1, 2777778, period_90hz) == 1);    // no limit (360)
    CHECK(pc_vr_pace_display_frames(2, 0, period_90hz) == 2);          // the limiter off: the least
    CHECK(pc_vr_pace_display_frames(0, 0, period_90hz) == 1);
    CHECK(pc_vr_pace_display_frames(1, 16666667, period_72hz) == 2);   // 60 at 72 Hz: 36
    CHECK(pc_vr_pace_display_frames(3, 16666667, period_72hz) == 3);
    CHECK(pc_vr_pace_display_frames(1, 22222222 + 100000, period_90hz) == 2);  // within 1% of a display period over two
    CHECK(pc_vr_pace_display_frames(1, 22222222 + 200000, period_90hz) == 3);
    CHECK(pc_vr_pace_display_frames(2, 16666667, 0) == 2);             // no display period: the least
    CHECK(pc_vr_pace_display_frames(1, UINT64_MAX / 2, 1) == 1000);    // (bounded)
}

// A game whose frame takes work_ns, from a start the pace gave: the frame after it, as pc_vr_pace.c asks.
typedef struct {
    uint64_t start_ns;   // the frame's start on the grid
    uint64_t begun_ns;   // when the game really began it (after the wait, or at once when late)
} PacedFrame;

static PacedFrame next_frame(const PcVrDisplayClock *clock, uint64_t offset_ns, uint64_t game_period_ns,
                             PacedFrame previous, uint64_t work_ns) {
    const uint64_t now_ns = previous.begun_ns + work_ns;
    PacedFrame frame;
    frame.start_ns = pc_vr_pace_frame_start(clock, offset_ns, game_period_ns, previous.start_ns, now_ns);
    frame.begun_ns = frame.start_ns > now_ns ? frame.start_ns : now_ns;
    return frame;
}

static void a_fast_game_keeps_the_grid(void) {
    const uint64_t offsets[] = {0, 2500000, 9 * MILLISECOND, 13 * MILLISECOND};  // (the last wraps past one period)
    for (unsigned offset_index = 0; offset_index < 4; ++offset_index) {
        const uint64_t offset_ns = offsets[offset_index];
        const PcVrDisplayClock clock = {.frame_start_ns = 5000 * MILLISECOND, .period_ns = period_90hz};
        const uint64_t game_period_ns = 2 * period_90hz;
        // The first frame, with no previous one: a grid point no more than half a period ahead, or the one now is in.
        const uint64_t first_now_ns = 5007 * MILLISECOND;
        PacedFrame frame = {.start_ns = pc_vr_pace_frame_start(&clock, offset_ns, game_period_ns, 0, first_now_ns)};
        CHECK(on_grid(&clock, offset_ns, frame.start_ns));
        CHECK(frame.start_ns + period_90hz > first_now_ns && frame.start_ns <= first_now_ns + period_90hz / 2);
        frame.begun_ns = frame.start_ns > first_now_ns ? frame.start_ns : first_now_ns;
        // 18 ms of work in a 22.2 ms period: every frame starts exactly two display periods after the last, and waits
        // for it once the first frame's late beginning (when its grid point was the one behind it) is made up.
        for (unsigned count = 0; count < 500; ++count) {
            const PacedFrame following = next_frame(&clock, offset_ns, game_period_ns, frame, 18 * MILLISECOND);
            CHECK(following.start_ns == frame.start_ns + game_period_ns);
            CHECK(on_grid(&clock, offset_ns, following.start_ns));
            if (count >= 2) CHECK(following.begun_ns == following.start_ns);
            frame = following;
        }
    }
}

// The display clock as the XR owner republishes it: the latest display frame's start, each seen up to 0.5 ms early or
// late. The frames stay two display periods apart to within that jitter.
static void a_jittering_display_clock(void) {
    const uint64_t first_display_frame_ns = 2000 * MILLISECOND, offset_ns = 2500000, game_period_ns = 2 * period_90hz;
    unsigned random_state = 12345;
    PcVrDisplayClock clock = {.frame_start_ns = first_display_frame_ns, .period_ns = period_90hz};
    PacedFrame frame = {.start_ns = first_display_frame_ns + offset_ns, .begun_ns = first_display_frame_ns + offset_ns};
    for (unsigned count = 0; count < 2000; ++count) {
        const uint64_t now_ns = frame.begun_ns + 17 * MILLISECOND;
        random_state = random_state * 1103515245u + 12345u;
        const int64_t jitter_ns = (int64_t)(random_state >> 8) % 1000001 - 500000;
        const uint64_t display_frames_so_far = (now_ns - first_display_frame_ns) / period_90hz;
        clock.frame_start_ns = (uint64_t)((int64_t)(first_display_frame_ns + display_frames_so_far * period_90hz) + jitter_ns);
        const PacedFrame following = next_frame(&clock, offset_ns, game_period_ns, frame, 17 * MILLISECOND);
        const int64_t step_error_ns = (int64_t)(following.start_ns - frame.start_ns) - (int64_t)game_period_ns;
        CHECK(step_error_ns >= -1000000 && step_error_ns <= 1000000);
        CHECK(on_grid(&clock, offset_ns, following.start_ns));
        frame = following;
    }
}

// 24 ms of work against a 22.2 ms period: the game is slower than its pace, never waits, and its starts follow the
// real time on the grid (always the grid point the moment falls in).
static void a_slow_game_never_waits(void) {
    const PcVrDisplayClock clock = {.frame_start_ns = 900 * MILLISECOND, .period_ns = period_90hz};
    const uint64_t offset_ns = 2500000, game_period_ns = 2 * period_90hz;
    PacedFrame frame = {.start_ns = clock.frame_start_ns + offset_ns, .begun_ns = clock.frame_start_ns + offset_ns};
    unsigned three_period_steps = 0;
    for (unsigned count = 0; count < 1000; ++count) {
        const PacedFrame following = next_frame(&clock, offset_ns, game_period_ns, frame, 24 * MILLISECOND);
        CHECK(following.begun_ns == frame.begun_ns + 24 * MILLISECOND);  // no wait
        CHECK(following.start_ns <= following.begun_ns && following.begun_ns - following.start_ns < period_90hz);
        CHECK(on_grid(&clock, offset_ns, following.start_ns));
        const uint64_t step_ns = following.start_ns - frame.start_ns;
        CHECK(step_ns == 2 * period_90hz || step_ns == 3 * period_90hz);
        three_period_steps += step_ns == 3 * period_90hz;
        frame = following;
    }
    // 24 ms a frame is 2.16 display periods: 16 frames in a hundred take three.
    CHECK(three_period_steps >= 150 && three_period_steps <= 170);
}

static void a_hitch_and_the_frames_after_it(void) {
    const PcVrDisplayClock clock = {.frame_start_ns = 3000 * MILLISECOND, .period_ns = period_90hz};
    const uint64_t offset_ns = 4 * MILLISECOND, game_period_ns = 2 * period_90hz;
    PacedFrame frame = {.start_ns = clock.frame_start_ns + offset_ns, .begun_ns = clock.frame_start_ns + offset_ns};
    PacedFrame after_hitch = next_frame(&clock, offset_ns, game_period_ns, frame, 1500 * MILLISECOND);
    CHECK(after_hitch.begun_ns == frame.begun_ns + 1500 * MILLISECOND);
    CHECK(after_hitch.start_ns <= after_hitch.begun_ns && after_hitch.begun_ns - after_hitch.start_ns < period_90hz);
    CHECK(on_grid(&clock, offset_ns, after_hitch.start_ns));
    // The next frame, quick again, waits for the grid two periods after the hitch's grid point.
    const PacedFrame recovered = next_frame(&clock, offset_ns, game_period_ns, after_hitch, 5 * MILLISECOND);
    CHECK(recovered.start_ns == after_hitch.start_ns + game_period_ns && recovered.begun_ns == recovered.start_ns);
}

// A frame up to a display period late keeps its grid point and starts at once; the one after it is back on time.
static void a_slightly_late_frame(void) {
    const PcVrDisplayClock clock = {.frame_start_ns = 700 * MILLISECOND, .period_ns = period_90hz};
    const uint64_t offset_ns = 0, game_period_ns = 2 * period_90hz;
    const PacedFrame frame = {.start_ns = clock.frame_start_ns, .begun_ns = clock.frame_start_ns};
    const PacedFrame late = next_frame(&clock, offset_ns, game_period_ns, frame, game_period_ns + 3 * MILLISECOND);
    CHECK(late.start_ns == frame.start_ns + game_period_ns && late.begun_ns == late.start_ns + 3 * MILLISECOND);
    const PacedFrame on_time = next_frame(&clock, offset_ns, game_period_ns, late, 15 * MILLISECOND);
    CHECK(on_time.start_ns == late.start_ns + game_period_ns && on_time.begun_ns == on_time.start_ns);
}

// The display frame the clock names can lie after the time asked about (the XR owner published a newer frame while
// the game read its own clock): grid points run both ways from it.
static void a_display_clock_ahead_of_now(void) {
    const PcVrDisplayClock clock = {.frame_start_ns = 8000 * MILLISECOND, .period_ns = period_72hz};
    const uint64_t offset_ns = 2500000, now_ns = 7950 * MILLISECOND;
    const uint64_t start_ns = pc_vr_pace_frame_start(&clock, offset_ns, 2 * period_72hz, 0, now_ns);
    CHECK(on_grid(&clock, offset_ns, start_ns));
    CHECK(start_ns + period_72hz > now_ns && start_ns <= now_ns + period_72hz / 2);
    CHECK(pc_vr_pace_grid_floor(-5, 0, 10) == -10 && pc_vr_pace_grid_floor(-10, 0, 10) == -10);
    CHECK(pc_vr_pace_grid_floor(5, 0, 10) == 0 && pc_vr_pace_grid_floor(10, 0, 10) == 10);
}

int main(void) {
    display_frames_for_a_limit();
    a_fast_game_keeps_the_grid();
    a_jittering_display_clock();
    a_slow_game_never_waits();
    a_hitch_and_the_frames_after_it();
    a_slightly_late_frame();
    a_display_clock_ahead_of_now();
    printf("PC VR pace fixture passed (%u checks)\n", checks);
    return 0;
}
