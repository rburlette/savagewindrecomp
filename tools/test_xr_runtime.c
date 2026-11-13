// Asset-free fake-runtime checks. No loader/runtime/device is opened. Compile
// with SAVAGEWIND_WITH_OPENXR, official headers, Vulkan headers, SDL3 and pthread.
#include <assert.h>
#include <stdio.h>
#include "../runtime/xr/xr_runtime.c"
#include "test_pc_vr_settings.h"  // (the XR owner's settings: the play configuration's, PC VR off unless a case says)
void rt_fatal_trace(void) { abort(); }

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

static struct {
    atomic_uint waits, begins, ends, locates, session_ends, destroy_spaces, destroy_sessions, destroy_instances, queue_idles;
    atomic_int event;
    XrResult wait_result, begin_result, end_result, locate_result;
    XrResult destroy_view_result, destroy_local_result, destroy_session_result, destroy_instance_result;
    VkResult idle_result;
    XrSpaceLocationFlags flags;
    XrViewStateFlags view_flags;
    XrSpaceVelocityFlags velocity_flags;
    XrVector3f angular_velocity;
    bool reset_during_locate, delay_end, paced, no_render, require_frame_completion, rich_views;
    XrTime predicted;
    unsigned expected_layers;
    float eye_shift;
    XrQuaternionf head;
} mock;
static PFN_vkVoidFunction (*test_device_proc)(VkDevice, const char *);
static void (*test_end_inspect)(const XrFrameEndInfo *);

// Direct entry points referenced by the implementation. These deliberately
// cannot discover a runtime, even if a developer accidentally calls discover.
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char *layer, uint32_t cap,
        uint32_t *count, XrExtensionProperties *props) {
    (void)layer; (void)cap; (void)count; (void)props; return XR_ERROR_RUNTIME_UNAVAILABLE;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo *info, XrInstance *instance) {
    (void)info; (void)instance; return XR_ERROR_RUNTIME_UNAVAILABLE;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *fn) {
    (void)instance; (void)name; *fn = NULL; return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance) {
    (void)instance; atomic_fetch_add(&mock.destroy_instances, 1); return mock.destroy_instance_result;
}
void plat_terminate_status(int status) {
    if (status != 1 || !xr.teardown_pending || !xr.instance || !xr.session ||
        atomic_load(&mock.destroy_spaces) || atomic_load(&mock.destroy_instances)) _Exit(98);
    puts("OpenXR failed-shutdown exit fixture passed (nonzero before loader destruction)");
    fflush(stdout);
    _Exit(status);
}
static VKAPI_ATTR VkResult VKAPI_CALL mock_vk_create(VkPhysicalDevice physical,
        const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *device) {
    (void)physical; (void)allocator;
    CHECK(xr_vulkan_feature_chain_valid(info));
    *device = (VkDevice)(uintptr_t)1;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL mock_vk_instance(const VkInstanceCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkInstance *instance) {
    (void)allocator;
    CHECK(info->pApplicationInfo->apiVersion == VK_API_VERSION_1_3);
    *instance = (VkInstance)(uintptr_t)1;
    return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    (void)instance;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)mock_vk_instance;
    return !strcmp(name, "vkCreateDevice") ? (PFN_vkVoidFunction)mock_vk_create : NULL;
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue) {
    (void)device; (void)family; (void)index; *queue = (VkQueue)(uintptr_t)1;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name) {
    return test_device_proc ? test_device_proc(device, name) : NULL;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
    if (mock.require_frame_completion) {
        CHECK(atomic_load(&mock.ends) > 0);
        CHECK(pthread_mutex_trylock(&xr_vulkan_lock) != 0);
    }
    (void)queue; atomic_fetch_add(&mock.queue_idles, 1); return mock.idle_result;
}

