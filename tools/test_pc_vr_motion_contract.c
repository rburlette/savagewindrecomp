// Asset-free PC VR motion contract fixture (runtime/hle/pc_vr_motion_contract.c): the ten motion import wrappers
// forward to a mock SDK, the verified SevenSix read is answered from a mock XR pose or head look's identity, and the
// pinned mode-2 Stop is redirected to Start. No game files, generated code or headset are loaded.
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_game_sites_182.h"
#include "../runtime/hle/pc_vr_motion_contract.c"

static Module main_module = {.name = "main", .base = MAIN_BASE, .size = TEST_MAIN_SIZE_182};
static uint64_t expected_args[8];
static uint64_t expected_address;
static unsigned sdk_calls, resolve_calls;
static int32_t mock_return = 1;
static int mock_concurrent;
static bool mock_motion_active;
static bool mock_pc_vr;
static bool mock_head_look;
static bool mock_mode2_tracking;
static XrPoseReadResult mock_pose_result = XR_POSE_UNAVAILABLE;
static XrPoseSnapshot mock_pose;
static bool mock_main_available = true;
static bool mock_sample_lock_held;
static unsigned mock_sample_acquires, mock_sample_releases;
static jmp_buf fatal_jump;
static bool expect_fatal;
static unsigned fatal_count;

bool pc_vr_enabled(void) { return mock_pc_vr; }
bool pc_vr_head_look_enabled(void) { return mock_head_look; }
bool pc_vr_labo_mode2_tracking(const Cpu *cpu) {
    (void)cpu;
    return mock_pc_vr && mock_mode2_tracking;
}
XrPoseReadResult xr_runtime_get_pose(XrPoseSnapshot *out) {
    assert(mock_sample_lock_held);
    assert(out);
    *out = mock_pose_result == XR_POSE_BUSY ? (XrPoseSnapshot){0} : mock_pose;
    return mock_pose_result;
}

// AM-before-contract lock order: the AM sample lock is never taken with the contract lock held.
bool labo_motion_sample_acquire(void) {
    assert(!mock_sample_lock_held);
    assert(pthread_mutex_trylock(&motion_contract_lock) == 0);
    pthread_mutex_unlock(&motion_contract_lock);
    if (!mock_motion_active) return false;
    mock_sample_lock_held = true;
    ++mock_sample_acquires;
    return true;
}
void labo_motion_sample_release(void) {
    assert(mock_sample_lock_held);
    assert(pthread_mutex_trylock(&motion_contract_lock) == 0);
    pthread_mutex_unlock(&motion_contract_lock);
    mock_sample_lock_held = false;
    ++mock_sample_releases;
}

Module *loader_module(const char *name) {
    return mock_main_available && !strcmp(name, "main") ? &main_module : NULL;
}

uint64_t loader_find_symbol_in(const char *module, const char *name) {
    assert(!strcmp(module, "sdk"));
    __atomic_fetch_add(&resolve_calls, 1, __ATOMIC_RELAXED);
    for (int import_index = 0; import_index < MOTION_IMPORT_COUNT; ++import_index)
        if (!strcmp(name, motion_imports[import_index].symbol)) return 0x777000ull + 4 * import_index;
    return 0;
}

void guest_call(Cpu *cpu, uint64_t address) {
    assert(!mock_sample_lock_held); // Never enter the SDK with the AM sample lock held.
    if (!mock_concurrent) {
        assert(address == expected_address);
        for (int i = 0; i < 8; ++i) assert(cpu->x[i] == expected_args[i]);
    }
    uint64_t return_address = cpu->x[30];
    cpu->x[30] = 0;
    __atomic_fetch_add(&sdk_calls, 1, __ATOMIC_RELAXED);
    cpu->x[0] = (uint64_t)(int64_t)mock_return; // original SDK result, including negative status
    cpu->x[1] = 0xface;        // guest clobber: the contract must not restore argument registers
    cpu->x[30] = return_address;
}

