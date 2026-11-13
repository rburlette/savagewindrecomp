// PC VR motion contract: how PC VR answers the game's SixAxis/SevenSix motion imports. Under PC VR, import resolution
// (runtime/dispatch.c) installs a wrapper for each of the ten imports below. Every wrapper forwards to the original SDK
// export and keeps the SevenSix sensor's initialized/started state, with two exceptions at verified game sites
// (runtime/game_sites.h):
//  - GetSevenSixAxisSensorStates returning to g_site.labo_sensor_ret is answered here with one sample: the XR head
//    pose relative to its neutral calibration, or the identity sample while head look is on. No SDK transfer-memory
//    ring is fabricated; other readers still reach the SDK.
//  - The pinned docked mode-2 lifecycle's StopSevenSixAxisSensor, returning to g_site.labo_track_start_ret, starts
//    the sensor instead (with the paired hooks in runtime/pc_vr.c).
// Guest payloads are never logged; two bounded [labo-pose] lines record tracking changes and the first redirect.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../game_sites.h"
#include "../hle.h"
#include "../pc_vr.h"
#include "../services/labo_motion_state.h"
#include "../xr/xr_runtime.h"
#include "labo_guest_memory.h"
#include "labo_pose_orientation.h"
#include "labo_sensor_sample.h"
#include "pc_vr_head_look.h"
#include "pc_vr_motion_contract.h"

#define TRACKING_STATE_LOG_LIMIT 16

typedef struct {
    const char *symbol;
    const char *label;
    HostFn wrapper;
    uint64_t sdk_address;  // resolved on first use, cleared on reboot
} MotionImport;

enum {
    HANDLES, START_SIX, READ_SIX, GYRO_DRIFT, FINALIZE_SEVEN,
    INITIALIZE_SEVEN, RESET_SEVEN, START_SEVEN, STOP_SEVEN, READ_SEVEN, MOTION_IMPORT_COUNT
};

static void motion_contract_call(Cpu *cpu, int import_index);
#define MOTION_WRAPPER(name, import_index) static void name(Cpu *cpu) { motion_contract_call(cpu, import_index); }
MOTION_WRAPPER(motion_get_six_handles, HANDLES)
MOTION_WRAPPER(motion_start_six, START_SIX)
MOTION_WRAPPER(motion_read_six, READ_SIX)
MOTION_WRAPPER(motion_gyro_drift, GYRO_DRIFT)
MOTION_WRAPPER(motion_finalize_seven, FINALIZE_SEVEN)
MOTION_WRAPPER(motion_initialize_seven, INITIALIZE_SEVEN)
MOTION_WRAPPER(motion_reset_seven, RESET_SEVEN)
MOTION_WRAPPER(motion_start_seven, START_SEVEN)
MOTION_WRAPPER(motion_stop_seven, STOP_SEVEN)
MOTION_WRAPPER(motion_read_seven, READ_SEVEN)