static XrResult XRAPI_CALL mock_wait(XrSession session, const XrFrameWaitInfo *info, XrFrameState *state) {
    (void)session; (void)info;
    // The owner waits without xr_vulkan_lock; game device-idle may proceed.
    CHECK(pthread_mutex_trylock(&xr_vulkan_lock) == 0);
    pthread_mutex_unlock(&xr_vulkan_lock);
    if (mock.paced) SDL_Delay(2);
    atomic_fetch_add(&mock.waits, 1);
    state->predictedDisplayTime = mock.predicted;
    state->predictedDisplayPeriod = 11111111;
    state->shouldRender = mock.no_render ? XR_FALSE : XR_TRUE;
    return mock.wait_result;
}
static XrResult XRAPI_CALL mock_begin(XrSession session, const XrFrameBeginInfo *info) {
    (void)session; (void)info; atomic_fetch_add(&mock.begins, 1); return mock.begin_result;
}
static XrResult XRAPI_CALL mock_end(XrSession session, const XrFrameEndInfo *info) {
    (void)session;
    CHECK(!strcmp(xr_runtime_current_call(), mock.expected_layers ? "xrEndFrame(game-eyes)" : "xrEndFrame(zero-layers)"));
    CHECK(info->layerCount == mock.expected_layers);
    CHECK(mock.expected_layers ? info->layers != NULL : info->layers == NULL);
    CHECK(info->displayTime == mock.predicted);
    if (test_end_inspect) test_end_inspect(info);
    if (mock.delay_end) SDL_Delay(260);
    atomic_fetch_add(&mock.ends, 1);
    return mock.end_result;
}
static XrResult XRAPI_CALL mock_locate(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation *location) {
    (void)space; (void)base;
    CHECK(time == mock.predicted);
    CHECK(space == xr.view && base == xr.local);
    XrSpaceVelocity *velocity = location->next;
    CHECK(velocity && velocity->type == XR_TYPE_SPACE_VELOCITY && !velocity->next);
    velocity->velocityFlags = mock.velocity_flags;
    velocity->angularVelocity = mock.angular_velocity;
    atomic_fetch_add(&mock.locates, 1);
    location->locationFlags = mock.flags;
    location->pose.orientation = mock.head;
    if (mock.reset_during_locate) xr_runtime_reset_game();
    return mock.locate_result;
}
static XrResult XRAPI_CALL mock_views(XrSession session, const XrViewLocateInfo *info,
        XrViewState *state, uint32_t cap, uint32_t *count, XrView *views) {
    (void)session;
    CHECK(info->displayTime == mock.predicted);
    CHECK(cap == 2);
    *count = 2;
    state->viewStateFlags = mock.view_flags;
    views[0].pose.orientation.w = views[1].pose.orientation.w = 1;
    views[0].pose.position.x = -0.031f + mock.eye_shift;
    views[1].pose.position.x = 0.034f + mock.eye_shift;
    for (unsigned i = 0; i < 2; ++i) views[i].fov = (XrFovf){-0.7f, 0.8f, 0.6f, -0.5f};
    if (mock.rich_views) for (unsigned i = 0; i < 2; ++i) {
        views[i].pose.orientation = (XrQuaternionf){0.05f, i ? -0.1f : 0.1f, 0.02f, sqrtf(1 - 0.0025f - 0.01f - 0.0004f)};
        views[i].pose.position.y = 1.25f;
        views[i].pose.position.z = -0.25f;
        views[i].fov = (XrFovf){i ? -0.73f : -0.72f, i ? 0.87f : 0.86f, 0.67f, -0.55f};
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_poll(XrInstance instance, XrEventDataBuffer *buffer) {
    (void)instance;
    const int event = atomic_exchange(&mock.event, 0);
    if (!event) return XR_EVENT_UNAVAILABLE;
    if (event == 1 || event == 3) {
        XrEventDataSessionStateChanged change = {.type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED,
            .session = xr.session, .state = event == 1 ? XR_SESSION_STATE_VISIBLE : XR_SESSION_STATE_STOPPING};
        memcpy(buffer, &change, sizeof change);
    } else if (event == 2) {
        XrEventDataReferenceSpaceChangePending change = {.type = XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING,
            .session = xr.session, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL, .changeTime = mock.predicted + 1};
        memcpy(buffer, &change, sizeof change);
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_exit(XrSession session) {
    (void)session; atomic_store(&mock.event, 3); return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_end_session(XrSession session) {
    (void)session;
    CHECK(atomic_load(&mock.queue_idles) > 0);
    atomic_fetch_add(&mock.session_ends, 1); return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_destroy_space(XrSpace space) {
    CHECK(atomic_load(&mock.destroy_sessions) == 0);
    atomic_fetch_add(&mock.destroy_spaces, 1);
    return (uintptr_t)space == 4 ? mock.destroy_view_result : mock.destroy_local_result;
}
static XrResult XRAPI_CALL mock_destroy_session(XrSession session) {
    (void)session;
    CHECK(!xr.view && !xr.local);
    CHECK(atomic_load(&mock.destroy_instances) == 0);
    CHECK(atomic_load(&mock.queue_idles) > 0);
    atomic_fetch_add(&mock.destroy_sessions, 1);
    return mock.destroy_session_result;
}

static void prepare(void) {
    memset(&xr, 0, sizeof xr);
    memset(&mock, 0, sizeof mock);
    memset(&published, 0, sizeof published);
    memset(&eye_gpu, 0, sizeof eye_gpu);
    test_end_inspect = NULL;
    atomic_store(&latest_present, 0);
    atomic_store(&stop_requested, false);
    atomic_store(&accepting, true);
    atomic_store(&reset_epoch, 0);
    atomic_store(&reference_generation, 1);
    xr.instance = (XrInstance)(uintptr_t)1;
    xr.session = (XrSession)(uintptr_t)2;
    xr.local = (XrSpace)(uintptr_t)3;
    xr.view = (XrSpace)(uintptr_t)4;
    xr.queue = (VkQueue)(uintptr_t)1;
    xr.state = XR_SESSION_STATE_FOCUSED;
    xr.running = true;
    xr.blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    xr.local_epoch = ++local_definitions;
    mock.head = (XrQuaternionf){0, 0, 0, 1};
    xr.WaitFrame = mock_wait; xr.BeginFrame = mock_begin; xr.EndFrame = mock_end;
    xr.LocateSpace = mock_locate; xr.LocateViews = mock_views; xr.PollEvent = mock_poll;
    xr.RequestExitSession = mock_exit; xr.EndSession = mock_end_session;
    xr.DestroySpace = mock_destroy_space; xr.DestroySession = mock_destroy_session;
    xr.DestroyInstance = xrDestroyInstance;
    mock.predicted = 123456789;
    mock.flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    mock.view_flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    mock.velocity_flags = XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
    mock.angular_velocity = (XrVector3f){0.25f, -1.5f, 0.125f};
}

static void test_frames(void) {
    XrPoseSnapshot pose;
    prepare();
    const uint64_t before_locate = xr_runtime_pose_clock_ns();
    tracking_frame(42);
    CHECK(!strcmp(xr_runtime_current_call(), "outside-XR-call"));
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID);
    CHECK(pose.published_monotonic_ns >= before_locate &&
          pose.published_monotonic_ns <= xr_runtime_pose_clock_ns());
    const uint64_t before_clock = SDL_GetTicksNS();
    const uint64_t pose_clock = xr_runtime_pose_clock_ns();
    CHECK(pose_clock >= before_clock && pose_clock <= SDL_GetTicksNS());
    CHECK(pose.game_present_id == 42 && pose.frame_id == 1);
    CHECK(pose.predicted_display_time_ns == mock.predicted);
    CHECK(pose.orientation_xyzw[3] == 1 && pose.views_valid);
    CHECK(!pose.native_views_valid); // Tracking availability does not imply native image provenance.
    CHECK(pose.angular_velocity_valid && pose.angular_velocity_xyz[0] == 0.25f &&
          pose.angular_velocity_xyz[1] == -1.5f && pose.angular_velocity_xyz[2] == 0.125f);
    CHECK(pose.eyes[0].position_xyz[0] == -0.031f && pose.eyes[1].position_xyz[0] == 0.034f);
    CHECK(pose.eyes[0].fov_left_right_up_down[0] == -0.7f);
    CHECK(atomic_load(&mock.begins) == 1 && atomic_load(&mock.ends) == 1);
    pthread_mutex_lock(&pose_lock);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_BUSY);
    pthread_mutex_unlock(&pose_lock);
    mock.flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    tracking_frame(43);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE);
    CHECK(!pose.angular_velocity_valid && pose.angular_velocity_xyz[0] == 0 &&
          pose.angular_velocity_xyz[1] == 0 && pose.angular_velocity_xyz[2] == 0);
    CHECK(atomic_load(&mock.ends) == 2 && !xr.terminal);
    mock.flags |= XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    const uint64_t lost_generation = atomic_load(&reference_generation);
    tracking_frame(44);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && pose.generation > lost_generation);
    prepare(); mock.locate_result = XR_ERROR_TIME_INVALID;
    tracking_frame(1);
    CHECK(atomic_load(&mock.ends) == 1 && !xr.terminal);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE);
    prepare(); mock.wait_result = XR_ERROR_SESSION_LOST;
    tracking_frame(1);
    CHECK(xr.terminal && atomic_load(&mock.begins) == 0 && atomic_load(&mock.ends) == 0);
    CHECK(!strcmp(xr_runtime_current_call(), "outside-XR-call"));
    prepare(); mock.begin_result = XR_ERROR_SESSION_LOST;
    tracking_frame(1);
    CHECK(xr.terminal && atomic_load(&mock.begins) == 1 && atomic_load(&mock.ends) == 0);
    prepare(); mock.end_result = XR_ERROR_SESSION_LOST;
    tracking_frame(1);
    CHECK(xr.terminal && atomic_load(&mock.ends) == 1);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE);
    prepare(); atomic_store(&mock.event, 1);
    tracking_frame(1);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE && xr.state == XR_SESSION_STATE_VISIBLE);
    tracking_frame(2);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && pose.active && !pose.focused);
    mock.no_render = true;
    tracking_frame(3);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE);
    CHECK(xr.outcomes[FRAME_NO_RENDER] == 1);
    mock.no_render = false;
    xr.state = XR_SESSION_STATE_SYNCHRONIZED;
    tracking_frame(3);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE);
    CHECK(xr.outcomes[FRAME_NOT_VISIBLE] == 1);
    prepare(); atomic_store(&mock.event, 2);
    tracking_frame(1);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE && xr.pending_reference_change != 0);
    ++mock.predicted;
    tracking_frame(2);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && xr.pending_reference_change == 0);
    prepare(); mock.reset_during_locate = true;
    tracking_frame(1);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE && atomic_load(&mock.ends) == 1);
    prepare(); mock.delay_end = true;
    tracking_frame(1);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE); // age starts at locate, not late publication
    CHECK(!pose.angular_velocity_valid);
    prepare(); mock.velocity_flags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
    tracking_frame(1);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID); // angular rate is independently optional
    CHECK(!pose.angular_velocity_valid && pose.angular_velocity_xyz[0] == 0 &&
          pose.angular_velocity_xyz[1] == 0 && pose.angular_velocity_xyz[2] == 0);
    prepare(); mock.angular_velocity.y = NAN;
    tracking_frame(1);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && !pose.angular_velocity_valid);
    CHECK(pose.angular_velocity_xyz[0] == 0 && pose.angular_velocity_xyz[1] == 0 &&
          pose.angular_velocity_xyz[2] == 0);
    mock.angular_velocity.y = INFINITY;
    tracking_frame(2);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && !pose.angular_velocity_valid);
    prepare(); tracking_frame(1);
    xr_runtime_reset_game();
    CHECK(!published.angular_velocity_valid && published.angular_velocity_xyz[1] == 0);
    // Display-only frames run the full frame lifecycle but never publish, so
    // game consumers keep one tracking sample per game present.
    prepare(); tracking_frame(7);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && pose.frame_id == 1 && pose.game_present_id == 7);
    ++mock.predicted;
    tracking_frame(0);
    CHECK(atomic_load(&mock.begins) == 2 && atomic_load(&mock.ends) == 2 && xr.frame_id == 2);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && pose.frame_id == 1 && pose.game_present_id == 7);
    mock.flags = 0;
    tracking_frame(0);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && pose.frame_id == 1); // loss is judged at publication
    tracking_frame(8);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE);
    // Display-rate sampling refreshes available tracking without another game
    // present. Invalid tracking also takes effect on a display-only frame.
    prepare(); xr.display_rate_tracking = true; tracking_frame(7);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && pose.frame_id == 1);
    tracking_frame(0);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && pose.frame_id == 2 && !pose.game_present_id);
    CHECK(atomic_load(&mock.begins) == 2 && atomic_load(&mock.ends) == 2);
    mock.flags = 0; tracking_frame(0);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE);
}