void rt_fatal_trace(void) {
    if (!expect_fatal) abort();
    ++fatal_count;
    longjmp(fatal_jump, 1);
}

static unsigned contract_checks;
#define CHECK(condition) do { \
    ++contract_checks; \
    if (!(condition)) { \
        fprintf(stderr, "motion contract check failed at line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void set_removed_trace_env(const char *value) {
#ifdef _WIN32
    _putenv_s("SAVAGEWIND_LABO_SENSOR_TRACE", value ? value : "");
#else
    if (value) setenv("SAVAGEWIND_LABO_SENSOR_TRACE", value, 1);
    else unsetenv("SAVAGEWIND_LABO_SENSOR_TRACE");
#endif
}

static void *reader_worker(void *unused) {
    (void)unused;
    HostFn read_six = pc_vr_motion_contract_lookup(motion_imports[READ_SIX].symbol);
    Cpu cpu = {0};
    cpu.x[30] = MAIN_BASE + 0x1234 + 4 * READ_SIX;
    for (int i = 0; i < 500; ++i) {
        cpu.x[1] = 4;
        read_six(&cpu);
    }
    return NULL;
}

#ifdef _WIN32
static Cpu contract_call(int import_index, uint64_t output, int32_t capacity, uint64_t site,
                         int32_t result, bool sdk_forward) {
    Cpu cpu = {0};
    for (int i = 0; i < 8; ++i) cpu.x[i] = 0x200u + (unsigned)i;
    cpu.x[0] = output;
    cpu.x[1] = (uint32_t)capacity;
    cpu.x[30] = site;
    for (int i = 0; i < 8; ++i) expected_args[i] = cpu.x[i];
    expected_address = 0x777000ull + 4 *
        (import_index == STOP_SEVEN && mock_mode2_tracking && g_site.labo_track_start_ret &&
         site == g_site.labo_track_start_ret ? START_SEVEN : import_index);
    const unsigned before = sdk_calls;
    motion_imports[import_index].wrapper(&cpu);
    CHECK(!mock_sample_lock_held && mock_sample_acquires == mock_sample_releases);
    CHECK((int32_t)cpu.x[0] == result);
    CHECK(sdk_calls == before + (sdk_forward ? 1 : 0));
    CHECK(cpu.x[30] == site);
    CHECK(cpu.x[1] == (sdk_forward ? UINT64_C(0xface) : (uint32_t)capacity));
    for (int i = 2; i < 8; ++i) CHECK(cpu.x[i] == expected_args[i]);
    return cpu;
}

static void check_poison(const unsigned char *bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) CHECK(bytes[i] == 0xa5);
}

// The record the contract must write next: head look's identity, or the mock pose against the current neutral.
static void expect_sample(unsigned char *destination, uint64_t serial, int32_t capacity) {
    LaboSensorSample expected;
    if (mock_head_look) {
        CHECK(labo_sensor_sample_build(&expected, LABO_SENSOR_SAMPLE_IDENTITY, serial));
    } else {
        LaboPoseOrientation expected_orientation = pose_orientation;
        CHECK(labo_pose_orientation_sample_motion(&expected_orientation, mock_pose.orientation_xyzw,
              mock_pose.angular_velocity_xyz, mock_pose.generation, serial, &expected));
    }
    memset(destination - 1, 0xa5, sizeof expected + 2);
    contract_call(READ_SEVEN, (uint64_t)(uintptr_t)destination, capacity, g_site.labo_sensor_ret, 1, false);
    CHECK(!memcmp(destination, &expected, sizeof expected));
    CHECK(destination[-1] == 0xa5 && destination[sizeof expected] == 0xa5);
    CHECK(sample_serial == serial);
}

