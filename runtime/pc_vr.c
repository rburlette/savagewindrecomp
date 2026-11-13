// PC VR launch and startup (SAVAGEWIND_PC_VR=1). Interface: runtime/pc_vr.h.
//
// What this file does:
//  - answers the launch settings, from the process's configuration (runtime/pc_vr_config.h);
//  - starts the game straight into its own Labo VR mode (D033 to D035): at the title screen it makes the call the
//    game's Options menu makes when the player confirms "VR mode on", without the game's dialog
//    (pc_vr_startup_tick), then checks that VR mode and the motion sensors became active within 20 seconds
//    (pc_vr_startup_poll); the process stops if they do not, rather than running without VR;
//  - redirects two SDK calls of the game's VR lifecycle, from the game's own call sites, while it is in Labo mode 2
//    (pc_vr_lifecycle_lookup): BeginVrMode in place of EndVrMode, and a reset of the motion sensor's timestamps in
//    place of EnableRecording;
//  - holds the distance settings of the 2D panels (menus and HUD), which can be adjusted live.
#include "pc_vr.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "game_sites.h"
#include "pc_vr_main.h"
#include "hle.h"
#include "rt.h"
#include "hle/labo_guest_memory.h"
#include "hle/pc_vr_motion_contract.h"
#include "services/labo_motion_state.h"

bool pc_vr_enabled(void) { return pc_vr_config()->enabled; }

bool pc_vr_eye_size(unsigned *width, unsigned *height) {
    const PcVrConfig *config = pc_vr_config();
    *width = config->play.eye_width;
    *height = config->play.eye_height;
    return config->play.eye_width != 0;
}

bool pc_vr_main_compatible(void) {
    return pc_vr_main_layout(gamelib_tables(), loader_module("main"));
}

static atomic_bool startup_requested, startup_active_logged;
static atomic_uint_fast64_t startup_seen_at, startup_request_at;
static unsigned lifecycle_redirect_logs;

RT_ON_RESET(pc_vr_startup_reset) {
    atomic_store(&startup_requested, false);
    atomic_store(&startup_active_logged, false);
    atomic_store(&startup_seen_at, 0);
    atomic_store(&startup_request_at, 0);
    lifecycle_redirect_logs = 0;
}

// The game's Labo VR object, through its platform manager. Guest layout (1.8.2 and 1.9.0):
//   [g_site.labo_manager_slot] -> manager slot -> manager; manager + 0x380: the VR object; manager + 0x11be: the
//   Labo mode (uint8); VR object + 0x10: active (uint8, 0 or 1); VR object + 0x300: two pending request flags.
#define MANAGER_VR_OBJECT 0x380
#define MANAGER_LABO_MODE 0x11be
#define VR_ACTIVE 0x10
#define VR_REQUESTS 0x300

// Reads the VR object's address, checking every pointer on the way and reading the chain twice, so a manager being
// replaced meanwhile is not used. False while the platform manager is not ready.
static bool pc_vr_object(uint64_t *out) {
    // The original Options confirmation path dereferences this singleton
    // twice (1.8.2 main+0xd1c9dc/e0), then reads manager+0x380 (in 1.9.0 too).
    uint64_t manager_slot = 0, manager = 0, vr = 0;
    uint64_t manager_slot_check = 0, manager_check = 0, vr_check = 0;
    uint8_t active = 0, request[2] = {0};
    if (!labo_guest_copy(g_site.labo_manager_slot, &manager_slot, sizeof manager_slot, false) ||
        manager_slot < GUEST_LO || manager_slot >= GUEST_HI - sizeof manager ||
        (manager_slot & 7) ||
        !labo_guest_copy(manager_slot, &manager, sizeof manager, false) ||
        manager < GUEST_LO || manager >= GUEST_HI - 0x381 || (manager & 7) ||
        !labo_guest_copy(manager + MANAGER_VR_OBJECT, &vr, sizeof vr, false) ||
        vr < GUEST_LO || vr >= GUEST_HI - 0x302 || (vr & 7) ||
        !labo_guest_copy(vr + VR_ACTIVE, &active, sizeof active, false) ||
        !labo_guest_copy(vr + VR_REQUESTS, request, sizeof request, false) ||
        active > 1 || request[0] > 1 || request[1] > 1 ||
        !labo_guest_copy(g_site.labo_manager_slot, &manager_slot_check,
                         sizeof manager_slot_check, false) ||
        !labo_guest_copy(manager_slot, &manager_check, sizeof manager_check, false) ||
        !labo_guest_copy(manager + MANAGER_VR_OBJECT, &vr_check, sizeof vr_check, false) ||
        manager_slot != manager_slot_check || manager != manager_check || vr != vr_check)
        return false;
    *out = vr;
    return true;
}

