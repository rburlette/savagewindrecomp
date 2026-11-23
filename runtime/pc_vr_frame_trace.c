// PC VR experiment: where a late game frame's time went, the process's side. What it is for: pc_vr_frame_trace.h.
#include "pc_vr_frame_trace.h"

#include <pthread.h>
#include <string.h>

#include "hle.h"
#include "pc_vr_config.h"
#include "platform.h"
#include "rt.h"

// The frames by present. A record is made by the game's thread when its present goes to the queue, completed by the
// queue's thread and read by the XR owner: all three under the lock, which nothing else is taken inside.
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;
static PcVrFrameRecord trace_ring[PC_VR_FRAME_TRACE_RING];
static uint64_t last_written_present;
static unsigned late_images_written;

// The game's thread only: the frame being made, and when the present before it went to the queue.
static struct {
    uint64_t start_ns, slept_ns, sync_begin_ns, sync_end_ns, submit_ns;
    bool on_grid;
    unsigned pace_frames;
} making;
static uint64_t previous_queued_ns, previous_present_id;

// The queue's thread only: the command lists run since the last present, and its counts at the last offer.
static struct {
    uint64_t exec_begin_ns, exec_ns;
    uint64_t draws, submits, compiles, gpu_wait_ns, upload_bytes, allocate_ns;
} queue_side;

RT_ON_RESET(pc_vr_frame_trace_reset) {
    memset(&making, 0, sizeof making);
    previous_queued_ns = 0;
}

bool pc_vr_frame_trace_on(void) {
    static int on = -1;  // (read once; the same answer from any thread)
    if (on < 0) {
        const PcVrConfig *config = pc_vr_config();
        on = config->enabled && config->diagnostics.perf;
    }
    return on > 0;
}

static PcVrFrameRecord *record_of(uint64_t present_id) {
    PcVrFrameRecord *record = &trace_ring[present_id % PC_VR_FRAME_TRACE_RING];
    return present_id && record->present_id == present_id ? record : NULL;
}

void pc_vr_frame_trace_started(uint64_t started_ns, uint64_t slept_ns, bool on_grid, unsigned display_frames) {
    if (!pc_vr_frame_trace_on()) return;
    making.start_ns = started_ns;
    making.slept_ns = slept_ns;
    making.on_grid = on_grid;
    making.pace_frames = display_frames;
    pc_vr_stall_sampler_frame(started_ns, previous_present_id);
}

void pc_vr_frame_trace_update(void) {
    pc_vr_stall_sampler_register(false);
    pc_vr_stall_sampler_moved();
}

void pc_vr_frame_trace_user_mark(void) {
    if (!pc_vr_frame_trace_on()) return;
    static unsigned marks;  // (the input thread alone; the present's number is read as it stands)
    LOG("[pc-vr] user mark %u: after present %llu\n", ++marks, (unsigned long long)previous_present_id);
}

void pc_vr_frame_trace_sync(bool ended) {
    if (!pc_vr_frame_trace_on()) return;
    const uint64_t now_ns = plat_now_ns();
    if (ended) making.sync_end_ns = now_ns;
    else if (!making.sync_begin_ns) making.sync_begin_ns = now_ns;  // (the first begins it, the last ends it)
    pc_vr_stall_sampler_gpu_wait(!ended);
}

void pc_vr_frame_trace_submitted(void) {
    if (!pc_vr_frame_trace_on()) return;
    making.submit_ns = plat_now_ns();
    pc_vr_stall_sampler_moved();
}

void pc_vr_frame_trace_queued(uint64_t present_id) {
    if (!pc_vr_frame_trace_on() || !present_id) return;
    extern uint64_t g_fs_vfr;  // (nvn/objects.c: when the game's frame update began, set in hle/framepace.c)
    const uint64_t now_ns = plat_now_ns();
    pc_vr_stall_sampler_register(true);
    pc_vr_stall_sampler_presented();
    previous_present_id = present_id;
    // Without the pace a frame starts where the present before it went out.
    const PcVrFrameRecord made = {.present_id = present_id,
        .start_ns = making.start_ns ? making.start_ns : previous_queued_ns, .slept_ns = making.slept_ns,
        .on_grid = making.on_grid, .pace_frames = making.pace_frames, .update_ns = g_fs_vfr,
        .sync_begin_ns = making.sync_begin_ns,
        .sync_end_ns = making.sync_end_ns, .submit_ns = making.submit_ns, .queued_ns = now_ns};
    pthread_mutex_lock(&trace_lock);
    trace_ring[present_id % PC_VR_FRAME_TRACE_RING] = made;
    pthread_mutex_unlock(&trace_lock);
    memset(&making, 0, sizeof making);
    previous_queued_ns = now_ns;
}

void pc_vr_frame_trace_executed(uint64_t began_ns) {
    if (!pc_vr_frame_trace_on() || !began_ns) return;
    if (!queue_side.exec_begin_ns) queue_side.exec_begin_ns = began_ns;
    queue_side.exec_ns += plat_now_ns() - began_ns;
}