static XrQuaternionf yaw_pitch_roll(double yaw, double pitch, double roll) {
    // Head rotation yaw(+Y) * pitch(+X) * roll(+Z), xyzw.
    const double cy = cos(0.5 * yaw), sy = sin(0.5 * yaw), cp = cos(0.5 * pitch), sp = sin(0.5 * pitch);
    const double cr = cos(0.5 * roll), sr = sin(0.5 * roll);
    const double yp[4] = {cy * sp, sy * cp, -sy * sp, cy * cp}; // (0,sy,0,cy) * (sp,0,0,cp)
    return (XrQuaternionf){(float)(yp[0] * cr + yp[1] * sr), (float)(yp[1] * cr - yp[0] * sr),
                           (float)(yp[2] * cr + yp[3] * sr), (float)(yp[3] * cr - yp[2] * sr)};
}

static void test_display_only_recovery(void) {
    prepare();
    xr.display_rate_tracking = true;
    mock.view_flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
        XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    tracking_frame(0);
    XrPoseSnapshot pose;
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && !pose.game_present_id);
    const XrCalibration calibration = pose.calibration;
    CHECK(calibration.epoch != 0);
    const XrSpaceLocationFlags flags = mock.flags;
    mock.flags = 0;
    tracking_frame(0);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE);
    mock.flags = flags;
    tracking_frame(0);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && !pose.game_present_id);
    CHECK(!memcmp(&pose.calibration, &calibration, sizeof calibration));
    atomic_store(&mock.event, 2);
    tracking_frame(0);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE && xr.pending_reference_change);
    ++mock.predicted;
    tracking_frame(0);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && !xr.pending_reference_change && !pose.game_present_id);
    CHECK(pose.calibration.epoch != calibration.epoch && pose.calibration.epoch == xr.local_epoch);
    CHECK(atomic_load(&mock.begins) == 5 && atomic_load(&mock.ends) == 5);
}
static bool heading_is(const XrCalibration *c, double yaw) {
    return fabs(c->heading_xyzw[0]) < 1e-6 && fabs(c->heading_xyzw[2]) < 1e-6 &&
        fabs(c->heading_xyzw[1] - sin(0.5 * yaw)) < 1e-5 && fabs(c->heading_xyzw[3] - cos(0.5 * yaw)) < 1e-5;
}