// Calls the guest function at address with (x0, x1) on this guest thread, restoring its registers after.
static void pc_vr_call(Cpu *cpu, const Cpu *saved, uint64_t address, uint64_t x0, uint64_t x1) {
    *cpu = *saved;
    cpu->x[0] = x0;
    cpu->x[1] = x1;
    guest_call(cpu, address);
    *cpu = *saved;
}

// Stops the process when VR mode did not become active within 20 seconds of the title screen (or of the request).
void pc_vr_startup_poll(void) {
    if (!pc_vr_enabled()) return;
#ifndef _WIN32
    FATAL("SAVAGEWIND_PC_VR requires native Windows for this prototype\n");
#endif
    uint64_t seen = atomic_load(&startup_seen_at);
    if (!seen) return; // The original title-menu hook has not run yet.
    if (atomic_load(&startup_active_logged)) return;
    if (!pc_vr_main_compatible())
        FATAL("PC VR startup requires a game version with its PC VR sites (tools/game_sites.py)\n");
    uint64_t now = plat_now_ns();
    uint64_t vr;
    uint8_t active = 0;
    if (pc_vr_object(&vr) &&
        labo_guest_copy(vr + VR_ACTIVE, &active, sizeof active, false) &&
        active && labo_motion_active()) {
        if (!atomic_exchange(&startup_active_logged, true))
            LOG("[pc-vr] original VR and sensor lifecycle active\n");
        return;
    }
    bool requested = atomic_load(&startup_requested);
    uint64_t deadline_from = requested ? atomic_load(&startup_request_at) : seen;
    if (now - deadline_from > 20000000000ull) {
        if (requested) FATAL("PC VR original VR/sensor transition did not become active\n");
        FATAL("PC VR original platform manager did not become ready at title\n");
    }
}

// At the title screen: once the VR object exists, request VR mode as the Options menu does.
void pc_vr_startup_tick(void) {
    if (!pc_vr_enabled()) return;
    if (!pc_vr_main_compatible())
        FATAL("PC VR startup requires a game version with its PC VR sites (tools/game_sites.py)\n");
    uint64_t now = plat_now_ns();
    uint64_t expected = 0;
    atomic_compare_exchange_strong(&startup_seen_at, &expected, now);
    pc_vr_startup_poll();
    if (atomic_load(&startup_active_logged) || atomic_load(&startup_requested)) return;
    uint64_t vr;
    if (pc_vr_object(&vr)) {
        Cpu *cpu = kernel_current_cpu();
        if (!cpu) FATAL("PC VR startup has no guest thread context\n");
        Cpu saved = *cpu;
        // Original confirmed-enable branch in Options::update (1.8.2 main+0xd1c9e8):
        // queue the VR-object transition, then notify the two game managers.
        // The prior Nintendo dialog is intentionally omitted for explicit PC VR launch.
        pc_vr_call(cpu, &saved, g_site.labo_set_vr, vr, 1);
        pc_vr_call(cpu, &saved, g_site.labo_vr_notify_1, 1, 0);
        pc_vr_call(cpu, &saved, g_site.labo_vr_notify_2, 1, 0);
        atomic_store(&startup_request_at, now);
        atomic_store(&startup_requested, true);
        LOG("[pc-vr] requested original VR transition at title readiness\n");
    }
}

// At the lifecycle call sites: x19 holds the manager and x21 its mode's address. True in Labo mode 2 with VR on.
bool pc_vr_labo_mode2_tracking(const Cpu *cpu) {
    if (!pc_vr_enabled() || !cpu || !pc_vr_main_compatible()) return false;
    uint64_t manager = cpu->x[19], vr = 0;
    uint8_t mode = 0, enabled = 0;
    if (manager < GUEST_LO || manager >= GUEST_HI - 0x11bf ||
        (manager & 7) || cpu->x[21] != manager + MANAGER_LABO_MODE ||
        !labo_guest_copy(manager + MANAGER_LABO_MODE, &mode, sizeof mode, false) ||
        !labo_guest_copy(manager + MANAGER_VR_OBJECT, &vr, sizeof vr, false) ||
        vr < GUEST_LO || vr >= GUEST_HI - 0x2fd || (vr & 7) ||
        !labo_guest_copy(vr + VR_ACTIVE, &enabled, sizeof enabled, false))
        return false;
    return mode == 2 && enabled != 0;
}

// Calls the SDK function `original`, or `redirect` instead when the call comes from expected_return (the game's own
// call site) in Labo mode 2. Returns whether it redirected.
static bool pc_vr_lifecycle_call(Cpu *cpu, const char *original, const char *redirect,
                                 uint64_t expected_return, const char *label) {
    const uint64_t lr = cpu->x[30];
    bool adapt = lr == expected_return && pc_vr_labo_mode2_tracking(cpu);
    const char *symbol = adapt ? redirect : original;
    uint64_t sdk_addr = loader_find_symbol_in("sdk", symbol);
    if (!sdk_addr) FATAL("PC VR lifecycle SDK export missing (%s)\n", label);
    if (adapt && lifecycle_redirect_logs++ < 8)
        LOG("[pc-vr] pinned mode2 lifecycle: %s\n", label);
    guest_call(cpu, sdk_addr);
    cpu->x[30] = lr;
    return adapt;
}

