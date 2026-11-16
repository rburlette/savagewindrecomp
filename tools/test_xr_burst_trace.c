// The burst trace's detector (runtime/xr/xr_burst_trace.h), asset-free: single missed display periods are no burst;
// a burst is reported once, after it is over, with the second before it and the frames after it; a burst near the
// start or longer than the rows allowed, a ring that has wrapped and bursts one after another all name rows that are
// still in the ring.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/xr/xr_burst_trace.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "XR burst trace check failed line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static XrBurstTrace trace;
static XrBurstReport report;
static unsigned reports;
static uint64_t clock_ns;

#define DISPLAY_PERIOD_NS 11111111ull

// One iteration with this many missed display periods; its frame_id is its iteration number plus one. The clock
// moves a display period for an iteration on the cadence and one more for each period it missed.
static bool add(uint32_t missed) {
    clock_ns += DISPLAY_PERIOD_NS * (1 + missed);
    const XrBurstFrame frame = {.frame_id = trace.count + 1, .missed = missed, .wait_began_ns = clock_ns,
        .wait_ns = (uint32_t)(DISPLAY_PERIOD_NS * (1 + missed)) - 1500000, .work_ns = 900000, .lock_ns = 50000,
        .fence_ns = 400000,
        .end_ns = 300000, .completion_ns = 600000, .should_render = true, .image_submitted = true,
        .new_image = trace.count % 2 == 0, .game_present_id = trace.count % 2 ? 0 : 5000 + trace.count / 2,
        .counters = {1.5f, NAN, (float)missed}};
    const bool reported = xr_burst_trace_add(&trace, &frame, &report);
    if (reported) {
        ++reports;
        CHECK(report.from <= report.to && report.to == trace.count);
        CHECK(report.to - report.from <= XR_BURST_ROWS);
        CHECK(trace.count - report.from <= XR_BURST_RING);  // (every row is still in the ring)
        for (uint64_t iteration = report.from; iteration < report.to; ++iteration)
            CHECK(xr_burst_trace_frame(&trace, iteration)->frame_id == iteration + 1);
    }
    return reported;
}

static void quiet(unsigned iterations) {
    for (unsigned index = 0; index < iterations; ++index) CHECK(!add(0));
}

static void reset(void) {
    trace = (XrBurstTrace){0};
    report = (XrBurstReport){0};
    reports = 0;
    clock_ns = 0;
}

static unsigned occurrences(const char *text, const char *part) {
    unsigned found = 0;
    for (const char *at = strstr(text, part); at; at = strstr(at + 1, part)) ++found;
    return found;
}

static void single_missed_periods_are_no_burst(void) {
    reset();
    for (unsigned round = 0; round < 40; ++round) {
        quiet(99);
        CHECK(!add(1));
    }
    // Five in a window are still under the threshold, however they are spread.
    quiet(XR_BURST_WINDOW);
    for (unsigned index = 0; index < 5; ++index) CHECK(!add(1));
    quiet(XR_BURST_WINDOW);
    CHECK(!add(2));
    CHECK(!add(0));
    CHECK(!add(3));
    quiet(200);
    CHECK(reports == 0 && trace.bursts == 0);
}

static void a_burst_is_written_once_when_it_is_over(void) {
    reset();
    quiet(300);
    for (unsigned index = 0; index < 50; ++index) CHECK(!add(1));  // iterations 300 to 349
    quiet(XR_BURST_TAIL - 1);
    CHECK(add(0));  // the 24th iteration without a missed period
    CHECK(reports == 1 && report.ended && report.burst == 1);
    CHECK(report.first_missed == 300 && report.missed_periods == 50 && report.iterations == 50);
    CHECK(report.from == 300 - XR_BURST_LEAD);
    CHECK(report.to == 350 + XR_BURST_TAIL);
    quiet(500);
    CHECK(reports == 1);
}

static void a_burst_begins_at_its_first_missed_period(void) {
    reset();
    quiet(200);
    CHECK(!add(1));   // iteration 200: the first of the window that reaches the threshold
    quiet(3);
    CHECK(!add(2));
    quiet(3);
    CHECK(!add(3));   // six missed periods within sixteen iterations: the burst is on
    CHECK(trace.state == XR_BURST_ACTIVE && trace.first_missed == 200);
    quiet(XR_BURST_TAIL - 1);
    CHECK(add(0));
    CHECK(report.first_missed == 200 && report.missed_periods == 6 && report.iterations == 9);
    // A missed period seventeen iterations before the rest is not part of it.
    reset();
    quiet(100);
    CHECK(!add(1));
    quiet(XR_BURST_WINDOW);
    for (unsigned index = 0; index < 6; ++index) CHECK(!add(1));
    CHECK(trace.state == XR_BURST_ACTIVE && trace.first_missed == 101 + XR_BURST_WINDOW);
}

static void a_burst_near_the_start_has_a_short_lead(void) {
    reset();
    quiet(10);
    for (unsigned index = 0; index < 8; ++index) CHECK(!add(1));
    quiet(XR_BURST_TAIL - 1);
    CHECK(add(0));
    CHECK(report.from == 0 && report.first_missed == 10 && report.ended);
}