// D053: one calibration per LOCAL definition, yaw only, kept through tracking
// loss and recovery; an applied LOCAL change (the runtime's recenter) recaptures.
#define BENCH_PI 3.14159265358979323846
// Benchmark head script: bounded schedule, rigid turn of head and eyes about their midpoint, FOV kept.
static void test_bench_head(void) {
    double yaw = 1, pitch = 1;
    CHECK(xr_bench_head_turn(0, &yaw, &pitch) && yaw == 0 && pitch == 0);
    CHECK(xr_bench_head_turn(150, &yaw, &pitch) && fabs(yaw - 70 * BENCH_PI / 180) < 1e-9 && pitch == 0);
    CHECK(xr_bench_head_turn(675, &yaw, &pitch) && yaw == 0 && fabs(pitch - 25 * BENCH_PI / 180) < 1e-9);
    CHECK(xr_bench_head_turn(1200, &yaw, &pitch) && fabs(yaw - BENCH_PI) < 1e-9 && pitch == 0);
    CHECK(!xr_bench_head_turn(XR_BENCH_HEAD_STEPS, &yaw, &pitch));
    CHECK(!xr_bench_head_turn(0, NULL, &pitch));
    float head[4] = {0, 0, 0, 1};
    XrEyeSnapshot eyes[2] = {
        {.orientation_xyzw = {0, 0, 0, 1}, .position_xyz = {-0.032f, 1.6f, 0}, .fov_left_right_up_down = {-0.9f, 0.8f, 0.85f, -0.9f}},
        {.orientation_xyzw = {0, 0, 0, 1}, .position_xyz = {0.032f, 1.6f, 0}, .fov_left_right_up_down = {-0.8f, 0.9f, 0.85f, -0.9f}}};
    CHECK(xr_bench_head_apply(BENCH_PI / 2, 0, head, eyes));  // +90 degrees yaw: forward -Z becomes -X
    CHECK(fabsf(head[1] - (float)sin(BENCH_PI / 4)) < 1e-6f && fabsf(head[3] - (float)cos(BENCH_PI / 4)) < 1e-6f);
    CHECK(fabsf(eyes[0].position_xyz[0]) < 1e-6f && fabsf(eyes[0].position_xyz[2] - 0.032f) < 1e-6f);
    CHECK(fabsf(eyes[1].position_xyz[2] + 0.032f) < 1e-6f && eyes[1].position_xyz[1] == 1.6f);
    CHECK(eyes[0].fov_left_right_up_down[0] == -0.9f && eyes[1].fov_left_right_up_down[1] == 0.9f);
    CHECK(!memcmp(eyes[0].orientation_xyzw, head, sizeof head));
    XrEyeSnapshot untouched[2];
    memcpy(untouched, eyes, sizeof untouched);
    float bad[4] = {0, 0, 0, 2};
    CHECK(!xr_bench_head_apply(0.1, 0, bad, eyes) && !memcmp(untouched, eyes, sizeof untouched));
}