static void pc_vr_end_vr_mode(Cpu *cpu) {
    pc_vr_lifecycle_call(cpu, "_ZN2nn2pl9EndVrModeEv", "_ZN2nn2pl11BeginVrModeEv",
                         g_site.labo_end_vr_call, "EndVrMode -> BeginVrMode");
}

static void pc_vr_enable_recording(Cpu *cpu) {
    if (pc_vr_lifecycle_call(cpu, "_ZN2nn2oe15EnableRecordingEv",
                             "_ZN2nn3hid32ResetSevenSixAxisSensorTimestampEv",
                             g_site.labo_enable_rec_call, "EnableRecording -> ResetSevenSixTimestamp"))
        pc_vr_motion_contract_timestamp_reset();
}

PcVrHostFn pc_vr_lifecycle_lookup(const char *name) {
    // Without PC VR, NULL lets import resolution select the guest SDK directly.
    if (!pc_vr_enabled()) return NULL;
    if (!strcmp(name, "_ZN2nn2pl9EndVrModeEv")) return pc_vr_end_vr_mode;
    if (!strcmp(name, "_ZN2nn2oe15EnableRecordingEv")) return pc_vr_enable_recording;
    return NULL;
}

// The PC VR 2D content as world-locked quads, its only way to the headset (D068): menus (D056)
// SAVAGEWIND_PC_VR_MENU_DISTANCE metres ahead (default 0.77, the user's choice in the headset), the gameplay HUD (D057)
// at SAVAGEWIND_PC_VR_HUD_DEPTH times the camera target's distance (default 0.9: where the user set it by hand, at or a
// little in front of Link); both from the configuration (pc_vr_config.h), whose eye images they need. Live steps stay
// within each panel's range.
static pthread_mutex_t panel_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    double min, max;
    bool read, active;
    double depth, launch;
} panels[2] = {  // under panel_lock
    [PC_VR_PANEL_MENU] = {PC_VR_MENU_DISTANCE_MIN, PC_VR_MENU_DISTANCE_MAX},
    [PC_VR_PANEL_HUD] = {PC_VR_HUD_DEPTH_MIN, PC_VR_HUD_DEPTH_MAX},
};

static void panel_read_locked(PcVrPanel which) {
    if (panels[which].read) return;
    panels[which].read = true;
    const PcVrConfig *config = pc_vr_config();
    panels[which].active = config->enabled;  // (the play configuration has the eye images)
    panels[which].launch = which == PC_VR_PANEL_MENU ? config->play.menu_distance : config->play.hud_depth;
    panels[which].depth = panels[which].launch;
}

double pc_vr_panel_depth(PcVrPanel which) {
    if ((unsigned)which > PC_VR_PANEL_HUD) return 0;
    pthread_mutex_lock(&panel_lock);
    panel_read_locked(which);
    const double depth = panels[which].active ? panels[which].depth : 0;
    pthread_mutex_unlock(&panel_lock);
    return depth;
}

double pc_vr_panel_depth_adjust(PcVrPanel which, double factor) {
    if ((unsigned)which > PC_VR_PANEL_HUD) return 0;
    pthread_mutex_lock(&panel_lock);
    panel_read_locked(which);
    double depth = 0;
    if (panels[which].active) {
        const double adjusted = factor > 0 ? panels[which].depth * factor : panels[which].launch;
        panels[which].depth = adjusted < panels[which].min ? panels[which].min :
                              adjusted > panels[which].max ? panels[which].max : adjusted;
        depth = panels[which].depth;
    }
    pthread_mutex_unlock(&panel_lock);
    if (depth && which == PC_VR_PANEL_HUD)
        LOG("[pc-vr-eyes] hud depth %.2f times the camera target's distance\n", depth);
    else if (depth) LOG("[pc-vr-eyes] menu distance %.2f m\n", depth);
    return depth;
}

double pc_vr_hud_distance(double depth, double focus_m, double previous_m, double dt_s) {
    double target = depth * focus_m;
    if (!(target > 0) || !isfinite(target)) target = previous_m > 0 ? previous_m : 0.77;
    target = target < 0.1 ? 0.1 : target > 20 ? 20 : target;
    if (!(previous_m > 0) || !(dt_s >= 0)) return target;
    // (exponential easing in log distance: the fraction of the way covered in dt_s, with a 0.25 s time constant)
    const double eased = 1 - exp(-dt_s / 0.25);
    return exp(log(previous_m) + eased * (log(target) - log(previous_m)));
}