static MotionImport motion_imports[MOTION_IMPORT_COUNT] = {
    {"_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "GetSixAxisSensorHandles", motion_get_six_handles},
    {"_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", "StartSixAxisSensor", motion_start_six},
    {"_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", "GetSixAxisSensorStates", motion_read_six},
    {"_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", "SetGyroscopeZeroDriftMode", motion_gyro_drift},
    {"_ZN2nn3hid26FinalizeSevenSixAxisSensorEv", "FinalizeSevenSixAxisSensor", motion_finalize_seven},
    {"_ZN2nn3hid28InitializeSevenSixAxisSensorEPvm", "InitializeSevenSixAxisSensor", motion_initialize_seven},
    {"_ZN2nn3hid32ResetSevenSixAxisSensorTimestampEv", "ResetSevenSixAxisSensorTimestamp", motion_reset_seven},
    {"_ZN2nn3hid23StartSevenSixAxisSensorEv", "StartSevenSixAxisSensor", motion_start_seven},
    {"_ZN2nn3hid22StopSevenSixAxisSensorEv", "StopSevenSixAxisSensor", motion_stop_seven},
    {"_ZN2nn3hid27GetSevenSixAxisSensorStatesEPNS0_23SevenSixAxisSensorStateEi", "GetSevenSixAxisSensorStates", motion_read_seven},
};

// Lock order: the AM sample lock (labo_motion_sample_acquire) before this one. Neither is held while guest code runs.
static pthread_mutex_t motion_contract_lock = PTHREAD_MUTEX_INITIALIZER;
// Set when import resolution installs the wrappers under PC VR, cleared on reboot until the imports resolve again.
static atomic_bool pose_delivery_configured;
static bool sensor_initialized, sensor_started;
static uint64_t sample_serial;
static LaboPoseOrientation pose_orientation;
static bool pose_was_available;
static unsigned tracking_state_logs;
static bool mode2_redirect_logged;

static void reset_pose_timestamp_locked(void) {
    sample_serial = 0;
    labo_pose_orientation_reset(&pose_orientation);
    pose_was_available = false;
}

void pc_vr_motion_contract_timestamp_reset(void) {
    pthread_mutex_lock(&motion_contract_lock);
    reset_pose_timestamp_locked();
    pthread_mutex_unlock(&motion_contract_lock);
}

// Answers the verified public-reader call site only. Returns false to forward the read to the SDK.
static bool deliver_sensor_sample(Cpu *cpu) {
    if (!atomic_load(&pose_delivery_configured)) return false;
    const Module *main_module = loader_module("main");
    if (pc_vr_enabled() && g_site.labo_sensor_ret && cpu->x[30] == g_site.labo_sensor_ret &&
        (!main_module || main_module->base != MAIN_BASE || main_module->size != g_site.main_end - MAIN_BASE))
        FATAL("PC VR SevenSix reader requires main laid out as its game sites\n");
    if (!g_site.labo_sensor_ret || !main_module || main_module->base != MAIN_BASE ||
        main_module->size != g_site.main_end - MAIN_BASE || cpu->x[30] != g_site.labo_sensor_ret)
        return false;
    if (!labo_motion_sample_acquire()) return false;
    pthread_mutex_lock(&motion_contract_lock);
    if (!sensor_initialized || !sensor_started) {
        pthread_mutex_unlock(&motion_contract_lock);
        labo_motion_sample_release();
        return false;
    }
    if ((int32_t)cpu->x[1] < 1 || sample_serial == UINT64_MAX) {
        cpu->x[0] = 0;
        pthread_mutex_unlock(&motion_contract_lock);
        labo_motion_sample_release();
        return true;
    }
    LaboSensorSample sample;
    LaboPoseOrientation pending_orientation = pose_orientation;
    bool built = false;
    const bool head_look = pc_vr_head_look_enabled();
    if (head_look) {
        // Head orientation is composed only on render-owned Labo eye copies. Identity is the original consumer's
        // unavailable sentinel; zero angular velocity prevents the former sensor-to-orbit input. Physical Npad input
        // and the user's saved motion-aim preference are untouched.
        built = labo_sensor_sample_build(&sample, LABO_SENSOR_SAMPLE_IDENTITY, sample_serial + 1);
    } else {
        XrPoseSnapshot snapshot = {0};
        XrPoseReadResult pose_result = xr_runtime_get_pose(&snapshot);
        if (pose_result == XR_POSE_VALID) {
            bool velocity_valid = snapshot.angular_velocity_valid;
            for (unsigned axis = 0; axis < 3 && velocity_valid; ++axis)
                velocity_valid = isfinite(snapshot.angular_velocity_xyz[axis]);
            if (velocity_valid)
                built = labo_pose_orientation_sample_motion(&pending_orientation,
                    snapshot.orientation_xyzw, snapshot.angular_velocity_xyz,
                    snapshot.generation, sample_serial + 1, &sample);
            if (!built) pose_result = XR_POSE_UNAVAILABLE;
        }
        // Loss, focus change, an invalid pose or velocity and stale tracking all require a fresh neutral on
        // recovery. Never replay the last orientation. A busy snapshot drops this read and keeps the neutral.
        if (pose_result == XR_POSE_UNAVAILABLE) labo_pose_orientation_reset(&pose_orientation);
        if (pose_result != XR_POSE_BUSY && built != pose_was_available &&
            tracking_state_logs++ < TRACKING_STATE_LOG_LIMIT)
            LOG("[labo-pose] tracking %s frame=%llu generation=%llu\n",
                built ? "available" : "unavailable",
                (unsigned long long)snapshot.frame_id,
                (unsigned long long)snapshot.generation);
        if (pose_result != XR_POSE_BUSY) pose_was_available = built;
    }
    if (!built) {
        cpu->x[0] = 0;
        pthread_mutex_unlock(&motion_contract_lock);
        labo_motion_sample_release();
        return true;
    }
    const bool delivered = labo_guest_copy(cpu->x[0], &sample, sizeof sample, true);
    if (delivered) {
        ++sample_serial;
        if (!head_look) pose_orientation = pending_orientation;  // Calibration commits with the guest sample.
    }
    cpu->x[0] = delivered ? 1 : 0;
    pthread_mutex_unlock(&motion_contract_lock);
    labo_motion_sample_release();
    return true;
}

static uint64_t sdk_export_locked(int import_index) {
    MotionImport *motion_import = &motion_imports[import_index];
    if (!motion_import->sdk_address)
        motion_import->sdk_address = loader_find_symbol_in("sdk", motion_import->symbol);
    return motion_import->sdk_address;
}

RT_ON_RESET(pc_vr_motion_contract_reset) {
    pthread_mutex_lock(&motion_contract_lock);
    for (int import_index = 0; import_index < MOTION_IMPORT_COUNT; ++import_index)
        motion_imports[import_index].sdk_address = 0;
    sensor_initialized = sensor_started = false;
    reset_pose_timestamp_locked();
    tracking_state_logs = 0;
    mode2_redirect_logged = false;
    atomic_store(&pose_delivery_configured, false);
    pthread_mutex_unlock(&motion_contract_lock);
}

// Outside PC VR the original guest imports stay untouched, with no per-call wrapper cost.
HostFn pc_vr_motion_contract_lookup(const char *name) {
    if (!pc_vr_enabled()) return NULL;
    for (int import_index = 0; import_index < MOTION_IMPORT_COUNT; ++import_index) {
        if (strcmp(name, motion_imports[import_index].symbol)) continue;
        atomic_store(&pose_delivery_configured, true);
        return motion_imports[import_index].wrapper;
    }
    return NULL;
}

static void motion_contract_call(Cpu *cpu, int import_index) {
    MotionImport *motion_import = &motion_imports[import_index];
    const uint64_t return_address = cpu->x[30];
    // In docked Labo mode 2, the pinned original lifecycle helper takes its disable branch despite the VR object
    // being active. The paired PC VR hooks route EndVrMode -> BeginVrMode and recording -> timestamp reset; route
    // this final Stop -> Start at that one verified return site. The helper tailcalls Stop after restoring x30, so
    // Stop observes the outer caller's return (g_site.labo_track_start_ret), not its internal one. Other stops,
    // especially actual VR shutdown, keep the original SDK behaviour.
    const bool mode2_tracking_start = import_index == STOP_SEVEN && g_site.labo_track_start_ret &&
                                      return_address == g_site.labo_track_start_ret &&
                                      pc_vr_labo_mode2_tracking(cpu);
    pthread_mutex_lock(&motion_contract_lock);
    uint64_t sdk_address = sdk_export_locked(import_index);
    if (mode2_tracking_start) {
        sdk_address = sdk_export_locked(START_SEVEN);
        if (!mode2_redirect_logged) {
            mode2_redirect_logged = true;
            LOG("[labo-pose] pinned mode2 lifecycle: StopSevenSix -> StartSevenSix\n");
        }
    }
    pthread_mutex_unlock(&motion_contract_lock);
    if (!sdk_address) FATAL("labo sensor SDK export missing: %s\n",
                            mode2_tracking_start ? motion_imports[START_SEVEN].label : motion_import->label);

    // guest_call preserves x30 by contract; restore explicitly as the heap wrappers do.
    // No contract lock is held while guest code runs, allowing nested SDK calls.
    const bool answered = import_index == READ_SEVEN && deliver_sensor_sample(cpu);
    if (!answered) guest_call(cpu, sdk_address);
    cpu->x[30] = return_address;

    pthread_mutex_lock(&motion_contract_lock);
    if (import_index == INITIALIZE_SEVEN) sensor_initialized = true;
    if (import_index == START_SEVEN) sensor_started = true;
    if (import_index == STOP_SEVEN) sensor_started = mode2_tracking_start;
    if (import_index == FINALIZE_SEVEN) sensor_initialized = sensor_started = false;
    if (import_index == RESET_SEVEN) reset_pose_timestamp_locked();
    if (atomic_load(&pose_delivery_configured) && (import_index == STOP_SEVEN || import_index == FINALIZE_SEVEN)) {
        labo_pose_orientation_reset(&pose_orientation);
        pose_was_available = false;
    }
    pthread_mutex_unlock(&motion_contract_lock);
}