static void test_calibration(void) {
    const float left[3] = {-0.03f, 1.6f, 0.1f}, right[3] = {0.034f, 1.62f, 0.12f};
    XrCalibration c = {0}, untouched;
    for (unsigned roll = 0; roll < 2; ++roll) {
        const XrQuaternionf q = yaw_pitch_roll(0.7, -0.5, roll ? 0.3 : 0);
        CHECK(xr_calibration_capture((const float[4]){q.x, q.y, q.z, q.w}, left, right, 5, &c));
        CHECK(c.epoch == 5 && heading_is(&c, 0.7)); // pitch and roll do not enter the heading
        CHECK(fabsf(c.origin_xyz[0] - 0.002f) < 1e-6f && fabsf(c.origin_xyz[1] - 1.61f) < 1e-6f &&
              fabsf(c.origin_xyz[2] - 0.11f) < 1e-6f);
    }
    const XrQuaternionf behind = yaw_pitch_roll(-2.9, 0.2, 0);
    CHECK(xr_calibration_capture((const float[4]){behind.x, behind.y, behind.z, behind.w}, left, right, 6, &c) &&
          heading_is(&c, -2.9));
    untouched = c;
    const XrQuaternionf up = yaw_pitch_roll(0.4, 1.45, 0); // about 83 degrees up
    CHECK(!xr_calibration_capture((const float[4]){up.x, up.y, up.z, up.w}, left, right, 7, &c));
    CHECK(!xr_calibration_capture((const float[4]){0, 0, 0, 1}, left, right, 0, &c));
    CHECK(!xr_calibration_capture((const float[4]){0, 0, 0, 2}, left, right, 7, &c));
    CHECK(!xr_calibration_capture((const float[4]){NAN, 0, 0, 1}, left, right, 7, &c));
    CHECK(!xr_calibration_capture((const float[4]){0, 0, 0, 1}, (const float[3]){INFINITY, 0, 0}, right, 7, &c));
    CHECK(!memcmp(&c, &untouched, sizeof c));

    XrPoseSnapshot pose;
    prepare();
    const uint64_t epoch = xr.local_epoch;
    tracking_frame(1); // orientation-only views: no calibration yet
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && !pose.calibration.epoch);
    mock.view_flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
        XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    mock.head = yaw_pitch_roll(0.5, 0.1, 0);
    tracking_frame(2);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && pose.native_views_valid);
    CHECK(pose.calibration.epoch == epoch && heading_is(&pose.calibration, 0.5));
    CHECK(fabsf(pose.calibration.origin_xyz[0] - 0.0015f) < 1e-6f);
    const XrCalibration first = pose.calibration;
    // Turned head, moved eyes, then tracking loss and recovery: same calibration.
    mock.head = yaw_pitch_roll(-1.2, 0, 0); mock.eye_shift = 0.25f;
    tracking_frame(3);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && !memcmp(&pose.calibration, &first, sizeof first));
    mock.flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    tracking_frame(4);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE);
    const uint64_t lost = atomic_load(&reference_generation);
    mock.flags |= XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    tracking_frame(5);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && pose.generation > lost);
    CHECK(!memcmp(&pose.calibration, &first, sizeof first));
    atomic_store(&mock.event, 1); // focus loss and return
    tracking_frame(6);
    xr.state = XR_SESSION_STATE_FOCUSED;
    tracking_frame(7);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && !memcmp(&pose.calibration, &first, sizeof first));
    // An applied LOCAL change starts a new epoch, captured from the current head.
    atomic_store(&mock.event, 2);
    tracking_frame(8);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE && xr.pending_reference_change &&
          xr.calibration.epoch == epoch); // kept until the change is in force
    ++mock.predicted;
    tracking_frame(9);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && pose.calibration.epoch > epoch);
    CHECK(heading_is(&pose.calibration, -1.2) && fabsf(pose.calibration.origin_xyz[0] - 0.2515f) < 1e-6f);

    // Menu anchor: the calibration pose whatever the head does, keeping the
    // head-local eye offsets, optical cant, FOV and tracking fields.
    const XrQuaternionf heading = yaw_pitch_roll(0.2, 0, 0);
    XrPoseSnapshot current = {.frame_id = 4, .generation = 9, .active = true, .focused = true,
        .native_views_valid = true, .calibration = {.epoch = 2, .heading_xyzw = {heading.x, heading.y, heading.z, heading.w},
        .origin_xyz = {0.1f, 1.6f, -0.2f}}}, anchors[2];
    for (unsigned k = 0; k < 2; ++k) {
        const XrQuaternionf q = k ? yaw_pitch_roll(-1.1, 0.4, 0.2) : yaw_pitch_roll(0.8, -0.3, 0);
        const double head[4] = {q.x, q.y, q.z, q.w}, shift[3] = {k ? 0.7 : -0.2, k ? 1.2 : 1.7, k ? 0.5 : 0.1};
        current.orientation_xyzw[0] = q.x; current.orientation_xyzw[1] = q.y;
        current.orientation_xyzw[2] = q.z; current.orientation_xyzw[3] = q.w;
        for (unsigned side = 0; side < 2; ++side) {
            const double local[3] = {side ? 0.032 : -0.032, 0, 0}, cant[4] = {0, sin(side ? -0.025 : 0.025), 0, cos(0.025)};
            double placed[3], eye[4];
            quaternion_rotate(head, local, placed);
            quaternion_multiply(head, cant, eye);
            for (unsigned i = 0; i < 3; ++i) current.eyes[side].position_xyz[i] = (float)(shift[i] + placed[i]);
            for (unsigned i = 0; i < 4; ++i) current.eyes[side].orientation_xyzw[i] = (float)eye[i];
            current.eyes[side].fov_left_right_up_down[0] = side ? -0.9f : -0.8f;
        }
        CHECK(xr_calibration_anchor(&current, &anchors[k]));
        CHECK(anchors[k].generation == 9 && anchors[k].frame_id == 4 && anchors[k].native_views_valid);
        CHECK(anchors[k].eyes[1].fov_left_right_up_down[0] == -0.9f && heading_is(&anchors[k].calibration, 0.2));
        for (unsigned side = 0; side < 2; ++side) {
            const double x = side ? 0.032 : -0.032, yaw = 0.2 + (side ? -0.05 : 0.05);
            const XrEyeSnapshot *e = &anchors[k].eyes[side];
            CHECK(fabs(e->position_xyz[0] - (0.1 + x * cos(0.2))) < 1e-5 && fabs(e->position_xyz[1] - 1.6) < 1e-5 &&
                  fabs(e->position_xyz[2] - (-0.2 - x * sin(0.2))) < 1e-5);
            CHECK(fabs(e->orientation_xyzw[0]) < 1e-5 && fabs(e->orientation_xyzw[2]) < 1e-5 &&
                  fabs(e->orientation_xyzw[1] - sin(0.5 * yaw)) < 1e-5 && fabs(e->orientation_xyzw[3] - cos(0.5 * yaw)) < 1e-5);
        }
    }
    XrPoseSnapshot sentinel = anchors[0];
    current.calibration.epoch = 0;
    CHECK(!xr_calibration_anchor(&current, &anchors[0]));
    current.calibration.epoch = 2; current.native_views_valid = false;
    CHECK(!xr_calibration_anchor(&current, &anchors[0]));
    current.native_views_valid = true; current.eyes[1].orientation_xyzw[3] = NAN;
    CHECK(!xr_calibration_anchor(&current, &anchors[0]));
    CHECK(!memcmp(&sentinel, &anchors[0], sizeof sentinel));

    // D056 menu panel: ahead along the heading, the rise vertical, facing back along the heading.
    XrCalibration panel_at = {.epoch = 3, .heading_xyzw = {heading.x, heading.y, heading.z, heading.w},
        .origin_xyz = {0.1f, 1.6f, -0.2f}};
    float position[3], orientation[4];
    CHECK(xr_calibration_panel(&panel_at, 5, -1.2f, position, orientation));
    CHECK(fabs(position[0] - (0.1 - 5 * sin(0.2))) < 1e-5 && fabs(position[1] - 0.4) < 1e-5 &&
          fabs(position[2] - (-0.2 - 5 * cos(0.2))) < 1e-5);
    CHECK(fabs(orientation[0]) < 1e-6 && fabs(orientation[2]) < 1e-6 &&
          fabs(orientation[1] - sin(0.1)) < 1e-6 && fabs(orientation[3] - cos(0.1)) < 1e-6);
    const float kept_position[3] = {position[0], position[1], position[2]};
    CHECK(!xr_calibration_panel(&panel_at, 0, 0, position, orientation));
    CHECK(!xr_calibration_panel(&panel_at, 5, NAN, position, orientation));
    CHECK(!xr_calibration_panel(&panel_at, INFINITY, 0, position, orientation));
    CHECK(!xr_calibration_panel(NULL, 5, 0, position, orientation));
    panel_at.heading_xyzw[3] = 2;
    CHECK(!xr_calibration_panel(&panel_at, 5, 0, position, orientation));
    panel_at.heading_xyzw[3] = heading.w; panel_at.epoch = 0;
    CHECK(!xr_calibration_panel(&panel_at, 5, 0, position, orientation));
    CHECK(!memcmp(position, kept_position, sizeof position));
}

