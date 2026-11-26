// Part of dispatch.c (D133): PC VR's control endpoint commands and the data-watch audit.
// Included where it stands by dispatch.c, so it compiles in dispatch.c's translation unit as before.

// [pc-vr-audit] A CPU data breakpoint's access (runtime/platform/windows.c plat_data_watch): the first one from each
// slot, host pc and guest call site (lr) is logged with its guest call chain; hits per slot and per site each second.
// (probe) The site table: 4096 sites, started afresh by each `watch` command (data_watch_sites_reset), so one run can
// list the readers of several sets of words; each window line also carries a per-site total at the end of a set.
#define DATA_WATCH_SITES 4096
static atomic_flag data_watch_lock = ATOMIC_FLAG_INIT;
static struct { uint64_t pc, lr, hits, total; unsigned slot; } data_watch_sites[DATA_WATCH_SITES];
static int data_watch_site_count, data_watch_set;

static void data_watch_sites_reset(void) {
    while (atomic_flag_test_and_set_explicit(&data_watch_lock, memory_order_acquire)) {}
    fprintf(stderr, "[pc-vr-audit] watch set=%d totals", data_watch_set);
    for (int k = 0; k < data_watch_site_count; k++)
        fprintf(stderr, " %d:%llu", k, (unsigned long long)data_watch_sites[k].total);
    fprintf(stderr, "\n");
    memset(data_watch_sites, 0, sizeof data_watch_sites);
    data_watch_site_count = 0;
    data_watch_set++;
    atomic_flag_clear_explicit(&data_watch_lock, memory_order_release);
}

void rt_data_watch_hit(unsigned slot, uint64_t pc, const uint64_t regs[4]) {
#define lock data_watch_lock
#define sites data_watch_sites
#define nsites data_watch_site_count
    static uint64_t window_ns, window_hits[4];
    const Cpu *cpu = kernel_current_cpu();
    const uint64_t lr = cpu ? cpu->x[30] : 0, now = plat_now_ns();
    while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {}
    window_hits[slot & 3]++;
    int i = 0;
    while (i < nsites && !(sites[i].pc == pc && sites[i].lr == lr && sites[i].slot == slot)) i++;
    if (i == nsites && nsites < DATA_WATCH_SITES) {
        sites[nsites].pc = pc;
        sites[nsites].lr = lr;
        sites[nsites++].slot = slot;
        uint64_t offset = 0;
        const char *name = rt_host_fn_name(pc, &offset);
        if (!name) name = plat_symbol(pc, &offset);
        if (!name) name = "savagewind rt_fault_watch", offset = pc - (uint64_t)(uintptr_t)&rt_fault_watch;
        extern uint64_t renderer_present_count(void);
        fprintf(stderr, "[pc-vr-audit] watch set=%d site=%d slot=%u by %s+%llx present=%llu (thread %x) guest x0-x2 %llx %llx "
                "%llx host rax rcx rdx r8 %llx %llx %llx %llx\n", data_watch_set, i, slot, name, (unsigned long long)offset,
                (unsigned long long)renderer_present_count(), kernel_current_thread_handle(),
                (unsigned long long)(cpu ? cpu->x[0] : 0), (unsigned long long)(cpu ? cpu->x[1] : 0),
                (unsigned long long)(cpu ? cpu->x[2] : 0), (unsigned long long)regs[0], (unsigned long long)regs[1],
                (unsigned long long)regs[2], (unsigned long long)regs[3]);
        extern void kernel_log_guest_chain(void);
        kernel_log_guest_chain();
    }
    if (i < nsites) sites[i].hits++, sites[i].total++;
    if (!window_ns) window_ns = now;
    if (now - window_ns >= 1000000000ull) {
        extern uint64_t renderer_present_count(void);
        fprintf(stderr, "[pc-vr-audit] watch window set=%d present=%llu hits=%llu,%llu,%llu,%llu sites=%d\n",
                data_watch_set, (unsigned long long)renderer_present_count(), (unsigned long long)window_hits[0],
                (unsigned long long)window_hits[1], (unsigned long long)window_hits[2],
                (unsigned long long)window_hits[3], nsites);
        for (int k = 0; k < nsites; k++) sites[k].hits = 0;
        memset(window_hits, 0, sizeof window_hits);
        window_ns = now;
    }
    atomic_flag_clear_explicit(&lock, memory_order_release);
#undef lock
#undef sites
#undef nsites
}