void pc_vr_frame_trace_present_begin(uint64_t present_id) {
    if (!pc_vr_frame_trace_on()) return;
    const uint64_t now_ns = plat_now_ns();
    pthread_mutex_lock(&trace_lock);
    PcVrFrameRecord *record = record_of(present_id);
    if (record) {
        record->exec_begin_ns = queue_side.exec_begin_ns;
        record->exec_ns = queue_side.exec_ns;
        record->present_begin_ns = now_ns;
    }
    pthread_mutex_unlock(&trace_lock);
    queue_side.exec_begin_ns = queue_side.exec_ns = 0;
}

void pc_vr_frame_trace_offered(uint64_t present_id, uint64_t draws, uint64_t submits, uint64_t compiles,
                               uint64_t gpu_wait_ns, uint64_t upload_bytes, uint64_t allocate_ns) {
    if (!pc_vr_frame_trace_on()) return;
    const uint64_t now_ns = plat_now_ns();
    pthread_mutex_lock(&trace_lock);
    PcVrFrameRecord *record = record_of(present_id);
    if (record) {
        record->offered_ns = now_ns;
        record->draws = pc_vr_frame_trace_added(draws, queue_side.draws);
        record->submits = pc_vr_frame_trace_added(submits, queue_side.submits);
        record->compiles = pc_vr_frame_trace_added(compiles, queue_side.compiles);
        record->gpu_wait_ns = pc_vr_frame_trace_added(gpu_wait_ns, queue_side.gpu_wait_ns);
        record->upload_bytes = pc_vr_frame_trace_added(upload_bytes, queue_side.upload_bytes);
        record->allocate_ns = pc_vr_frame_trace_added(allocate_ns, queue_side.allocate_ns);
    }
    pthread_mutex_unlock(&trace_lock);
    queue_side.draws = draws;
    queue_side.submits = submits;
    queue_side.compiles = compiles;
    queue_side.gpu_wait_ns = gpu_wait_ns;
    queue_side.upload_bytes = upload_bytes;
    queue_side.allocate_ns = allocate_ns;
}

void pc_vr_frame_trace_present_end(uint64_t present_id) {
    if (!pc_vr_frame_trace_on()) return;
    const uint64_t now_ns = plat_now_ns();
    pthread_mutex_lock(&trace_lock);
    PcVrFrameRecord *record = record_of(present_id);
    if (record) record->present_end_ns = now_ns;
    pthread_mutex_unlock(&trace_lock);
}

void pc_vr_frame_trace_gpu_finished(uint64_t present_id, uint64_t finished_ns) {
    if (!pc_vr_frame_trace_on() || !present_id) return;
    pthread_mutex_lock(&trace_lock);
    const PcVrFrameRecord *found = record_of(present_id);
    const PcVrFrameRecord frame = found ? *found : (PcVrFrameRecord){0};
    pthread_mutex_unlock(&trace_lock);
    if (!frame.start_ns || finished_ns < frame.start_ns) return;
    // (the offer's time is written by the queue's thread a moment after the watch begins: -1 when the GPU was first)
    LOG("[pc-vr] finish %llu %d %.3f %.3f %.3f\n", (unsigned long long)present_id, frame.on_grid, frame.exec_ns / 1e6,
        frame.offered_ns >= frame.start_ns ? (double)(frame.offered_ns - frame.start_ns) / 1e6 : -1.0,
        (double)(finished_ns - frame.start_ns) / 1e6);
}

void pc_vr_frame_trace_late(uint64_t present_id, unsigned shown, unsigned unfinished, bool copy_carried, uint64_t spare_ns) {
    if (!pc_vr_frame_trace_on() || !present_id) return;
    char text[(PC_VR_FRAME_TRACE_ROWS + 2) * 320];
    size_t length = 0;
    pthread_mutex_lock(&trace_lock);
    const PcVrFrameRecord *late_frame = record_of(present_id);
    // (on its pace: the image before it was meant to stay up that long)
    const bool on_pace = late_frame && late_frame->pace_frames && shown <= late_frame->pace_frames;
    if (on_pace || late_images_written >= PC_VR_FRAME_TRACE_CAP) {
        pthread_mutex_unlock(&trace_lock);
        return;
    }
    if (!late_images_written++)
        length += (size_t)snprintf(text + length, sizeof text - length, "[pc-vr] late columns: %s\n", PC_VR_FRAME_TRACE_COLUMNS);
    length += (size_t)snprintf(text + length, sizeof text - length,
        "[pc-vr] late image: present %llu, after an image shown for %u display frames; unfinished %u copy-carried %d "
        "spare-ms %.2f\n", (unsigned long long)present_id, shown, unfinished, copy_carried, spare_ns / 1e6);
    // The frames before it that are not written yet, and its own last.
    uint64_t first = present_id >= PC_VR_FRAME_TRACE_ROWS ? present_id - PC_VR_FRAME_TRACE_ROWS + 1 : 1;
    if (first <= last_written_present) first = last_written_present + 1;
    if (first > present_id) first = present_id;
    for (uint64_t id = first; id <= present_id; ++id) {
        const PcVrFrameRecord *record = record_of(id);
        if (record) length += pc_vr_frame_trace_row(record, text + length, sizeof text - length);
    }
    last_written_present = present_id;
    pthread_mutex_unlock(&trace_lock);
    LOG("%s", text);
}
