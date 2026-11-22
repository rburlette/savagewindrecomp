// The late-frame trace's row (runtime/pc_vr_frame_trace.h), asset-free: a frame's stage times as milliseconds from
// its start, a stage that was not reached as a dash, lengths as lengths; and what a count added between two presents
// when the two-second statistics set it back in between.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/pc_vr_frame_trace.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "PC VR frame trace check failed line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

#define MILLISECOND 1000000ull

static void a_whole_frame(void) {
    const uint64_t start = 5000 * MILLISECOND;
    const PcVrFrameRecord record = {.present_id = 12345, .start_ns = start, .slept_ns = 3210000, .on_grid = true,
        .pace_frames = 2, .update_ns = start + 50000, .sync_begin_ns = start + 2100000, .sync_end_ns = start + 7900000,
        .submit_ns = start + 15200000, .queued_ns = start + 15800000, .exec_begin_ns = start + 1200000,
        .exec_ns = 12400000, .present_begin_ns = start + 16000000, .offered_ns = start + 17100000,
        .present_end_ns = start + 17900000, .draws = 5200, .submits = 14, .compiles = 1, .gpu_wait_ns = 6100000,
        .upload_bytes = 2411725, .allocate_ns = 250000};
    char text[320];
    const size_t length = pc_vr_frame_trace_row(&record, text, sizeof text);
    CHECK(length == strlen(text));
    CHECK(!strcmp(text, "[pc-vr] late row 12345 1 2 3.21 | 0.05 2.10 7.90 15.20 15.80 | 1.20 12.40 16.00 17.10 17.90 | "
                        "5200 14 1 6.10 2.3 0.25\n"));
    // The columns' names are as many as a row's values.
    unsigned names = 1, values = 0;
    for (const char *at = PC_VR_FRAME_TRACE_COLUMNS; *at; ++at) names += *at == ' ';
    for (const char *at = strstr(text, "row ") + 4; *at; ++at) values += *at == ' ' || *at == '\n';
    CHECK(names == values);
}

static void stages_not_reached_are_dashes(void) {
    const uint64_t start = 800 * MILLISECOND;
    // A late frame off the grid whose present never came (the run ended), with an update time left from the frame
    // before and no sync wait.
    const PcVrFrameRecord record = {.present_id = 7, .start_ns = start, .update_ns = start - 3 * MILLISECOND,
        .submit_ns = start + 20 * MILLISECOND, .queued_ns = start + 21 * MILLISECOND, .exec_ns = 500000};
    char text[320];
    pc_vr_frame_trace_row(&record, text, sizeof text);
    CHECK(!strcmp(text, "[pc-vr] late row 7 0 0 0.00 | - - - 20.00 21.00 | - 0.50 - - - | 0 0 0 0.00 0.0 0.00\n"));
    // No start at all (the first frame of a run without the pace): nothing can be placed.
    const PcVrFrameRecord unplaced = {.present_id = 1, .queued_ns = 40 * MILLISECOND};
    pc_vr_frame_trace_row(&unplaced, text, sizeof text);
    CHECK(!strcmp(text, "[pc-vr] late row 1 0 0 0.00 | - - - - - | - 0.00 - - - | 0 0 0 0.00 0.0 0.00\n"));
}

static void a_text_without_room_is_cut_and_ended(void) {
    const PcVrFrameRecord record = {.present_id = 99, .start_ns = MILLISECOND, .queued_ns = 9 * MILLISECOND};
    char text[100];
    memset(text, 'x', sizeof text);
    const size_t length = pc_vr_frame_trace_row(&record, text, sizeof text);
    CHECK(length == strlen(text) && length < sizeof text);
    char none[1] = {'x'};
    CHECK(pc_vr_frame_trace_row(&record, none, sizeof none) == 0 && none[0] == 0);
}

static void counts_between_two_presents(void) {
    CHECK(pc_vr_frame_trace_added(5200, 5000) == 200);
    CHECK(pc_vr_frame_trace_added(5000, 5000) == 0);
    CHECK(pc_vr_frame_trace_added(300, 5000) == 300);  // (set back to zero in between: what it holds now)
    CHECK(pc_vr_frame_trace_added(0, 5000) == 0);
}

int main(void) {
    a_whole_frame();
    stages_not_reached_are_dashes();
    a_text_without_room_is_cut_and_ended();
    counts_between_two_presents();
    printf("PC VR frame trace fixture: %u checks passed (no runtime/device/game data)\n", checks);
    return 0;
}
