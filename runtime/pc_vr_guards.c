// PC VR: refusal counts on the eye path. What they are for and the reasons: pc_vr_guards.h.
#include "pc_vr_guards.h"

#include <stdio.h>

#include "rt.h"

_Atomic uint64_t pc_vr_guard_counts[PC_VR_GUARD_COUNT];

static const char *const pc_vr_guard_names[PC_VR_GUARD_COUNT] = {
    "view-not-fresh", "view-no-pose", "view-pair", "view-placement", "write-failed", "fov-precondition", "traversal",
    "certificate-expired", "frame-uncertified", "frame-misplaced", "frame-menu-without-pose",
    "offer-contract", "offer-panel", "offer-hud", "offer-stopped", "offer-local", "offer-mailbox",
};

const char *pc_vr_guard_name(PcVrGuard guard) {
    return guard < PC_VR_GUARD_COUNT ? pc_vr_guard_names[guard] : "?";
}

void pc_vr_guards_log(const char *when) {
    char text[2048];
    size_t used = 0;
    uint64_t total = 0;
    for (unsigned i = 0; i < PC_VR_GUARD_COUNT; ++i) {
        const uint64_t count = atomic_load_explicit(&pc_vr_guard_counts[i], memory_order_relaxed);
        total += count;
        if (count && used + 64 < sizeof text)
            used += (size_t)snprintf(text + used, sizeof text - used, " %s=%llu", pc_vr_guard_names[i],
                                     (unsigned long long)count);
    }
    text[used] = 0;
    LOG("[pc-vr-guards] at=%s total=%llu%s\n", when, (unsigned long long)total, text);
}
