// PC VR experiment (headset pace): where a late game frame's time went. The XR owner knows which images came late: one
// that follows an image shown for three display frames or more missed its own display frame. This keeps each game
// frame's stage times, by its present, and writes the late frame's and the three before it when the XR owner names
// one. With the timing windows only (SAVAGEWIND_PC_VR_PERF). The frames of the bursts are xr/xr_burst_trace.h's; these
// are the scattered single late frames.
//
// A frame's stages, on two threads. The game's: the frame starts (the pace's grid point, or at once when late), its
// update begins, it waits on a sync (the GPU's work of the frame before), hands its last command list and then its
// present to the queue. The queue's: the frame's command lists run, the present is taken, the eye pair is offered to
// the XR owner, the present ends. All times are the platform clock's.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PC_VR_FRAME_TRACE_RING = 256,  // game frames kept, by present
    PC_VR_FRAME_TRACE_ROWS = 4,    // rows written for a late image: its own frame last
    PC_VR_FRAME_TRACE_CAP = 6000   // late images written in a run
};

typedef struct {
    uint64_t present_id;     // 0: none
    // the game's thread
    uint64_t start_ns;       // when the frame started
    uint64_t slept_ns;       // the pace's sleep before it
    bool on_grid;            // it waited for its grid point (a late one did not)
    unsigned pace_frames;    // the display frames the pace gave the frame before it (0: no pace): how long that
                             // frame's image is meant to stay up
    uint64_t update_ns;      // the game's frame update began
    uint64_t sync_begin_ns, sync_end_ns;  // its last wait on a sync
    uint64_t submit_ns;      // its last command list went to the queue
    uint64_t queued_ns;      // its present went to the queue
    // the queue's thread
    uint64_t exec_begin_ns;  // the first of its command lists began to run
    uint64_t exec_ns;        // running them, together
    uint64_t present_begin_ns, offered_ns, present_end_ns;
    // what the queue's thread did within the frame
    uint64_t draws, submits, compiles;
    uint64_t gpu_wait_ns, upload_bytes, allocate_ns;
} PcVrFrameRecord;

#define PC_VR_FRAME_TRACE_COLUMNS \
    "present grid pace slept | update sync-begin sync-end submit queued | exec-begin exec present-begin offered present-end | " \
    "draws submits compiles gpu-wait upload-mb allocate"

// One frame as a row: times in milliseconds from its start (a stage that was not reached is a dash), the pace's sleep
// and the durations (exec, the GPU waits, allocating video memory) as lengths. Never beyond size, always ended.
// Returns the text's length.
static inline size_t pc_vr_frame_trace_row(const PcVrFrameRecord *record, char *text, size_t size) {
    size_t length = 0;
    if (size) text[0] = 0;
#define PC_VR_FRAME_TEXT(...) do { \
    if (size > length && size - length > 64) length += (size_t)snprintf(text + length, size - length, __VA_ARGS__); \
} while (0)
#define PC_VR_FRAME_TIME(stage_ns) do { /* (a time from before the frame's start is an earlier frame's: not reached) */ \
    if (record->start_ns && (stage_ns) >= record->start_ns) \
        PC_VR_FRAME_TEXT(" %.2f", (double)((stage_ns) - record->start_ns) / 1e6); \
    else PC_VR_FRAME_TEXT(" -"); \
} while (0)
    PC_VR_FRAME_TEXT("[pc-vr] late row %llu %d %u %.2f |", (unsigned long long)record->present_id, record->on_grid,
                     record->pace_frames, record->slept_ns / 1e6);
    PC_VR_FRAME_TIME(record->update_ns);
    PC_VR_FRAME_TIME(record->sync_begin_ns);
    PC_VR_FRAME_TIME(record->sync_end_ns);
    PC_VR_FRAME_TIME(record->submit_ns);
    PC_VR_FRAME_TIME(record->queued_ns);
    PC_VR_FRAME_TEXT(" |");
    PC_VR_FRAME_TIME(record->exec_begin_ns);
    PC_VR_FRAME_TEXT(" %.2f", record->exec_ns / 1e6);
    PC_VR_FRAME_TIME(record->present_begin_ns);
    PC_VR_FRAME_TIME(record->offered_ns);
    PC_VR_FRAME_TIME(record->present_end_ns);
    PC_VR_FRAME_TEXT(" | %llu %llu %llu %.2f %.1f %.2f\n", (unsigned long long)record->draws,
                     (unsigned long long)record->submits, (unsigned long long)record->compiles,
                     record->gpu_wait_ns / 1e6, record->upload_bytes / 1048576.0, record->allocate_ns / 1e6);
