// Experiment (headset pace): the XR owner's frames around a burst of missed display periods. The timing windows
// (xr_perf.h) say that a window missed 51 display periods, not what came first. This keeps every iteration of the
// frame loop in a ring and names the range to write out once a burst is over: the second before it, the burst, and
// the frames after it. No allocation, locks or clock-domain conversions; the owner thread alone uses it.
#ifndef SAVAGEWIND_XR_BURST_TRACE_H
#define SAVAGEWIND_XR_BURST_TRACE_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum {
    XR_BURST_RING = 1024,       // iterations kept (a power of two)
    XR_BURST_LEAD = 90,         // iterations written from before the burst's first missed period (a second at 90 Hz)
    XR_BURST_TAIL = 24,         // iterations without a missed period that end a burst; written too
    XR_BURST_WINDOW = 16,       // a burst begins when this many iterations in a row hold...
    XR_BURST_MISSED = 6,        // ...at least this many missed periods (single missed periods are no burst)
    XR_BURST_ROWS = 400,        // the most iterations written for one burst; a longer one is written while it goes on
    XR_BURST_COUNTERS = 10,     // the runtime's own counters kept per iteration (XR_META_performance_metrics)
    XR_BURST_COUNTER_NAME = 48  // a counter's name, with its end
};

// One iteration of the frame loop. Times are the host clock's; durations in nanoseconds.
typedef struct {
    uint64_t frame_id;
    uint64_t game_present_id;   // the game present whose tracking sample this iteration published (0: none)
    uint64_t wait_began_ns;     // when xrWaitFrame was called
    int64_t predicted_display_time;  // as the runtime gave it (its own clock)
    uint32_t wait_ns;           // xrWaitFrame, call to return
    uint32_t work_ns;           // xrWaitFrame's return to the xrEndFrame call: the lock, begin, locate, publish, the eye copy
    uint32_t end_ns;            // xrEndFrame, call to return
    uint32_t completion_ns;     // the queue completion wait after it
    uint32_t lock_ns;           // within work_ns: until the owner had xr_vulkan_lock (the game holds it for a device idle)
    uint32_t fence_ns;          // within work_ns: the owner's waits on an eye copy's fence
    uint32_t missed;            // display periods the predicted time skipped since the iteration before
    bool should_render, image_submitted, new_image;
    float counters[XR_BURST_COUNTERS];  // NaN: no value this iteration
} XrBurstFrame;

typedef struct {
    XrBurstFrame ring[XR_BURST_RING];
    uint64_t count;         // iterations added so far; iteration n is ring[n % XR_BURST_RING]
    uint64_t first_missed;  // the iteration holding the burst's first missed period
    uint64_t last_missed;   // the latest iteration with a missed period
    uint64_t missed_periods;  // display periods missed in the burst so far
    uint64_t rows_from;     // the first iteration to write
    enum { XR_BURST_IDLE, XR_BURST_ACTIVE, XR_BURST_WRITTEN } state;
    unsigned bursts;        // bursts seen so far
    // The counters' names, as the columns after the bar (set once by the owner of the runtime's counters).
    char counter_names[XR_BURST_COUNTERS][XR_BURST_COUNTER_NAME];
    unsigned counter_count;
} XrBurstTrace;

// What to write: iterations [from, to) (none when from == to: a long burst's rows were written while it went on).
// ended: the burst is over, and missed_periods and iterations count all of it.
typedef struct {
    uint64_t from, to, first_missed, missed_periods, iterations;
    unsigned burst;
    bool ended;
} XrBurstReport;

static inline const XrBurstFrame *xr_burst_trace_frame(const XrBurstTrace *trace, uint64_t iteration) {
    return &trace->ring[iteration % XR_BURST_RING];
}