static XrResult XRAPI_CALL mock_runtime_create_device(XrInstance instance,
        const XrVulkanDeviceCreateInfoKHR *info, VkDevice *device, VkResult *result) {
    (void)instance;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)info->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice");
    VkDeviceCreateInfo invalid = *info->vulkanCreateInfo;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .pNext = (void *)invalid.pNext, .timelineSemaphore = VK_TRUE};
    invalid.pNext = &timeline;
    CHECK(create(info->vulkanPhysicalDevice, &invalid, NULL, device) == VK_ERROR_INITIALIZATION_FAILED);
    *result = create(info->vulkanPhysicalDevice, info->vulkanCreateInfo, NULL, device);
    return XR_SUCCESS;
}

static void test_guard(void) {
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore = VK_TRUE};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2};
    CHECK(xr_vulkan_feature_chain_valid(&info));
    VkPhysicalDeviceFeatures base = {0};
    info.pEnabledFeatures = &base;
    CHECK(!xr_vulkan_feature_chain_valid(&info));
    info.pEnabledFeatures = NULL;
    VkPhysicalDeviceVulkan12Features duplicate = v12;
    v12.pNext = &duplicate;
    CHECK(!xr_vulkan_feature_chain_valid(&info));
    v12.pNext = &f2;
    CHECK(!xr_vulkan_feature_chain_valid(&info)); // cycle terminates before driver
    v12.pNext = NULL;
    info.enabledExtensionCount = 257;
    CHECK(!xr_vulkan_feature_chain_valid(&info));
    info.enabledExtensionCount = 0;
    CHECK(!xr_vulkan_feature_chain_valid(NULL));
    prepare();
    xr.CreateVulkanDeviceKHR = mock_runtime_create_device;
    VkDevice device = VK_NULL_HANDLE;
    CHECK(xr_runtime_create_vulkan_device((VkPhysicalDevice)(uintptr_t)1, &info, &device) == VK_SUCCESS);
    CHECK(device != VK_NULL_HANDLE && create_attempts == 2 && create_rejections == 1 && driver_creates == 1);
    CHECK(info.pNext == &f2 && f2.pNext == &v12 && v12.pNext == NULL && v12.timelineSemaphore == VK_TRUE);
    requested_vulkan_api = VK_API_VERSION_1_3;
    successful_instance_creates = 0;
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)guarded_instance_get_proc(VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance = VK_NULL_HANDLE;
    CHECK(create_instance(&instance_info, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(instance == VK_NULL_HANDLE && successful_instance_creates == 0);
    app.apiVersion = VK_API_VERSION_1_3;
    CHECK(create_instance(&instance_info, NULL, &instance) == VK_SUCCESS);
    CHECK(instance != VK_NULL_HANDLE && successful_instance_creates == 1);
}

static void test_owner(void) {
    prepare();
    // Coalescing is bounded and a reset drops already-produced game provenance.
    xr_runtime_notify_game_present(1);
    xr_runtime_notify_game_present(2);
    CHECK(atomic_load(&latest_present) == 2);
    xr_runtime_reset_game();
    CHECK(atomic_load(&latest_present) == 0);
    // Before the session runs, polling services events without XR frames.
    xr.running = false;
    CHECK(pthread_create(&xr.thread, NULL, frame_owner, NULL) == 0);
    xr.thread_started = true;
    SDL_Delay(50);
    CHECK(atomic_load(&mock.waits) == 0);
    xr_runtime_shutdown();
    // A running session frames at display cadence without game presents and
    // publishes tracking only for a present.
    prepare();
    mock.paced = true;
    CHECK(pthread_create(&xr.thread, NULL, frame_owner, NULL) == 0);
    xr.thread_started = true;
    for (unsigned i = 0; i < 200 && atomic_load(&mock.ends) < 5; ++i) SDL_Delay(2);
    CHECK(atomic_load(&mock.ends) >= 5 && atomic_load(&mock.waits) >= 5);
    XrPoseSnapshot pose;
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_UNAVAILABLE && !pose.frame_id);
    xr_runtime_notify_game_present(100);
    for (unsigned i = 0; i < 100 && !published.game_present_id; ++i) SDL_Delay(2);
    CHECK(xr_runtime_get_pose(&pose) == XR_POSE_VALID && pose.game_present_id == 100);
    xr_runtime_vulkan_idle_begin(); // device-wide idle is not starved by the loop
    xr_runtime_vulkan_idle_end();
    xr_runtime_shutdown();
    CHECK(atomic_load(&mock.session_ends) == 1);
    CHECK(atomic_load(&mock.destroy_spaces) == 2 && atomic_load(&mock.destroy_sessions) == 1 &&
          atomic_load(&mock.destroy_instances) == 1);
    CHECK(!xr.thread_started && !atomic_load(&accepting));
    xr_runtime_notify_game_present(200);
    CHECK(atomic_load(&latest_present) == 0);
    xr_runtime_shutdown();
    CHECK(atomic_load(&mock.destroy_instances) == 1); // idempotent cleanup
    prepare();
    xr.local = xr.view = XR_NULL_HANDLE;
    xr.session = XR_NULL_HANDLE;
    xr.DestroyInstance = NULL;
    xr_runtime_shutdown();
    CHECK(atomic_load(&mock.destroy_instances) == 1); // partial proc-load failure fallback
}

static void test_runtime_queue_completion(void) {
    XrInstanceProperties properties = {.type = XR_TYPE_INSTANCE_PROPERTIES,
        .runtimeVersion = XR_MAKE_VERSION(1, 208, 0)};
    strcpy(properties.runtimeName, "Oculus");
    CHECK(needs_runtime_queue_completion(&properties));
    properties.runtimeVersion = XR_MAKE_VERSION(1, 209, 0);
    CHECK(!needs_runtime_queue_completion(&properties));
    properties.runtimeVersion = XR_MAKE_VERSION(1, 208, 0);
    strcpy(properties.runtimeName, "Other runtime");
    CHECK(!needs_runtime_queue_completion(&properties));
    prepare();
    tracking_frame(1);
    CHECK(!atomic_load(&mock.queue_idles)); // All other runtimes retain the existing path.
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        prepare(); xr.runtime_queue_completion = true; mock.require_frame_completion = true;
        mock.no_render = scenario == 1;
        if (scenario == 2) mock.idle_result = VK_ERROR_DEVICE_LOST;
        XrFrameState frame;
        CHECK(wait_frame(&frame));
        pthread_mutex_lock(&xr_vulkan_lock);
        run_frame(&frame, 1); // Match the production owner's lock boundary.
        pthread_mutex_unlock(&xr_vulkan_lock);
        CHECK(atomic_load(&mock.begins) == 1 && atomic_load(&mock.ends) == 1 && atomic_load(&mock.queue_idles) == 1);
        CHECK(xr.queue_completion_waits == 1 && xr.frame_id == 1);
        CHECK(xr.terminal == (scenario == 2));
        if (scenario == 2) {
            CHECK(!published.active);
            CHECK(xr.outcomes[FRAME_TRACKING] == 1); // Successful EndFrame stays counted despite completion failure.
        }
    }
    prepare(); xr.runtime_queue_completion = true; mock.end_result = XR_ERROR_RUNTIME_FAILURE;
    tracking_frame(1);
    CHECK(xr.terminal && !atomic_load(&mock.queue_idles)); // Never drain after an unsuccessful EndFrame.
}