#undef PC_VR_FRAME_TIME
#undef PC_VR_FRAME_TEXT
    return length;
}

// A count that only grows, read at two presents: what the frame between them added. One that was set back to zero in
// between (the two-second statistics take some) gives what it holds now.
static inline uint64_t pc_vr_frame_trace_added(uint64_t now, uint64_t before) {
    return now >= before ? now - before : now;
}

// ---- The process's trace (runtime/pc_vr_frame_trace.c). Each call is a no-op without the timing windows.

bool pc_vr_frame_trace_on(void);
// The game's thread. The next frame starts (pc_vr_pace.c, after its wait), display_frames after the one before it.
void pc_vr_frame_trace_started(uint64_t started_ns, uint64_t slept_ns, bool on_grid, unsigned display_frames);
// The game's thread, in the frame: a wait on a sync begins and ends; a command list goes to the queue.
void pc_vr_frame_trace_sync(bool ended);
void pc_vr_frame_trace_submitted(void);
// The game's thread: the frame's present goes to the queue, with the identity that names the frame from here on.
void pc_vr_frame_trace_queued(uint64_t present_id);
// The queue's thread: one of the next present's command lists ran, from began_ns to now.
void pc_vr_frame_trace_executed(uint64_t began_ns);
// The queue's thread: the present is taken; its eye pair was offered, with the thread's counts so far (ever-growing
// ones, and the statistics' own that are set back every two seconds); the present ended.
void pc_vr_frame_trace_present_begin(uint64_t present_id);
void pc_vr_frame_trace_offered(uint64_t present_id, uint64_t draws, uint64_t submits, uint64_t compiles,
                               uint64_t gpu_wait_ns, uint64_t upload_bytes, uint64_t allocate_ns);
void pc_vr_frame_trace_present_end(uint64_t present_id);
// The XR owner: the image of this present came after the image before it was shown for `shown` display frames. Late,
// and written, when that is more than the pace gave it (the menus' 30 frames a second are three display frames each).
// What the owner saw of it: unfinished, the display frames at which the pair was there but the GPU had not finished
// the frame; copy_carried, the owner's copy of it not done within its display frame; spare_ns, how long before the
// display frame that took it the pair was offered (0: after it began). A pair with neither was offered too late: the
// queue's thread was still running its lists.
void pc_vr_frame_trace_late(uint64_t present_id, unsigned shown, unsigned unfinished, bool copy_carried, uint64_t spare_ns);
// The finish watcher's thread (nvn/vulkan/vr_eyes.c): the GPU finished the present's eye pair at finished_ns, as a
// wait on the renderer's timeline saw it (the wake-up of that wait included, some tenths of a millisecond). Written
// for every offered frame as "[pc-vr] finish <present> <on grid> <lists ms> <offered ms> <submitted ms> <finished ms>",
// the last three from the frame's start: what a late frame's row cannot say, when each frame's GPU work was done.
// submitted_ns: when the watcher saw that the driver had the pair's submission (0: not known), which splits the time
// after the offer into the hand-over to the driver and the driver's and the GPU's own.
void pc_vr_frame_trace_gpu_finished(uint64_t present_id, uint64_t submitted_ns, uint64_t finished_ns);
// The thread that runs the game's update, as it begins one (hle/framepace.c rt_vfr_update): the stall sampler's.
void pc_vr_frame_trace_update(void);
// The user's mark, "I saw a hitch just now": a press of the controller's Capture / Share / Mute button (hle/input.c;
// the game never uses it, and upstream gives it to fly mode in control-endpoint runs only). Written with the present
// it came after, so that what was seen can be laid beside the trace; a person's press comes about half a second after
// what they saw.
void pc_vr_frame_trace_user_mark(void);

// ---- The stall sampler (runtime/pc_vr_stall_sampler.c, Windows only): while a frame stalls (no stage of it moving,
// outside a sync wait), what the thread of the game's update and the presenting thread are in, read every couple of
// milliseconds, and once in each stall the game's other threads, written as "[pc-vr] stall sample" lines. Called by
// the trace above.
void pc_vr_stall_sampler_register(bool presenting);  // the calling thread takes the role, the first one to ask
void pc_vr_stall_sampler_frame(uint64_t start_ns, uint64_t after_present_id);  // a frame starts: its present is due
void pc_vr_stall_sampler_moved(void);  // a stage of the frame has moved: its update begins, its lists are submitted
void pc_vr_stall_sampler_gpu_wait(bool waiting);  // a sync wait begins or ends: waiting on the GPU is no stall
void pc_vr_stall_sampler_presented(void);  // its present has come

#ifdef __cplusplus
}
#endif