// Adds one iteration. True when there is something to write: format it before the next call. A burst begins once
// XR_BURST_WINDOW iterations in a row hold XR_BURST_MISSED missed periods, at the first of them that missed one, and
// ends after XR_BURST_TAIL iterations without one.
static inline bool xr_burst_trace_add(XrBurstTrace *trace, const XrBurstFrame *frame, XrBurstReport *report) {
    const uint64_t iteration = trace->count++;
    trace->ring[iteration % XR_BURST_RING] = *frame;
    if (trace->state == XR_BURST_IDLE) {
        if (!frame->missed) return false;
        const uint64_t window_from = iteration + 1 > XR_BURST_WINDOW ? iteration + 1 - XR_BURST_WINDOW : 0;
        uint64_t missed_periods = 0, first_missed = iteration;
        for (uint64_t earlier = iteration + 1; earlier-- > window_from;) {
            const uint32_t missed = xr_burst_trace_frame(trace, earlier)->missed;
            if (!missed) continue;
            missed_periods += missed;
            first_missed = earlier;
        }
        if (missed_periods < XR_BURST_MISSED) return false;
        const uint64_t oldest_kept = trace->count > XR_BURST_RING ? trace->count - XR_BURST_RING : 0;
        trace->state = XR_BURST_ACTIVE;
        trace->first_missed = first_missed;
        trace->last_missed = iteration;
        trace->missed_periods = missed_periods;
        trace->rows_from = first_missed > XR_BURST_LEAD ? first_missed - XR_BURST_LEAD : 0;
        if (trace->rows_from < oldest_kept) trace->rows_from = oldest_kept;
        ++trace->bursts;
        return false;
    }
    if (frame->missed) {
        trace->last_missed = iteration;
        trace->missed_periods += frame->missed;
    }
    const bool ended = iteration - trace->last_missed >= XR_BURST_TAIL;
    const bool rows_full = trace->state == XR_BURST_ACTIVE && iteration + 1 - trace->rows_from >= XR_BURST_ROWS;
    if (!ended && !rows_full) return false;
    *report = (XrBurstReport){.from = trace->rows_from, .to = iteration + 1, .first_missed = trace->first_missed,
        .missed_periods = trace->missed_periods, .iterations = trace->last_missed + 1 - trace->first_missed,
        .burst = trace->bursts, .ended = ended};
    if (trace->state == XR_BURST_WRITTEN) report->from = report->to;  // (its rows are out already)
    trace->state = ended ? XR_BURST_IDLE : XR_BURST_WRITTEN;
    return true;
}

// The report as log lines in text (never beyond size, always ended; what does not fit is left out): a line of
// totals, the columns, then one row per iteration, the runtime's counters after the bar. Times are milliseconds: a
// row's first column is its xrWaitFrame call against that of the burst's first missed period, its second the time
// since the row before; the lock and the fence are parts of the work. A report without rows (a long burst's end) is
// the totals alone. Returns the text's length.
static inline size_t xr_burst_trace_format(const XrBurstTrace *trace, const XrBurstReport *report, char *text, size_t size) {
    size_t length = 0;
    if (size) text[0] = 0;
#define XR_BURST_TEXT(...) do { \
    if (size > length && size - length > 512) length += (size_t)snprintf(text + length, size - length, __VA_ARGS__); \
} while (0)
    if (report->from == report->to) {
        XR_BURST_TEXT("[pc-vr] burst %u over: %llu display periods missed over %llu frames; its rows were written "
                      "while it went on\n", report->burst, (unsigned long long)report->missed_periods,
                      (unsigned long long)report->iterations);
        return length;
    }
    const XrBurstFrame *first_missed = xr_burst_trace_frame(trace, report->first_missed);
    XR_BURST_TEXT("[pc-vr] burst %u: %llu display periods missed over %llu frames from frame %llu%s; rows %llu, %llu "
                  "of them before it\n", report->burst, (unsigned long long)report->missed_periods,
                  (unsigned long long)report->iterations, (unsigned long long)first_missed->frame_id,
                  report->ended ? "" : " and going on", (unsigned long long)(report->to - report->from),
                  (unsigned long long)(report->first_missed - report->from));
    XR_BURST_TEXT("[pc-vr] burst %u columns: at-ms since-ms wait-ms work-ms lock-ms fence-ms end-ms idle-ms missed render "
                  "image new present |", report->burst);
    for (unsigned index = 0; index < trace->counter_count; ++index) XR_BURST_TEXT(" %s", trace->counter_names[index]);
    XR_BURST_TEXT("\n");
    for (uint64_t iteration = report->from; iteration < report->to; ++iteration) {
        const XrBurstFrame *row = xr_burst_trace_frame(trace, iteration);
        const bool has_before = iteration && trace->count - iteration < XR_BURST_RING;
        const uint64_t before_ns = has_before ? xr_burst_trace_frame(trace, iteration - 1)->wait_began_ns : row->wait_began_ns;
        XR_BURST_TEXT("[pc-vr] burst %u row %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %u %d %d %d %llu |", report->burst,
                      ((double)row->wait_began_ns - (double)first_missed->wait_began_ns) / 1e6,
                      (double)(row->wait_began_ns - before_ns) / 1e6, row->wait_ns / 1e6, row->work_ns / 1e6,
                      row->lock_ns / 1e6, row->fence_ns / 1e6, row->end_ns / 1e6, row->completion_ns / 1e6, row->missed,
                      row->should_render, row->image_submitted, row->new_image,
                      (unsigned long long)row->game_present_id);
        for (unsigned index = 0; index < trace->counter_count; ++index) {
            if (isnan(row->counters[index])) XR_BURST_TEXT(" -");
            else XR_BURST_TEXT(" %.3g", row->counters[index]);
        }
        XR_BURST_TEXT("\n");
    }
    XR_BURST_TEXT("[pc-vr] burst %u rows end\n", report->burst);
#undef XR_BURST_TEXT
    return length;
}
#endif