// Under PC VR, the answered site with main laid out otherwise is fatal before any lock or SDK call.
static void expect_fatal_read(uint64_t output) {
    Cpu cpu = {0};
    cpu.x[0] = output;
    cpu.x[1] = 1;
    cpu.x[30] = g_site.labo_sensor_ret;
    const unsigned before_sdk = sdk_calls, before_fatal = fatal_count;
    const unsigned before_acquires = mock_sample_acquires;
    expect_fatal = true;
    if (!setjmp(fatal_jump)) {
        motion_imports[READ_SEVEN].wrapper(&cpu);
        CHECK(0);
    }
    expect_fatal = false;
    CHECK(fatal_count == before_fatal + 1 && sdk_calls == before_sdk);
    CHECK(mock_sample_acquires == before_acquires && !mock_sample_lock_held);
    CHECK(pthread_mutex_trylock(&motion_contract_lock) == 0);
    pthread_mutex_unlock(&motion_contract_lock);
}
#endif

static void test_answered_read(void) {
#ifdef _WIN32
    mock_pc_vr = true;
    CHECK(pc_vr_motion_contract_lookup(motion_imports[READ_SEVEN].symbol) == motion_imports[READ_SEVEN].wrapper);
    CHECK(atomic_load(&pose_delivery_configured));
    mock_motion_active = true;
    mock_head_look = true; // Head look's identity samples are deterministic for the lifecycle and request checks.
    mock_return = -7; // Distinct forwarded result; void lifecycle exports ignore x0.
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const size_t page = info.dwPageSize;
    const uint64_t base = MAIN_BASE + UINT64_C(0x20000000);
    unsigned char *region = VirtualAlloc((void *)(uintptr_t)base, page * 3,
                                         MEM_RESERVE, PAGE_READWRITE);
    CHECK(region == (void *)(uintptr_t)base);
    CHECK(VirtualAlloc(region, page * 2, MEM_COMMIT, PAGE_READWRITE) == region);
    unsigned char *buffer = region + 0x100;
    memset(region, 0xa5, page * 2);
    const uint64_t output = (uint64_t)(uintptr_t)buffer;
    const uint64_t site = g_site.labo_sensor_ret;
    const uint64_t lifecycle_site = MAIN_BASE + 0x100;

    // Neither a start alone nor initialization alone enables sample writes.
    contract_call(READ_SEVEN, output, 1, site, -7, true);
    contract_call(START_SEVEN, 0, 0, lifecycle_site, -7, true);
    CHECK(!sensor_initialized && sensor_started);
    contract_call(READ_SEVEN, output, 1, site, -7, true);
    contract_call(STOP_SEVEN, 0, 0, lifecycle_site, -7, true);
    contract_call(INITIALIZE_SEVEN, 0, 0, lifecycle_site, -7, true);
    CHECK(sensor_initialized && !sensor_started);
    contract_call(READ_SEVEN, output, 1, site, -7, true);
    check_poison(buffer, sizeof(LaboSensorSample));
    contract_call(START_SEVEN, 0, 0, lifecycle_site, -7, true);
    CHECK(sensor_initialized && sensor_started);

    // Every prerequisite must hold. Other readers and other return sites always forward.
    mock_motion_active = false;
    contract_call(READ_SEVEN, output, 1, site, -7, true);
    mock_motion_active = true;
    contract_call(READ_SIX, output, 1, site, -7, true);
    contract_call(READ_SEVEN, output, 1, site + 4, -7, true);
    g_site.labo_sensor_ret = 0;  // (a game version without the reader's site)
    contract_call(READ_SEVEN, output, 1, site, -7, true);
    g_site.labo_sensor_ret = site;
    check_poison(buffer, sizeof(LaboSensorSample));
    CHECK(sample_serial == 0);

    // The answered site requires main laid out as its game sites; another site still forwards.
    ++main_module.size;
    expect_fatal_read(output);
    contract_call(READ_SEVEN, output, 1, site + 4, -7, true);
    --main_module.size;
    main_module.base += 0x1000;
    expect_fatal_read(output);
    main_module.base -= 0x1000;
    mock_main_available = false;
    expect_fatal_read(output);
    mock_main_available = true;
    check_poison(buffer, sizeof(LaboSensorSample));
    CHECK(sample_serial == 0);

    // Eligible malformed requests never reach the SDK or publish a partial sample.
    const int32_t invalid_capacity[] = {0, -1, INT32_MIN};
    for (unsigned i = 0; i < sizeof invalid_capacity / sizeof invalid_capacity[0]; ++i)
        contract_call(READ_SEVEN, output, invalid_capacity[i], site, 0, false);
    const uint64_t invalid_pointer[] = {0, GUEST_LO - 1, GUEST_HI - 32,
                                       UINT64_MAX - 32, base + page * 2};
    for (unsigned i = 0; i < sizeof invalid_pointer / sizeof invalid_pointer[0]; ++i)
        contract_call(READ_SEVEN, invalid_pointer[i], 1, site, 0, false);
    DWORD previous;
    CHECK(VirtualProtect(region, page, PAGE_READONLY, &previous));
    contract_call(READ_SEVEN, output, 1, site, 0, false);
    check_poison(buffer, sizeof(LaboSensorSample));
    CHECK(VirtualProtect(region, page, PAGE_READWRITE | PAGE_GUARD, &previous));
    contract_call(READ_SEVEN, output, 1, site, 0, false);
    CHECK(VirtualProtect(region, page, PAGE_READWRITE, &previous));
    check_poison(buffer, sizeof(LaboSensorSample));
    CHECK(VirtualProtect(region + page, page, PAGE_NOACCESS, &previous));
    contract_call(READ_SEVEN, base + page - 32, 1, site, 0, false);
    check_poison(region + page - 32, 32);
    CHECK(VirtualProtect(region + page, page, PAGE_READONLY, &previous));
    contract_call(READ_SEVEN, base + page - 32, 1, site, 0, false);
    check_poison(region + page - 32, 64);
    CHECK(VirtualProtect(region + page, page, PAGE_READWRITE, &previous));
    CHECK(sample_serial == 0);

    // Larger capacities still receive exactly one record, including unaligned output.
    expect_sample(buffer, 1, INT32_MAX);
    check_poison(buffer + sizeof(LaboSensorSample), 15 * sizeof(LaboSensorSample));
    expect_sample(buffer + 1, 2, 16);

    // The SDK timestamp reset restarts the numbering; an exhausted serial writes nothing.
    contract_call(RESET_SEVEN, 0, 0, lifecycle_site, -7, true);
    CHECK(sample_serial == 0);
    expect_sample(buffer, 1, 1);
    sample_serial = UINT64_MAX;
    memset(buffer, 0xa5, sizeof(LaboSensorSample));
    contract_call(READ_SEVEN, output, 1, site, 0, false);
    check_poison(buffer, sizeof(LaboSensorSample));
    CHECK(sample_serial == UINT64_MAX);
    contract_call(RESET_SEVEN, 0, 0, lifecycle_site, -7, true);

    // Stop and finalize end delivery until the sensor is initialized and started again.
    contract_call(STOP_SEVEN, 0, 0, lifecycle_site, -7, true);
    CHECK(!sensor_started);
    contract_call(READ_SEVEN, output, 1, site, -7, true);
    contract_call(START_SEVEN, 0, 0, lifecycle_site, -7, true);
    expect_sample(buffer, 1, 1);
    contract_call(FINALIZE_SEVEN, 0, 0, lifecycle_site, -7, true);
    CHECK(!sensor_initialized && !sensor_started);
    contract_call(START_SEVEN, 0, 0, lifecycle_site, -7, true);
    contract_call(READ_SEVEN, output, 1, site, -7, true);
    contract_call(INITIALIZE_SEVEN, 0, 0, lifecycle_site, -7, true);
    expect_sample(buffer, 2, 1);

    // A reboot clears the sensor state, the SDK addresses and delivery until the imports resolve again.
    pc_vr_motion_contract_reset();
    CHECK(!sensor_initialized && !sensor_started && sample_serial == 0);
    CHECK(!atomic_load(&pose_delivery_configured) && !mode2_redirect_logged && tracking_state_logs == 0);
    for (int import_index = 0; import_index < MOTION_IMPORT_COUNT; ++import_index)
        CHECK(motion_imports[import_index].sdk_address == 0);
    contract_call(INITIALIZE_SEVEN, 0, 0, lifecycle_site, -7, true);
    contract_call(START_SEVEN, 0, 0, lifecycle_site, -7, true);
    contract_call(READ_SEVEN, output, 1, site, -7, true);
    CHECK(pc_vr_motion_contract_lookup(motion_imports[READ_SEVEN].symbol) == motion_imports[READ_SEVEN].wrapper);
    expect_sample(buffer, 1, 1);
    pc_vr_motion_contract_reset();
    CHECK(mock_sample_acquires > 0 && mock_sample_acquires == mock_sample_releases);
    mock_head_look = false;

    // The XR pose: the in-process snapshot, with no trace or diagnostic option.
    CHECK(pc_vr_motion_contract_lookup(motion_imports[READ_SEVEN].symbol) != NULL);
    CHECK(atomic_load(&pose_delivery_configured));
    mock_pose = (XrPoseSnapshot){.active = true, .focused = true,
        .orientation_valid = true, .orientation_tracked = true,
        .angular_velocity_valid = true, .angular_velocity_xyz = {0, 6.283185307179586f, 0},
        .frame_id = 21, .generation = 7, .predicted_display_time_ns = 123456,
        .orientation_xyzw = {0, 0, 0, 1}};
    mock_pose_result = XR_POSE_UNAVAILABLE;
    contract_call(INITIALIZE_SEVEN, 0, 0, lifecycle_site, -7, true);

    // The pinned mode-2 Stop becomes Start only at its return site and only in mode-2 tracking.
    const uint64_t track_start_site = g_site.labo_track_start_ret;
    mock_mode2_tracking = true;
    contract_call(STOP_SEVEN, 0, 0, MAIN_BASE + 0x114d160, -7, true);
    CHECK(!sensor_started && !mode2_redirect_logged); // The helper's internal address is not the tailcall return.
    contract_call(STOP_SEVEN, 0, 0, track_start_site, -7, true);
    CHECK(sensor_started && mode2_redirect_logged); // Pinned mode-2 transition redirects to Start.
    CHECK(motion_imports[START_SEVEN].sdk_address == 0x777000ull + 4 * START_SEVEN);
    contract_call(STOP_SEVEN, 0, 0, track_start_site, -7, true);
    CHECK(sensor_started); // Every pinned transition redirects, not only the first.
    g_site.labo_track_start_ret = 0;  // (a game version without the site)
    contract_call(STOP_SEVEN, 0, 0, track_start_site, -7, true);
    CHECK(!sensor_started);
    g_site.labo_track_start_ret = track_start_site;
    contract_call(START_SEVEN, 0, 0, lifecycle_site, -7, true);
    mock_mode2_tracking = false;
    contract_call(STOP_SEVEN, 0, 0, track_start_site, -7, true);
    CHECK(!sensor_started); // Outside mode-2 tracking the site reaches the SDK Stop.
    contract_call(START_SEVEN, 0, 0, lifecycle_site, -7, true);
    contract_call(STOP_SEVEN, 0, 0, lifecycle_site, -7, true);
    CHECK(!sensor_started); // Ordinary shutdown still reaches the SDK Stop.
    contract_call(START_SEVEN, 0, 0, lifecycle_site, -7, true);
    memset(buffer, 0xa5, sizeof(LaboSensorSample));
    contract_call(READ_SEVEN, output, 1, site, 0, false);
    check_poison(buffer, sizeof(LaboSensorSample));
    CHECK(!pose_orientation.referenced && sample_serial == 0 && !pose_was_available);

    // Failed guest delivery cannot establish a reference or advance the serial.
    mock_pose_result = XR_POSE_VALID;
    contract_call(READ_SEVEN, GUEST_HI - 32, 1, site, 0, false);
    CHECK(!pose_orientation.referenced && sample_serial == 0);
    contract_call(READ_SEVEN, output, 1, site, 1, false);
    CHECK(pose_orientation.referenced && pose_orientation.generation == 7);
    CHECK(sample_serial == 1 && pose_was_available);
    CHECK(((LaboSensorSample *)buffer)->sampling_number == 1);
    CHECK(((LaboSensorSample *)buffer)->quaternion_30[2] > 0.008f);
    CHECK(fabs(((LaboSensorSample *)buffer)->vector_24[2] - 1) < 0.000001);

    const float yaw20[4] = {0, 0.17364817766693033f, 0, 0.98480775301220806f};
    LaboPoseOrientation expected_orientation = pose_orientation;
    LaboSensorSample expected_yaw20;
    CHECK(labo_pose_orientation_sample_motion(&expected_orientation, yaw20,
          mock_pose.angular_velocity_xyz, 7, 2, &expected_yaw20));
    memcpy(mock_pose.orientation_xyzw, yaw20, sizeof yaw20);
    mock_pose.frame_id = 22;
    contract_call(READ_SEVEN, output, 1, site, 1, false);
    CHECK(sample_serial == 2);
    CHECK(!memcmp(buffer, &expected_yaw20, sizeof expected_yaw20));
    CHECK(((LaboSensorSample *)buffer)->quaternion_30[2] > 0.18f);
    expect_sample(buffer, 3, 1);

    // A nonblocking snapshot collision drops this read but preserves neutral.
    mock_pose_result = XR_POSE_BUSY;
    contract_call(READ_SEVEN, output, 1, site, 0, false);
    CHECK(sample_serial == 3 && pose_orientation.referenced && pose_orientation.generation == 7 && pose_was_available);
    mock_pose_result = XR_POSE_VALID;
    mock_pose.orientation_xyzw[0] = NAN;
    contract_call(READ_SEVEN, output, 1, site, 0, false);
    CHECK(!pose_orientation.referenced && sample_serial == 3 && !pose_was_available);
    mock_pose.orientation_xyzw[0] = 0;
    mock_pose_result = XR_POSE_UNAVAILABLE; // Invalid/stale/focus/loss all fail closed.
    contract_call(READ_SEVEN, output, 1, site, 0, false);
    CHECK(!pose_orientation.referenced && sample_serial == 3);
    mock_pose_result = XR_POSE_VALID;
    mock_pose.frame_id = 23;
    contract_call(READ_SEVEN, output, 1, site, 1, false);
    CHECK(sample_serial == 4 && pose_orientation.referenced);
    CHECK(((LaboSensorSample *)buffer)->quaternion_30[2] > 0.008f &&
          ((LaboSensorSample *)buffer)->quaternion_30[2] < 0.009f);

    // Runtime generation change rebases even if the sensor remains started.
    mock_pose.generation = 8;
    mock_pose.frame_id = 24;
    contract_call(READ_SEVEN, output, 1, site, 1, false);
    CHECK(sample_serial == 5 && pose_orientation.generation == 8);
    CHECK(((LaboSensorSample *)buffer)->quaternion_30[2] > 0.008f &&
          ((LaboSensorSample *)buffer)->quaternion_30[2] < 0.009f);

    // The mode-2 EnableRecording redirect (runtime/pc_vr.c) restarts numbering and calibration.
    pc_vr_motion_contract_timestamp_reset();
    CHECK(sample_serial == 0 && !pose_orientation.referenced && !pose_was_available);
    contract_call(READ_SEVEN, output, 1, site, 1, false);
    CHECK(sample_serial == 1 && pose_orientation.referenced);

    // Velocity is required for live camera delivery. Losing its validity or receiving a nonfinite component drops
    // the read and neutral calibration. Neither condition writes a record or advances the serial.
    LaboSensorSample copied = *(LaboSensorSample *)buffer;
    mock_pose.angular_velocity_valid = false;
    contract_call(READ_SEVEN, output, 1, site, 0, false);
    CHECK(sample_serial == 1 && !pose_orientation.referenced);
    CHECK(!memcmp(buffer, &copied, sizeof copied));
    mock_pose.angular_velocity_valid = true;
    mock_pose.angular_velocity_xyz[0] = NAN;
    contract_call(READ_SEVEN, output, 1, site, 0, false);
    CHECK(sample_serial == 1 && !pose_orientation.referenced);
    CHECK(!memcmp(buffer, &copied, sizeof copied));
    mock_pose.angular_velocity_xyz[0] = 0;
    contract_call(READ_SEVEN, output, 1, site, 1, false);
    CHECK(sample_serial == 2 && pose_orientation.referenced &&
          ((LaboSensorSample *)buffer)->sampling_number == 2);
    CHECK(((LaboSensorSample *)buffer)->quaternion_30[2] > 0.008f &&
          ((LaboSensorSample *)buffer)->quaternion_30[2] < 0.009f);

    contract_call(STOP_SEVEN, 0, 0, lifecycle_site, -7, true);
    CHECK(!sensor_started && !pose_orientation.referenced && !pose_was_available);
    contract_call(READ_SEVEN, output, 1, site, -7, true);
    contract_call(START_SEVEN, 0, 0, lifecycle_site, -7, true);
    contract_call(RESET_SEVEN, 0, 0, lifecycle_site, -7, true);
    CHECK(sample_serial == 0 && !pose_orientation.referenced);
    contract_call(READ_SEVEN, output, 1, site, 1, false);
    CHECK(sample_serial == 1 && pose_orientation.referenced);
    // Render head look must never feed head rotation/velocity into the original orbit helper, even with
    // unavailable or changing XR tracking.
    mock_head_look = true;
    const XrPoseReadResult results[] = {XR_POSE_VALID, XR_POSE_BUSY, XR_POSE_UNAVAILABLE};
    for (unsigned i = 0; i < 3; ++i) {
        mock_pose_result = results[i];
        mock_pose.orientation_xyzw[0] = NAN;
        mock_pose.angular_velocity_xyz[0] = NAN;
        const uint64_t next = sample_serial + 1;
        LaboSensorSample neutral;
        CHECK(labo_sensor_sample_build(&neutral, LABO_SENSOR_SAMPLE_IDENTITY, next));
        contract_call(READ_SEVEN, output, 1, site, 1, false);
        CHECK(sample_serial == next && !memcmp(buffer, &neutral, sizeof neutral));
    }
    mock_head_look = false;
    mock_pose_result = XR_POSE_VALID;
    mock_pose.orientation_xyzw[0] = mock_pose.angular_velocity_xyz[0] = 0;

    contract_call(FINALIZE_SEVEN, 0, 0, lifecycle_site, -7, true);
    CHECK(!sensor_initialized && !sensor_started && !pose_orientation.referenced);
    mock_pc_vr = false;
    mock_pose_result = XR_POSE_UNAVAILABLE;
    pc_vr_motion_contract_reset();
    CHECK(mock_sample_acquires == mock_sample_releases);
    CHECK(VirtualFree(region, 0, MEM_RELEASE));
    mock_return = 1;
    mock_motion_active = false;
#else
    printf("pc vr motion contract: Windows mapping checks skipped on this platform\n");
#endif
}