static void test_shutdown_failures(void) {
    const VkResult idle_errors[] = {VK_ERROR_DEVICE_LOST, VK_ERROR_UNKNOWN};
    for (unsigned i = 0; i < 2; ++i) {
        prepare(); xr.running = false; mock.idle_result = idle_errors[i];
        CHECK(!xr_runtime_shutdown_checked());
        CHECK(xr.instance && xr.session && xr.local && xr.view && xr.queue && xr.teardown_pending);
        CHECK(!atomic_load(&mock.destroy_spaces) && !atomic_load(&mock.destroy_sessions) &&
            !atomic_load(&mock.destroy_instances));
        CHECK(!xr_runtime_discover());
        CHECK(!xr_runtime_bind_vulkan((VkInstance)(uintptr_t)1, (VkPhysicalDevice)(uintptr_t)1,
            (VkDevice)(uintptr_t)1, 0, 0));
        mock.idle_result = VK_SUCCESS;
        CHECK(xr_runtime_shutdown_checked());
        CHECK(!xr.instance && !xr.session && !xr.queue && !xr.teardown_pending);
        CHECK(xr_runtime_shutdown_checked());
    }
    const XrResult errors[] = {XR_ERROR_RUNTIME_FAILURE, XR_TIMEOUT_EXPIRED, XR_SESSION_LOSS_PENDING};
    for (unsigned phase = 0; phase < 4; ++phase) for (unsigned i = 0; i < 3; ++i) {
        prepare(); xr.running = false;
        XrResult *result = phase == 0 ? &mock.destroy_view_result : phase == 1 ? &mock.destroy_local_result :
            phase == 2 ? &mock.destroy_session_result : &mock.destroy_instance_result;
        *result = errors[i];
        CHECK(!xr_runtime_shutdown_checked());
        CHECK(xr.instance && xr.teardown_pending && !atomic_load(&accepting));
        CHECK(!xr_runtime_discover());
        if (phase < 2) {
            CHECK(xr.session && (phase == 0 ? xr.view && !xr.local : xr.local && !xr.view));
            CHECK(!atomic_load(&mock.destroy_sessions) && !atomic_load(&mock.destroy_instances));
        } else if (phase == 2) {
            CHECK(xr.session && !xr.local && !xr.view && !atomic_load(&mock.destroy_instances));
        } else CHECK(!xr.session && !xr.local && !xr.view);
        *result = XR_SUCCESS;
        CHECK(xr_runtime_shutdown_checked());
        CHECK(!xr.instance && !xr.session && !xr.local && !xr.view && !xr.queue);
        CHECK(atomic_load(&mock.destroy_spaces) == (phase < 2 ? 3u : 2u)); // successful siblings not repeated
        CHECK(atomic_load(&mock.destroy_sessions) == (phase == 2 ? 2u : 1u));
        CHECK(atomic_load(&mock.destroy_instances) == (phase == 3 ? 2u : 1u));
        CHECK(xr_runtime_shutdown_checked());
    }
    prepare(); xr.running = false; xr.DestroySpace = NULL;
    CHECK(!xr_runtime_shutdown_checked() && xr.local && xr.view && xr.session && xr.instance);
    CHECK(!atomic_load(&mock.destroy_sessions));
    xr.DestroySpace = mock_destroy_space;
    CHECK(xr_runtime_shutdown_checked());
    prepare(); xr.running = false; xr.DestroySession = NULL;
    CHECK(!xr_runtime_shutdown_checked() && !xr.local && !xr.view && xr.session && xr.instance);
    CHECK(!atomic_load(&mock.destroy_instances));
    xr.DestroySession = mock_destroy_session;
    CHECK(xr_runtime_shutdown_checked());
    prepare(); xr.running = false; xr.thread_started = true; xr.thread = pthread_self();
    CHECK(!xr_runtime_shutdown_checked()); // joining this thread must fail without destroying anything
    CHECK(xr.thread_started && xr.session && xr.instance && !atomic_load(&mock.destroy_spaces));
    xr.thread_started = false;
    CHECK(xr_runtime_shutdown_checked());
}