// [pc-vr-audit] The control endpoint's `watch <address> [<address> ...]` (developer mode, Windows): CPU data breakpoints
// on up to four 8-byte guest words; every read or write of one is reported by rt_data_watch_hit above. `watch` with new
// addresses replaces the earlier ones. Threads started after the command are not covered.
#include "control.h"
static void cmd_watch(int argc, char **argv, ControlReply *reply) {
#ifdef _WIN32
    extern bool plat_data_watch(const uint64_t addr[4]);
    uint64_t addresses[4] = {0};
    if (argc < 2 || argc > 5) {
        control_fail(reply, "Usage: watch <address> [<address> ...] (at most four)");
        return;
    }
    for (int i = 1; i < argc; i++) {
        char *end = NULL;
        addresses[i - 1] = strtoull(argv[i], &end, 0);
        if (!end || *end || addresses[i - 1] < GUEST_LO || addresses[i - 1] >= GUEST_HI) {
            control_fail(reply, "watch: %s is not a guest address", argv[i]);
            return;
        }
    }
    data_watch_sites_reset();
    control_field_bool(reply, "armed", plat_data_watch(addresses));
#else
    (void)argc, (void)argv;
    control_fail(reply, "watch: Windows only");
#endif
}

// (probe) `head <yaw degrees> <pitch degrees>` holds the head turned by that much from the tracked pose, `head off`
// lets it go: on the simulator, the head-turned view without a headset (runtime/xr/xr_runtime.c xr_probe_head_hold).
// Only a build with the XR runtime has a head to turn.
static void cmd_head(int argc, char **argv, ControlReply *reply) {
#ifndef SAVAGEWIND_WITH_OPENXR
    (void)argc, (void)argv;
    control_fail(reply, "head: a PC VR build only");
#else
    extern void xr_probe_head_hold(bool on, double yaw_degrees, double pitch_degrees, const double shift_metres[3]);
    double shift_metres[3] = {0, 0, 0};
    if (argc == 2 && !strcmp(argv[1], "off")) {
        xr_probe_head_hold(false, 0, 0, shift_metres);
        control_field_bool(reply, "held", false);
        return;
    }
    if (argc != 3 && argc != 6) {
        control_fail(reply, "Usage: head <yaw degrees> <pitch degrees> [<right m> <up m> <back m>] | head off");
        return;
    }
    const double yaw = strtod(argv[1], NULL), pitch = strtod(argv[2], NULL);
    for (int axis = 0; axis < 3 && argc == 6; axis++) shift_metres[axis] = strtod(argv[3 + axis], NULL);
    xr_probe_head_hold(true, yaw, pitch, shift_metres);
    control_field_bool(reply, "held", true);
    control_field_num(reply, "yaw", yaw);
    control_field_num(reply, "pitch", pitch);
#endif
}

// (probe) `eyeshot` saves the next frame's two eye images at full size, as the controller's capture button does
// (runtime/pc_vr.h pc_vr_eye_shot_request): build/vr-shot-mark<request>-submit0/1-<present>.png in the profile folder.
static void cmd_eyeshot(int argc, char **argv, ControlReply *reply) {
#ifndef SAVAGEWIND_WITH_OPENXR
    (void)argc, (void)argv;
    control_fail(reply, "eyeshot: a PC VR build only");
#else
    extern void pc_vr_eye_shot_request(void);
    extern unsigned pc_vr_eye_shot_requests(void);
    extern uint64_t renderer_present_count(void);
    (void)argv;
    if (argc != 1) {
        control_fail(reply, "Usage: eyeshot");
        return;
    }
    pc_vr_eye_shot_request();
    control_field_int(reply, "request", (int64_t)pc_vr_eye_shot_requests());
    control_field_int(reply, "frame", (int64_t)renderer_present_count());
#endif
}

void rt_data_watch_control_init(void) {
    static const ControlCmd commands[] = {
        {"watch", "<address> [<address> ...]", "Log the code that reads or writes up to four guest words", cmd_watch},
        {"head", "<yaw degrees> <pitch degrees> [<right m> <up m> <back m>] | off",
         "Hold the head turned, and moved, from the tracked pose (probe)", cmd_head},
        {"eyeshot", "", "Save the next frame's two eye images at full size (probe)", cmd_eyeshot}};
    control_register(commands, 3);
}