static void a_long_burst_is_written_while_it_goes_on(void) {
    reset();
    quiet(500);
    unsigned added = 0;
    while (!add(1)) ++added;  // rows fill before it ends
    ++added;
    CHECK(reports == 1 && !report.ended);
    CHECK(report.from == 500 - XR_BURST_LEAD && report.to - report.from == XR_BURST_ROWS);
    CHECK(added == XR_BURST_ROWS - XR_BURST_LEAD);
    for (unsigned index = 0; index < 2000; ++index) CHECK(!add(1));  // (the ring wraps under it)
    quiet(XR_BURST_TAIL - 1);
    CHECK(add(0));
    CHECK(reports == 2 && report.ended && report.from == report.to);  // the end: totals only
    CHECK(report.first_missed == 500 && report.missed_periods == added + 2000 && report.iterations == added + 2000);
    CHECK(report.burst == 1 && trace.state == XR_BURST_IDLE);
}

static void bursts_after_the_ring_wrapped(void) {
    reset();
    quiet(5 * XR_BURST_RING + 37);
    for (unsigned burst = 1; burst <= 3; ++burst) {
        const uint64_t first = trace.count;
        for (unsigned index = 0; index < 45; ++index) CHECK(!add(1));
        quiet(XR_BURST_TAIL - 1);
        CHECK(add(0));
        CHECK(report.burst == burst && report.ended && report.first_missed == first);
        CHECK(report.from == first - XR_BURST_LEAD && report.missed_periods == 45);
        quiet(30);  // the next one's lead reaches back into this one's rows
    }
    CHECK(reports == 3 && trace.bursts == 3);
}

// The text: a line of totals, the columns with the counters' names, one row per iteration with its times against
// the burst's first missed period, and an end line; a counter without a value is a dash.
static void a_burst_as_text(void) {
    static char text[192 * 1024];
    reset();
    snprintf(trace.counter_names[0], sizeof trace.counter_names[0], "app/gpu_frametime");
    snprintf(trace.counter_names[1], sizeof trace.counter_names[1], "compositor/spacewarp_mode");
    snprintf(trace.counter_names[2], sizeof trace.counter_names[2], "compositor/dropped_frame_count");
    trace.counter_count = 3;
    quiet(300);
    for (unsigned index = 0; index < 50; ++index) CHECK(!add(1));
    quiet(XR_BURST_TAIL - 1);
    CHECK(add(0));
    const size_t length = xr_burst_trace_format(&trace, &report, text, sizeof text);
    CHECK(length == strlen(text) && length < sizeof text);
    CHECK(strstr(text, "[pc-vr] burst 1: 50 display periods missed over 50 frames from frame 301; rows 164, 90 of them before it\n") == text);
    CHECK(strstr(text, "present | app/gpu_frametime compositor/spacewarp_mode compositor/dropped_frame_count\n"));
    CHECK(occurrences(text, "[pc-vr] burst 1 row ") == XR_BURST_LEAD + 50 + XR_BURST_TAIL);
    CHECK(occurrences(text, "\n") == XR_BURST_LEAD + 50 + XR_BURST_TAIL + 3);
    // The first row: 89 display periods on the cadence and the first missed period's two before it; its own time
    // since the row before is unknown to nobody (the ring still holds that row).
    CHECK(strstr(text, "[pc-vr] burst 1 row -1011.11 11.11 9.61 0.90 0.05 0.40 0.30 0.60 0 1 1 1 5105 | 1.5 - 0\n"));
    // The first missed period's row: at zero, two display periods after the row before, one period missed.
    CHECK(strstr(text, "[pc-vr] burst 1 row 0.00 22.22 20.72 0.90 0.05 0.40 0.30 0.60 1 1 1 1 5150 | 1.5 - 1\n"));
    CHECK(strstr(text, "[pc-vr] burst 1 rows end\n"));
    CHECK(text[length - 1] == '\n');
    // A text that has no room is cut, and still ended.
    char small[2048];
    memset(small, 'x', sizeof small);
    const size_t small_length = xr_burst_trace_format(&trace, &report, small, sizeof small);
    CHECK(small_length == strlen(small) && small_length < sizeof small);
    CHECK(occurrences(small, "[pc-vr] burst 1 row ") > 0 && occurrences(small, "[pc-vr] burst 1 row ") < 30);
    char none[1] = {'x'};
    CHECK(xr_burst_trace_format(&trace, &report, none, sizeof none) == 0 && none[0] == 0);
    // A long burst's end: the totals alone.
    reset();
    quiet(500);
    while (!add(1)) {}
    for (unsigned index = 0; index < 100; ++index) CHECK(!add(1));
    quiet(XR_BURST_TAIL - 1);
    CHECK(add(0));
    xr_burst_trace_format(&trace, &report, text, sizeof text);
    CHECK(!strcmp(text, "[pc-vr] burst 1 over: 410 display periods missed over 410 frames; its rows were written "
                        "while it went on\n"));
}

int main(void) {
    a_burst_as_text();
    single_missed_periods_are_no_burst();
    a_burst_is_written_once_when_it_is_over();
    a_burst_begins_at_its_first_missed_period();
    a_burst_near_the_start_has_a_short_lead();
    a_long_burst_is_written_while_it_goes_on();
    bursts_after_the_ring_wrapped();
    printf("XR burst trace fixture: %u checks passed (no runtime/device/game data)\n", checks);
    return 0;
}