int main(void) {
    // Outside PC VR no import gets a wrapper, even with the removed trace variable set.
    for (unsigned setting = 0; setting < 3; ++setting) {
        const char *values[] = {NULL, "1", "true"};
        set_removed_trace_env(values[setting]);
        CHECK(pc_vr_motion_contract_lookup("unknown") == NULL);
        for (int import_index = 0; import_index < MOTION_IMPORT_COUNT; ++import_index)
            CHECK(pc_vr_motion_contract_lookup(motion_imports[import_index].symbol) == NULL);
    }
    set_removed_trace_env(NULL);
    CHECK(sdk_calls == 0 && resolve_calls == 0 && !atomic_load(&pose_delivery_configured));

    // Under PC VR each of the ten imports forwards to its SDK export with every register but x30 left as the SDK
    // returned it; the address resolves once.
    mock_pc_vr = true;
    CHECK(pc_vr_motion_contract_lookup("unknown") == NULL);
    for (int import_index = 0; import_index < MOTION_IMPORT_COUNT; ++import_index) {
        HostFn wrapper = pc_vr_motion_contract_lookup(motion_imports[import_index].symbol);
        CHECK(wrapper != NULL && wrapper == motion_imports[import_index].wrapper);
        Cpu cpu = {0};
        for (int j = 0; j < 8; ++j) cpu.x[j] = expected_args[j] = 0x100 + j;
        cpu.x[1] = expected_args[1] = 4;
        cpu.x[30] = MAIN_BASE + 0x1234 + 4 * import_index;
        expected_address = 0x777000ull + 4 * import_index;
        wrapper(&cpu);
        CHECK(cpu.x[0] == 1);
        CHECK(cpu.x[1] == 0xface);
        CHECK(cpu.x[30] == MAIN_BASE + 0x1234 + 4 * import_index);
        // A second call reuses the SDK address and preserves the original result.
        for (int j = 0; j < 8; ++j) cpu.x[j] = expected_args[j];
        wrapper(&cpu);
        CHECK(cpu.x[0] == 1 && cpu.x[30] == MAIN_BASE + 0x1234 + 4 * import_index);
    }
    CHECK(atomic_load(&pose_delivery_configured));
    CHECK(sdk_calls == 2 * MOTION_IMPORT_COUNT && resolve_calls == MOTION_IMPORT_COUNT);
    for (int k = 0; k < 2; ++k) {
        const int import_index = k ? READ_SEVEN : READ_SIX;
        HostFn wrapper = pc_vr_motion_contract_lookup(motion_imports[import_index].symbol);
        Cpu cpu = {0};
        for (int j = 0; j < 8; ++j) cpu.x[j] = expected_args[j] = 0x100 + j;
        cpu.x[1] = expected_args[1] = 4;
        cpu.x[30] = MAIN_BASE + 0x1234 + 4 * import_index;
        expected_address = 0x777000ull + 4 * import_index;
        mock_return = 0;
        wrapper(&cpu);
        CHECK(cpu.x[0] == 0);
        for (int j = 0; j < 8; ++j) cpu.x[j] = expected_args[j];
        mock_return = -7;
        wrapper(&cpu);
        CHECK((int32_t)cpu.x[0] == -7);
    }
    mock_return = 1;
    const unsigned before_workers = sdk_calls;
    mock_concurrent = 1;
    pthread_t workers[4];
    for (int i = 0; i < 4; ++i) CHECK(pthread_create(&workers[i], NULL, reader_worker, NULL) == 0);
    for (int i = 0; i < 4; ++i) CHECK(pthread_join(workers[i], NULL) == 0);
    mock_concurrent = 0;
    CHECK(sdk_calls == before_workers + 2000 && resolve_calls == MOTION_IMPORT_COUNT);

    // A reboot drops the resolved addresses; the next call resolves again and still forwards.
    pc_vr_motion_contract_reset();
    CHECK(motion_imports[READ_SIX].sdk_address == 0 && !atomic_load(&pose_delivery_configured));
    Cpu reboot_call = {0};
    for (int j = 0; j < 8; ++j) reboot_call.x[j] = expected_args[j] = 0x100 + j;
    reboot_call.x[1] = expected_args[1] = 4;
    reboot_call.x[30] = MAIN_BASE + 0x1234 + 4 * READ_SIX;
    expected_address = 0x777000ull + 4 * READ_SIX;
    const unsigned before_reboot = sdk_calls;
    motion_imports[READ_SIX].wrapper(&reboot_call);
    CHECK(resolve_calls == MOTION_IMPORT_COUNT + 1 && sdk_calls == before_reboot + 1);
    pc_vr_motion_contract_reset();
    mock_pc_vr = false;

    test_answered_read();
    printf("pc vr motion contract: %u checks passed\n", contract_checks);
    return 0;
}

// hle/labo_guest_memory.h: a new access generation per call keeps its page cache cold here.
uint64_t plat_guest_access_generation(void) { static uint64_t generation; return ++generation; }