static void simulated_loader_static_destructor(void) {
    CHECK(xr.instance == XR_NULL_HANDLE && xr.session == XR_NULL_HANDLE);
    CHECK(atomic_load(&mock.destroy_instances) == 1 && atomic_load(&mock.destroy_sessions) == 1);
    printf("OpenXR exit-order fixture: %u checks passed (cleanup before simulated loader destruction)\n", checks);
}

static void forbidden_loader_destructor(void) {
    fputs("FAIL: loader destructor reached after failed XR cleanup\n", stderr);
    _Exit(97); // Distinct from the expected cleanup-failure status.
}

int main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--shutdown-failure-exit") || !strcmp(argv[1], "--shutdown-failure-direct"))) {
        prepare(); xr.running = false; mock.idle_result = VK_ERROR_DEVICE_LOST;
        if (atexit(forbidden_loader_destructor) != 0 || !register_exit_cleanup()) _Exit(99);
        if (!strcmp(argv[1], "--shutdown-failure-direct")) {
            xr_runtime_shutdown();
            _Exit(96); // Screenshot/UI/cache operations must never follow a failed void cleanup.
        }
        return 0; // The XR wrapper must terminate nonzero before the simulated loader callback.
    }
    if (argc == 2 && !strcmp(argv[1], "--exit-order")) {
        prepare();
        xr.running = false; // No worker required to test the native exit list.
        CHECK(atexit(simulated_loader_static_destructor) == 0);
        CHECK(register_exit_cleanup());
        CHECK(register_exit_cleanup()); // Idempotent callback registration.
        return 0; // Real CRT exit dispatch; simulated loader dies after XR calls.
    }
    XrPerfMetric metric = {0};
    CHECK(xr_perf_p95(&metric) == 0);
    for (unsigned i = 0; i < 19; ++i) xr_perf_add(&metric, 1000001);
    xr_perf_add(&metric, 900000000);
    CHECK(metric.count == 20 && metric.sum_ns == 919000019 && metric.max_ns == 900000000);
    CHECK(xr_perf_p95(&metric) == 2 && metric.histogram_ms[512] == 1);
    xr_perf_add(&metric, 900000000);
    CHECK(xr_perf_p95(&metric) == 512);
    xr_perf.enabled = true;
    test_frames();
    test_display_only_recovery();
    test_calibration();
    test_bench_head();
    test_guard();
    test_owner();
    test_runtime_queue_completion();
    test_shutdown_failures();
    printf("OpenXR fake-runtime fixture: %u checks passed (no runtime/device/game data)\n", checks);
    return 0;
}
