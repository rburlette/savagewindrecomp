// The in-process OpenXR owner (PC VR). The Khronos loader finds the OpenXR runtime (Meta's, through Quest Link); this
// module owns the XR session, its frame loop and the game's shared Vulkan binding.
//
// How it fits together:
//  1. Startup (xr_runtime_discover, then the Vulkan hooks below): the XR runtime chooses the GPU and adds what it needs
//     to the game's Vulkan instance and device, and one queue of the game's device is reserved for XR.
//  2. One thread, the frame owner (frame_owner), runs the OpenXR frame loop at the headset's rate (xrWaitFrame,
//     xrBeginFrame, xrEndFrame), independent of the game's frame rate (D081).
//  3. Each display frame it locates the head and both eyes and publishes them as a pose snapshot (XrPoseSnapshot),
//     which the game's camera code reads (xr_runtime_get_pose) to render its two eye views. The first fully tracked
//     frame in a LOCAL space captures the D053 calibration: the head's heading and the eyes' midpoint, which anchor
//     the game's forward and the 2D panels.
//  4. The renderer offers each finished eye pair (xr_runtime_offer_eye_pair, xr_images_internal.h). Each display frame
//     the owner copies the newest finished pair into its swapchains and submits it as a projection layer, with the
//     HUD or menu panels as quad layers; with no new pair it shows the last one again.
// The pure helpers first (calibration, panel placement, the benchmark head script) are built without OpenXR too, for
// the fixtures (tools/test_xr_runtime.c).
#include "xr_runtime.h"
#include <math.h>
#include <string.h>

// Calibration (D053) from one tracked sample: the head's yaw only (where it looks, projected onto the level plane)
// and the midpoint of the two eyes. Fails for an orientation that is not a unit quaternion or a head looking within
// about 10 degrees of straight up or down (no usable heading).
bool xr_calibration_capture(const float head_xyzw[4], const float left_xyz[3],
                            const float right_xyz[3], uint64_t epoch, XrCalibration *out) {
    if (!head_xyzw || !left_xyz || !right_xyz || !epoch || !out) return false;
    double head[4], length_squared = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(head_xyzw[i])) return false;
        head[i] = head_xyzw[i];
        length_squared += head[i] * head[i];
    }
    if (!(length_squared > 0.99 && length_squared < 1.01)) return false;
    for (unsigned i = 0; i < 4; ++i) head[i] /= sqrt(length_squared);
    // Head forward head * (0,0,-1) * inverse(head), x and z components only.
    const double forward_x = -2 * (head[0] * head[2] + head[3] * head[1]);
    const double forward_z = -(1 - 2 * (head[0] * head[0] + head[1] * head[1]));
    if (!(hypot(forward_x, forward_z) > 0.17)) return false;
    const double yaw = atan2(-forward_x, -forward_z); // rotation about +Y taking (0,0,-1) to the horizontal forward
    XrCalibration result = {.epoch = epoch, .heading_xyzw = {0, (float)sin(0.5 * yaw), 0, (float)cos(0.5 * yaw)}};
    for (unsigned i = 0; i < 3; ++i) {
        const double midpoint = 0.5 * ((double)left_xyz[i] + right_xyz[i]);
        if (!isfinite(midpoint)) return false;
        result.origin_xyz[i] = (float)midpoint;
    }
    *out = result;
    return true;
}

// in (x, y, z, w) normalized into out. False when it is not finite or not within 1% of unit length.
static bool unit_quaternion(const float in[4], double out[4]) {
    double length_squared = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(in[i])) return false;
        out[i] = in[i];
        length_squared += out[i] * out[i];
    }
    if (!(length_squared > 0.99 && length_squared < 1.01)) return false;
    for (unsigned i = 0; i < 4; ++i) out[i] /= sqrt(length_squared);
    return true;
}

// out = first * second (rotation second, then first), xyzw, as Khronos xr_linear.h.
static void quaternion_multiply(const double first[4], const double second[4], double out[4]) {
    out[0] = first[3] * second[0] + first[0] * second[3] + first[1] * second[2] - first[2] * second[1];
    out[1] = first[3] * second[1] - first[0] * second[2] + first[1] * second[3] + first[2] * second[0];
    out[2] = first[3] * second[2] + first[0] * second[1] - first[1] * second[0] + first[2] * second[3];
    out[3] = first[3] * second[3] - first[0] * second[0] - first[1] * second[1] - first[2] * second[2];
}

// out = vector rotated by the unit quaternion rotation: rotation * vector * inverse(rotation).
static void quaternion_rotate(const double rotation[4], const double vector[3], double out[3]) {
    const double as_quaternion[4] = {vector[0], vector[1], vector[2], 0};
    const double inverse[4] = {-rotation[0], -rotation[1], -rotation[2], rotation[3]};
    double half[4], rotated[4];
    quaternion_multiply(rotation, as_quaternion, half);
    quaternion_multiply(half, inverse, rotated);
    for (unsigned i = 0; i < 3; ++i) out[i] = rotated[i];
}

bool xr_calibration_anchor(const XrPoseSnapshot *current, XrPoseSnapshot *out) {
    if (!current || !out || !current->calibration.epoch || !current->native_views_valid) return false;
    double head[4], heading[4];
    if (!unit_quaternion(current->orientation_xyzw, head) ||
        !unit_quaternion(current->calibration.heading_xyzw, heading)) return false;
    const double inverse_head[4] = {-head[0], -head[1], -head[2], head[3]};
    XrPoseSnapshot result = *current;
    for (unsigned side = 0; side < 2; ++side) {
        double eye[4], local_rotation[4], placed_rotation[4], offset[3], local[3], placed[3];
        if (!unit_quaternion(current->eyes[side].orientation_xyzw, eye)) return false;
        for (unsigned i = 0; i < 3; ++i)
            offset[i] = (double)current->eyes[side].position_xyz[i] -
                0.5 * ((double)current->eyes[0].position_xyz[i] + current->eyes[1].position_xyz[i]);
        quaternion_rotate(inverse_head, offset, local);
        quaternion_rotate(heading, local, placed);
        quaternion_multiply(inverse_head, eye, local_rotation);
        quaternion_multiply(heading, local_rotation, placed_rotation);
        for (unsigned i = 0; i < 3; ++i) {
            const double position = (double)current->calibration.origin_xyz[i] + placed[i];
            if (!isfinite(position)) return false;
            result.eyes[side].position_xyz[i] = (float)position;
        }
        for (unsigned i = 0; i < 4; ++i) result.eyes[side].orientation_xyzw[i] = (float)placed_rotation[i];
    }
    for (unsigned i = 0; i < 4; ++i) result.orientation_xyzw[i] = (float)heading[i];
    *out = result;
    return true;
}

bool xr_bench_head_turn(uint64_t step, double *yaw, double *pitch) {
    const double pi = 3.14159265358979323846, degree = pi / 180;
    if (!yaw || !pitch || step >= XR_BENCH_HEAD_STEPS) return false;
    *yaw = *pitch = 0;
    if (step < 600) *yaw = 70 * degree * sin(2 * pi * (double)step / 600);
    else if (step < 900) *pitch = 25 * degree * sin(2 * pi * (double)(step - 600) / 300);
    else *yaw = 2 * pi * (double)(step - 900) / 600;
    return true;
}

bool xr_bench_head_apply(double yaw, double pitch, float head_xyzw[4], XrEyeSnapshot eyes[2]) {
    if (!head_xyzw || !eyes) return false;
    const double turn_yaw[4] = {0, sin(0.5 * yaw), 0, cos(0.5 * yaw)};
    const double turn_pitch[4] = {sin(0.5 * pitch), 0, 0, cos(0.5 * pitch)};
    double turn[4], head[4], eye[2][4];
    quaternion_multiply(turn_yaw, turn_pitch, turn);
    if (!unit_quaternion(head_xyzw, head) || !unit_quaternion(eyes[0].orientation_xyzw, eye[0]) ||
        !unit_quaternion(eyes[1].orientation_xyzw, eye[1])) return false;
    double center[3], turned[2][3], rotated[2][4], turned_head[4];
    for (unsigned i = 0; i < 3; ++i)
        center[i] = 0.5 * ((double)eyes[0].position_xyz[i] + eyes[1].position_xyz[i]);
    for (unsigned side = 0; side < 2; ++side) {
        double offset[3];
        for (unsigned i = 0; i < 3; ++i) offset[i] = (double)eyes[side].position_xyz[i] - center[i];
        quaternion_rotate(turn, offset, turned[side]);
        quaternion_multiply(turn, eye[side], rotated[side]);
        for (unsigned i = 0; i < 3; ++i) if (!isfinite(center[i] + turned[side][i])) return false;
    }
    quaternion_multiply(turn, head, turned_head);
    for (unsigned i = 0; i < 4; ++i) head_xyzw[i] = (float)turned_head[i];
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned i = 0; i < 3; ++i) eyes[side].position_xyz[i] = (float)(center[i] + turned[side][i]);
        for (unsigned i = 0; i < 4; ++i) eyes[side].orientation_xyzw[i] = (float)rotated[side][i];
    }
    return true;
}

bool xr_calibration_panel(const XrCalibration *calibration, float distance, float rise,
                          float position_xyz[3], float orientation_xyzw[4]) {
    if (!calibration || !calibration->epoch || !position_xyz || !orientation_xyzw ||
        !isfinite(distance) || !(distance > 0) || !isfinite(rise)) return false;
    double heading[4], placed[3];
    if (!unit_quaternion(calibration->heading_xyzw, heading)) return false;
    // The heading is a yaw: the rise stays vertical.
    quaternion_rotate(heading, (const double[3]){0, rise, -distance}, placed);
    float position[3];
    for (unsigned i = 0; i < 3; ++i) {
        const double coordinate = (double)calibration->origin_xyz[i] + placed[i];
        if (!isfinite(coordinate)) return false;
        position[i] = (float)coordinate;
    }
    memcpy(position_xyz, position, sizeof position);
    for (unsigned i = 0; i < 4; ++i) orientation_xyzw[i] = (float)heading[i];
    return true;
}

#ifdef SAVAGEWIND_WITH_OPENXR
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <SDL3/SDL_timer.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "../platform.h"
#include "../pc_vr_config.h"
#include <time.h>
#include <math.h>
#include "xr_vulkan.h"
#include "xr_feature_guard.h"

#define XRLOG(...) fprintf(stderr, "[pc-vr] " __VA_ARGS__)
// Mutually exclusive outcomes for successful EndFrame calls. These describe
// submitted layers, not a claim about the compositor's displayed pixels.
typedef enum {
    FRAME_IMAGE, FRAME_NO_RENDER, FRAME_NOT_VISIBLE, FRAME_REFERENCE, FRAME_TRACKING,
    FRAME_CANCELLED, FRAME_TERMINAL, FRAME_LAYER_AGE, FRAME_LAYER_FUTURE,
    FRAME_PRODUCER_PENDING, FRAME_NO_PAIR, FRAME_OUTCOME_COUNT
} FrameOutcome;
static const char *const frame_outcome_names[FRAME_OUTCOME_COUNT] = {
    "image", "should-not-render", "not-visible", "reference-change", "tracking-invalid",
    "reset-or-stop", "terminal", "layer-age", "future-pose", "producer-pending", "no-current-pair"
};
#define XR_PROCS(X) \
    X(DestroyInstance) X(GetInstanceProperties) X(GetSystem) \
    X(GetVulkanGraphicsRequirements2KHR) X(CreateVulkanInstanceKHR) \
    X(GetVulkanGraphicsDevice2KHR) X(CreateVulkanDeviceKHR) \
    X(EnumerateViewConfigurationViews) X(EnumerateEnvironmentBlendModes) \
    X(CreateSession) X(DestroySession) X(CreateReferenceSpace) X(DestroySpace) \
    X(PollEvent) X(BeginSession) X(EndSession) X(RequestExitSession) \
    X(WaitFrame) X(BeginFrame) X(EndFrame) X(LocateSpace) X(LocateViews) \
    X(EnumerateSwapchainFormats) X(CreateSwapchain) X(DestroySwapchain) \
    X(EnumerateSwapchainImages) X(AcquireSwapchainImage) X(WaitSwapchainImage) X(ReleaseSwapchainImage)

static struct {
#define DECLARE_PROC(name) PFN_xr##name name;
    XR_PROCS(DECLARE_PROC)
#undef DECLARE_PROC
    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSpace local, view;
    XrSessionState state;
    XrEnvironmentBlendMode blend;
    VkQueue queue;
    XrTime pending_reference_change;
    bool running, terminal, last_good, teardown_pending, runtime_queue_completion, display_rate_tracking;
    uint64_t queue_completion_waits, queue_completion_zero_layers, queue_completion_wait_ns, queue_completion_max_wait_ns;
    uint64_t frame_id;
    uint64_t outcomes[FRAME_OUTCOME_COUNT], visible_images;
    // D053: LOCAL's current definition and the calibration captured in it.
    uint64_t local_epoch;
    XrCalibration calibration;
    uint32_t max_eye_width, max_eye_height; // the runtime's per-view maximum (0: unknown)
    // SAVAGEWIND_PC_VR_BENCH_HEAD=<game present>: benchmark head script start (0: off) and the latest present seen.
    uint64_t bench_head_start, bench_last_present;
    pthread_t thread;
    bool thread_started;
} xr;

// Dedicated queue removes submit/present races. Device-wide idle still requires
// external synchronization with the runtime and therefore takes xr_vulkan_lock.
static pthread_mutex_t xr_vulkan_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t shutdown_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t pose_lock = PTHREAD_MUTEX_INITIALIZER;
static XrPoseSnapshot published;
static atomic_bool accepting, stop_requested;
static _Atomic uint64_t latest_present, reset_epoch, reference_generation = 1;
static uint64_t local_definitions; // never reused, so epochs differ across sessions
static PFN_vkCreateDevice forwarded_create_device;
static PFN_vkCreateInstance forwarded_create_instance;
static uint32_t requested_vulkan_api;
static unsigned successful_instance_creates;
static unsigned create_attempts, create_rejections, driver_creates;
static bool exit_cleanup_registered;
// D048 development admission of DEV_UNPROVEN eye pairs; never a default.
static bool eye_images_enabled;
static _Thread_local const char *current_xr_call;

uint64_t xr_runtime_pose_clock_ns(void) { return SDL_GetTicksNS(); }

const char *xr_runtime_current_call(void) {
    return current_xr_call ? current_xr_call : "outside-XR-call";
}

static XrResult complete_xr_call(XrResult result) {
    current_xr_call = NULL;
    return result;
}

// The comma operator sequences phase publication before the API call. No
// synchronization or logging is added to the frame path; only TLS metadata.
#define XR_CALL(phase, call) (current_xr_call = (phase), complete_xr_call(call))

static void lifecycle_marker(const char *phase, const char *status, int result) {
    XRLOG("lifecycle phase=%s status=%s result=%d\n", phase, status, result);
    fflush(stderr); // Preserve the final completed phase if native teardown faults.
}

static bool register_exit_cleanup(void) {
    if (exit_cleanup_registered) return true;
    // The statically linked Khronos loader constructs C++ function-local
    // singletons on its first calls. Register AFTER discovery so this callback
    // executes before their destructors, including on later Vulkan-init failure.
    if (atexit(xr_runtime_shutdown) != 0) return false;
    exit_cleanup_registered = true;
    return true;
}

static bool xr_ok(XrResult result, const char *operation) {
    if (XR_SUCCEEDED(result)) return true;
    XRLOG("%s failed: XrResult=%d\n", operation, result);
    return false;
}

// Destruction has only XR_SUCCESS as a defined successful result. Positive
// frame/wait statuses do not prove that a child handle was destroyed.
static bool xr_destroy_ok(XrResult result, const char *operation) {
    if (result == XR_SUCCESS) return true;
    XRLOG("%s did not complete: XrResult=%d; retaining handle\n", operation, result);
    return false;
}

static bool needs_runtime_queue_completion(const XrInstanceProperties *properties) {
    // Asset-free and game validation attribute pending command-buffer reuse to
    // this runtime's zero-layer and image EndFrame paths. Establish completion
    // before reuse; do not impose this compatibility wait on other versions.
    return !strcmp(properties->runtimeName, "Oculus") &&
        properties->runtimeVersion == XR_MAKE_VERSION(1, 208, 0);
}

static void invalidate_pose(const char *reason, bool advance_generation) {
    if (advance_generation) atomic_fetch_add(&reference_generation, 1);
    pthread_mutex_lock(&pose_lock);
    published.active = false;
    published.orientation_valid = false;
    published.orientation_tracked = false;
    published.views_valid = false;
    published.native_views_valid = false;
    published.angular_velocity_valid = false;
    memset(published.angular_velocity_xyz, 0, sizeof published.angular_velocity_xyz);
    published.generation = atomic_load(&reference_generation);
    pthread_mutex_unlock(&pose_lock);
    if (reason) XRLOG("tracking invalidated: %s generation=%llu\n", reason,
                      (unsigned long long)atomic_load(&reference_generation));
}

// Kept in the sole owner's translation unit: no second frame loop or queue
// owner is introduced. This header contains the GPU image handoff machinery.
#include "xr_perf.h"
static struct {
    bool enabled;
    uint64_t frames, first_game, last_game, new_images, repeated_images, pending;
    uint64_t missed;  // display periods xrWaitFrame's predicted times skipped (frames this loop lost)
    uint64_t shown[4];  // images that were shown for 1, 2, 3, 4 or more display frames in a row (the cadence)
    uint64_t skipped;   // finished pairs returned unshown because a newer one had finished (D081)
    XrPerfMetric wait, producer, swapchain, fence, end, completion, new_age, age;
} xr_perf;
static XrTime xr_perf_last_predicted;  // (kept across reports)
static unsigned xr_perf_showing;       // display frames the current image has been shown (kept across reports)

static void xr_perf_report(void) {
    if (!xr_perf.enabled || !xr_perf.frames) return;
    XRLOG("perf frames=%llu game=%llu:%llu new=%llu repeated=%llu pending=%llu missed=%llu shown=%llu/%llu/%llu/%llu skipped=%llu\n",
        (unsigned long long)xr_perf.frames, (unsigned long long)xr_perf.first_game,
        (unsigned long long)xr_perf.last_game, (unsigned long long)xr_perf.new_images,
        (unsigned long long)xr_perf.repeated_images, (unsigned long long)xr_perf.pending,
        (unsigned long long)xr_perf.missed, (unsigned long long)xr_perf.shown[0], (unsigned long long)xr_perf.shown[1],
        (unsigned long long)xr_perf.shown[2], (unsigned long long)xr_perf.shown[3], (unsigned long long)xr_perf.skipped);
#define XR_PERF_REPORT(field) do { \
    const XrPerfMetric *metric = &xr_perf.field; \
    XRLOG("perf " #field " n=%llu mean-ms=%.4f p95-bin-ms=%u p95-saturated=%d max-ms=%.4f\n", \
        (unsigned long long)metric->count, metric->count ? metric->sum_ns / (1e6 * metric->count) : 0, \
        xr_perf_p95(metric), xr_perf_p95(metric) == 512, metric->max_ns / 1e6); \
} while (0)
    XR_PERF_REPORT(wait); XR_PERF_REPORT(producer); XR_PERF_REPORT(swapchain); XR_PERF_REPORT(fence);
    XR_PERF_REPORT(end); XR_PERF_REPORT(completion); XR_PERF_REPORT(new_age); XR_PERF_REPORT(age);
#undef XR_PERF_REPORT
    memset(&xr_perf, 0, sizeof xr_perf);
    xr_perf.enabled = true;
}

#include "xr_images_internal.h"

static bool load_instance_functions(void) {
#define LOAD_PROC(name) do { \
    PFN_xrVoidFunction proc = NULL; \
    if (!xr_ok(xrGetInstanceProcAddr(xr.instance, "xr" #name, &proc), "load xr" #name) || !proc) return false; \
    xr.name = (PFN_xr##name)proc; \
} while (0);
    XR_PROCS(LOAD_PROC)
#undef LOAD_PROC
    return true;
}

bool xr_runtime_discover(void) {
    if (xr.teardown_pending) return false;
    if (xr.instance) return true;
    uint32_t count = 0;
    if (!xr_ok(xrEnumerateInstanceExtensionProperties(NULL, 0, &count, NULL), "enumerate extensions") ||
        !count || count > 256) return false;
    XrExtensionProperties *extensions = calloc(count, sizeof *extensions);
    if (!extensions) return false;
    for (uint32_t i = 0; i < count; ++i) extensions[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    XrResult result = xrEnumerateInstanceExtensionProperties(NULL, count, &count, extensions);
    bool enable2 = false;
    if (XR_SUCCEEDED(result)) for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) enable2 = true;
    free(extensions);
    if (!xr_ok(result, "enumerate extension data") || !enable2) {
        XRLOG("XR_KHR_vulkan_enable2 required; desktop fallback is disabled in PC VR mode\n");
        return false;
    }
    const char *enabled[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo info = {.type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {.applicationName = "Savage Wind PC VR", .applicationVersion = 1,
                            .engineName = "Savage Wind", .engineVersion = 1,
                            .apiVersion = XR_MAKE_VERSION(1, 0, 0)},
        .enabledExtensionCount = 1, .enabledExtensionNames = enabled};
    if (!xr_ok(xrCreateInstance(&info, &xr.instance), "xrCreateInstance")) return false;
    if (!load_instance_functions()) goto fail;
    XrInstanceProperties props = {.type = XR_TYPE_INSTANCE_PROPERTIES};
    if (!xr_ok(xr.GetInstanceProperties(xr.instance, &props), "xrGetInstanceProperties")) goto fail;
    XRLOG("runtime=%s version=%u.%u.%u\n", props.runtimeName,
          (unsigned)XR_VERSION_MAJOR(props.runtimeVersion), (unsigned)XR_VERSION_MINOR(props.runtimeVersion),
          (unsigned)XR_VERSION_PATCH(props.runtimeVersion));
    xr.runtime_queue_completion = needs_runtime_queue_completion(&props);
    memset(&xr_perf, 0, sizeof xr_perf);
    const PcVrConfig *config = pc_vr_config();
    xr_perf.enabled = config->diagnostics.perf;
    xr.display_rate_tracking = config->play.display_rate_tracking;
    xr.bench_head_start = config->diagnostics.bench_head_start;
    if (config->diagnostics.bench_head_requested)
        XRLOG("benchmark head script %s start-present=%llu steps=%u (published poses turned; never a play setting)\n",
              xr.bench_head_start ? "on" : "invalid, off", (unsigned long long)xr.bench_head_start, XR_BENCH_HEAD_STEPS);
    XRLOG("runtime queue completion=%s (measured runtime compatibility)\n", xr.runtime_queue_completion ? "on" : "off");
    XrSystemGetInfo system_info = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    if (!xr_ok(xr.GetSystem(xr.instance, &system_info, &xr.system), "xrGetSystem(HMD)")) goto fail;
    XrGraphicsRequirementsVulkan2KHR requirements = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!xr_ok(xr.GetVulkanGraphicsRequirements2KHR(xr.instance, xr.system, &requirements), "Vulkan requirements")) goto fail;
    const XrVersion requested = XR_MAKE_VERSION(1, 3, 0);
    XRLOG("Vulkan requested=1.3 minimum=%u.%u maximum-tested=%u.%u above-max-tested=%d\n",
          (unsigned)XR_VERSION_MAJOR(requirements.minApiVersionSupported), (unsigned)XR_VERSION_MINOR(requirements.minApiVersionSupported),
          (unsigned)XR_VERSION_MAJOR(requirements.maxApiVersionSupported), (unsigned)XR_VERSION_MINOR(requirements.maxApiVersionSupported),
          requested > requirements.maxApiVersionSupported);
    // maxApiVersionSupported is the runtime's maximum tested version, not an
    // absolute cap. The audited game profile needs 1.3; never downgrade it.
    if (requested < requirements.minApiVersionSupported) goto fail;
    uint32_t views = 0;
    if (!xr_ok(xr.EnumerateViewConfigurationViews(xr.instance, xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                0, &views, NULL), "stereo view configuration") || views != 2) goto fail;
    XrViewConfigurationView view_info[2] = {{.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}, {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (!xr_ok(xr.EnumerateViewConfigurationViews(xr.instance, xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                2, &views, view_info), "stereo view sizes") || views != 2) goto fail;
    // Eye images are the game's final-image halves (exec.c vr_capture_eyes), not this size; it is reported for comparison.
    xr.max_eye_width = view_info[0].maxImageRectWidth < view_info[1].maxImageRectWidth ?
        view_info[0].maxImageRectWidth : view_info[1].maxImageRectWidth;
    xr.max_eye_height = view_info[0].maxImageRectHeight < view_info[1].maxImageRectHeight ?
        view_info[0].maxImageRectHeight : view_info[1].maxImageRectHeight;
    XRLOG("view sizes recommended=%ux%u/%ux%u max=%ux%u\n",
          view_info[0].recommendedImageRectWidth, view_info[0].recommendedImageRectHeight,
          view_info[1].recommendedImageRectWidth, view_info[1].recommendedImageRectHeight,
          xr.max_eye_width, xr.max_eye_height);
    uint32_t blends = 0;
    if (!xr_ok(xr.EnumerateEnvironmentBlendModes(xr.instance, xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               0, &blends, NULL), "blend modes") || !blends || blends > 16) goto fail;
    XrEnvironmentBlendMode modes[16];
    if (!xr_ok(xr.EnumerateEnvironmentBlendModes(xr.instance, xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               blends, &blends, modes), "blend modes data")) goto fail;
    xr.blend = modes[0];
    for (uint32_t i = 0; i < blends; ++i) if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) xr.blend = modes[i];
    if (!register_exit_cleanup()) {
        XRLOG("could not register OpenXR cleanup before loader static destruction\n");
        goto fail;
    }
    return true;
fail:
    xr_runtime_shutdown();
    return false;
}

static VKAPI_ATTR VkResult VKAPI_CALL guarded_create_instance(const VkInstanceCreateInfo *info,
        const VkAllocationCallbacks *allocator, VkInstance *instance) {
    const uint32_t api = info && info->pApplicationInfo ? info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    XRLOG("runtime-forwarded Vulkan instance api=%u.%u.%u extensions=%u\n", VK_API_VERSION_MAJOR(api),
          VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api), info ? info->enabledExtensionCount : 0);
    if (!info || !forwarded_create_instance || info->enabledExtensionCount > 256 ||
        api < requested_vulkan_api || api < VK_API_VERSION_1_3 || VK_API_VERSION_VARIANT(api) != 0) {
        XRLOG("rejected runtime instance profile before driver: Vulkan 1.3 game request must be preserved\n");
        if (instance) *instance = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult result = forwarded_create_instance(info, allocator, instance);
    if (result == VK_SUCCESS) ++successful_instance_creates;
    return result;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL guarded_instance_get_proc(VkInstance instance, const char *name) {
    PFN_vkVoidFunction proc = vkGetInstanceProcAddr(instance, name);
    if (proc && !strcmp(name, "vkCreateInstance")) {
        forwarded_create_instance = (PFN_vkCreateInstance)proc;
        return (PFN_vkVoidFunction)guarded_create_instance;
    }
    return proc;
}

VkResult xr_runtime_create_vulkan_instance(const VkInstanceCreateInfo *info, VkInstance *instance) {
    if (!xr.instance || !xr.CreateVulkanInstanceKHR) return VK_ERROR_INITIALIZATION_FAILED;
    requested_vulkan_api = info && info->pApplicationInfo ? info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
    successful_instance_creates = 0;
    XrVulkanInstanceCreateInfoKHR create = {.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
        .systemId = xr.system, .pfnGetInstanceProcAddr = guarded_instance_get_proc, .vulkanCreateInfo = info};
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    XrResult xr_result = XR_CALL("xrCreateVulkanInstanceKHR", xr.CreateVulkanInstanceKHR(xr.instance, &create, instance, &result));
    XRLOG("xrCreateVulkanInstanceKHR xr=%d vk=%d\n", xr_result, result);
    return XR_SUCCEEDED(xr_result) && successful_instance_creates ? result : VK_ERROR_INITIALIZATION_FAILED;
}

bool xr_runtime_select_vulkan_device(VkInstance instance, VkPhysicalDevice *physical) {
    XrVulkanGraphicsDeviceGetInfoKHR info = {.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
        .systemId = xr.system, .vulkanInstance = instance};
    return xr.instance && xr.GetVulkanGraphicsDevice2KHR &&
        xr_ok(xr.GetVulkanGraphicsDevice2KHR(xr.instance, &info, physical), "runtime-selected Vulkan GPU") && *physical;
}

static VKAPI_ATTR VkResult VKAPI_CALL guarded_create_device(VkPhysicalDevice physical,
        const VkDeviceCreateInfo *info, const VkAllocationCallbacks *allocator, VkDevice *device) {
    ++create_attempts;
    if (!xr_vulkan_feature_chain_valid(info)) {
        ++create_rejections;
        XRLOG("Vulkan device attempt=%u rejected invalid runtime feature chain before driver\n", create_attempts);
        if (device) *device = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!forwarded_create_device) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = forwarded_create_device(physical, info, allocator, device);
    if (result == VK_SUCCESS) ++driver_creates;
    XRLOG("Vulkan device attempt=%u driver result=%d\n", create_attempts, result);
    return result;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL guarded_get_proc(VkInstance instance, const char *name) {
    PFN_vkVoidFunction proc = vkGetInstanceProcAddr(instance, name);
    if (proc && !strcmp(name, "vkCreateDevice")) {
        forwarded_create_device = (PFN_vkCreateDevice)proc;
        return (PFN_vkVoidFunction)guarded_create_device;
    }
    return proc;
}

VkResult xr_runtime_create_vulkan_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info, VkDevice *device) {
    if (!xr.instance || !xr.CreateVulkanDeviceKHR) return VK_ERROR_INITIALIZATION_FAILED;
    if (!xr_vulkan_feature_chain_valid(info)) return VK_ERROR_INITIALIZATION_FAILED;
    create_attempts = create_rejections = driver_creates = 0;
    XrVulkanDeviceCreateInfoKHR create = {.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
        .systemId = xr.system, .pfnGetInstanceProcAddr = guarded_get_proc,
        .vulkanPhysicalDevice = physical, .vulkanCreateInfo = info};
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    XrResult xr_result = XR_CALL("xrCreateVulkanDeviceKHR", xr.CreateVulkanDeviceKHR(xr.instance, &create, device, &result));
    XRLOG("shared-device admission xr=%d vk=%d attempts=%u rejected=%u driver-success=%u\n",
          xr_result, result, create_attempts, create_rejections, driver_creates);
    if (XR_FAILED(xr_result) || !driver_creates) return VK_ERROR_INITIALIZATION_FAILED;
    return result;
}

// Called only by the owner (and during its final shutdown), under Vulkan lock.
static void poll_events(void) {
    for (unsigned event = 0; event < 64; ++event) {
        XrEventDataBuffer event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result = XR_CALL("xrPollEvent", xr.PollEvent(xr.instance, &event));
        if (result == XR_EVENT_UNAVAILABLE) return;
        if (!xr_ok(result, "xrPollEvent")) {
            xr.terminal = true;
            invalidate_pose("event failure", true);
            return;
        }
        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            xr.terminal = true;
            invalidate_pose("instance loss; restart required", true);
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const XrEventDataReferenceSpaceChangePending *change = (const void *)&event;
            if (change->session == xr.session && change->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                // Invalidate now; rebase only once the runtime's change is in force.
                xr.pending_reference_change = change->changeTime;
                invalidate_pose("reference-space change pending", true);
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_EVENTS_LOST) {
            invalidate_pose("events lost", true);
        } else if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *change = (const void *)&event;
            if (change->session != xr.session) continue;
            XrSessionState old = xr.state;
            xr.state = change->state;
            XRLOG("session state=%d previous=%d\n", xr.state, old);
            if (old != xr.state) invalidate_pose(NULL, true);
            if (xr.state == XR_SESSION_STATE_READY && !xr.running && !atomic_load(&stop_requested)) {
                XrSessionBeginInfo begin = {.type = XR_TYPE_SESSION_BEGIN_INFO,
                    .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                if (xr_ok(XR_CALL("xrBeginSession", xr.BeginSession(xr.session, &begin)), "xrBeginSession")) {
                    xr.running = true;
                    xr_perf_last_predicted = 0;  // (a stopped session's gap is no missed frame)
                }
                else xr.terminal = true;
            } else if (xr.state == XR_SESSION_STATE_STOPPING && xr.running) {
                if (xr.queue) {
                    lifecycle_marker("queue-idle-before-end-session", "begin", 0);
                    const VkResult idle = vkQueueWaitIdle(xr.queue);
                    lifecycle_marker("queue-idle-before-end-session", "end", idle);
                    if (idle != VK_SUCCESS) XRLOG("reserved XR queue idle before session end failed: %d\n", idle);
                }
                lifecycle_marker("xrEndSession", "begin", 0);
                const XrResult ended = XR_CALL("xrEndSession", xr.EndSession(xr.session));
                lifecycle_marker("xrEndSession", "end", ended);
                if (!xr_ok(ended, "xrEndSession")) xr.terminal = true;
                xr.running = false;
            } else if (xr.state == XR_SESSION_STATE_EXITING || xr.state == XR_SESSION_STATE_LOSS_PENDING) {
                xr.terminal = true;
                invalidate_pose("session exit/loss; restart required", true);
            }
        }
    }
}

// xrWaitFrame paces the owner at display rate. It never touches the Vulkan
// queue (XR_KHR_vulkan_enable2 external-sync list), so the owner calls it
// without xr_vulkan_lock and device-wide idle never waits a display period.
static bool wait_frame(XrFrameState *frame) {
    XrFrameWaitInfo wait = {.type = XR_TYPE_FRAME_WAIT_INFO};
    *frame = (XrFrameState){.type = XR_TYPE_FRAME_STATE};
    const uint64_t began = xr_perf.enabled ? SDL_GetTicksNS() : 0;
    const bool ok = xr_ok(XR_CALL("xrWaitFrame", xr.WaitFrame(xr.session, &wait, frame)), "xrWaitFrame");
    if (xr_perf.enabled) {
        xr_perf_add(&xr_perf.wait, SDL_GetTicksNS() - began);
        const XrDuration period = frame->predictedDisplayPeriod;
        if (ok && period > 0 && xr_perf_last_predicted && frame->predictedDisplayTime > xr_perf_last_predicted) {
            const int64_t periods = (frame->predictedDisplayTime - xr_perf_last_predicted + period / 2) / period;
            if (periods > 1) xr_perf.missed += (uint64_t)(periods - 1);
        }
        if (ok) xr_perf_last_predicted = frame->predictedDisplayTime;
    }
    return ok;
}

static void frame_failed(void) {
    xr.terminal = true;
    invalidate_pose("frame lifecycle failed; restart required", true);
}

// One display frame, under xr_vulkan_lock, after a successful xrWaitFrame:
//  1. begin the frame (applying a pending LOCAL redefinition: calibrate again);
//  2. locate the head and both eyes at the predicted display time, and capture the D053 calibration on the first
//     fully tracked frame in this LOCAL;
//  3. build the layers from the latest admitted eye pair (eye_submit_layers: every display frame re-submits it) and
//     end the frame, counting its outcome;
//  4. publish the tracking sample. Only a frame carrying a game present normally publishes one; the display-rate
//     experiment refreshes it every display frame. The camera code keeps one complete sample for both eyes of each
//     game frame.
static void run_frame(const XrFrameState *waited, uint64_t game_present_id) {
    const uint64_t epoch = atomic_load(&reset_epoch);
    const XrFrameState frame = *waited;
    ++xr.frame_id;
    // 1. Begin.
    XrFrameBeginInfo begin = {.type = XR_TYPE_FRAME_BEGIN_INFO};
    if (!xr_ok(XR_CALL("xrBeginFrame", xr.BeginFrame(xr.session, &begin)), "xrBeginFrame")) goto fail;
    if (xr.pending_reference_change && frame.predictedDisplayTime >= xr.pending_reference_change) {
        xr.pending_reference_change = 0;
        // LOCAL is redefined (the runtime's recenter): calibrate again in it.
        xr.local_epoch = ++local_definitions;
        xr.calibration = (XrCalibration){0};
        invalidate_pose("reference-space change applied", true);
    }
    // 2. Locate. (Once begun, this frame is always ended, with invalid tracking and on a reset too.)
    const uint64_t located_generation = atomic_load(&reference_generation);
    XrSpaceVelocity velocity = {.type = XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation head = {.type = XR_TYPE_SPACE_LOCATION, .next = &velocity};
    XrResult located = XR_CALL("xrLocateSpace", xr.LocateSpace(xr.view, xr.local, frame.predictedDisplayTime, &head));
    XrViewLocateInfo locate = {.type = XR_TYPE_VIEW_LOCATE_INFO,
        .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        .displayTime = frame.predictedDisplayTime, .space = xr.local};
    XrViewState view_state = {.type = XR_TYPE_VIEW_STATE};
    XrView views[2] = {{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
    uint32_t view_count = 0;
    XrResult views_result = XR_CALL("xrLocateViews", xr.LocateViews(xr.session, &locate, &view_state, 2, &view_count, views));
    const uint64_t located_monotonic_ns = xr_runtime_pose_clock_ns();
    // Visibility permits rendering behind runtime overlays; focus only
    // describes eligibility for XR input, not tracking or image admission.
    const bool visible = xr.state == XR_SESSION_STATE_VISIBLE || xr.state == XR_SESSION_STATE_FOCUSED;
    bool valid = XR_SUCCEEDED(located) && XR_SUCCEEDED(views_result) && view_count == 2 &&
        (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
        (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) &&
        (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
        visible && frame.shouldRender && !xr.pending_reference_change;
    const XrViewStateFlags native_flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
        XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    const bool native_views_valid = valid && (view_state.viewStateFlags & native_flags) == native_flags;
    if (XR_SUCCEEDED(views_result) && visible) {
        // Which tracking the runtime reports for the eyes, logged as it changes, so a run's log says whether it had
        // 6DoF (rotation and position tracked) or 3DoF (the headset woke on a desk: position not tracked).
        static XrViewStateFlags logged_flags = ~(XrViewStateFlags)0;
        static unsigned logged;
        const XrViewStateFlags flags = view_state.viewStateFlags & native_flags;
        if (flags != logged_flags && logged < 32) {
            ++logged;
            logged_flags = flags;
            XRLOG("tracking mode: %s (orientation valid %d tracked %d, position valid %d tracked %d; head flags %#x) "
                  "frame=%llu\n", flags == native_flags ? "6DoF" : "3DoF or less",
                  !!(flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT), !!(flags & XR_VIEW_STATE_ORIENTATION_TRACKED_BIT),
                  !!(flags & XR_VIEW_STATE_POSITION_VALID_BIT), !!(flags & XR_VIEW_STATE_POSITION_TRACKED_BIT),
                  (unsigned)head.locationFlags, (unsigned long long)xr.frame_id);
        }
    }
    if (native_views_valid && xr.local_epoch && xr.calibration.epoch != xr.local_epoch) {
        const float head_xyzw[4] = {head.pose.orientation.x, head.pose.orientation.y,
                                    head.pose.orientation.z, head.pose.orientation.w};
        const float left[3] = {views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z};
        const float right[3] = {views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z};
        if (xr_calibration_capture(head_xyzw, left, right, xr.local_epoch, &xr.calibration))
            XRLOG("calibration epoch=%llu heading_deg=%.1f origin=%.3f,%.3f,%.3f; kept until LOCAL is redefined\n",
                (unsigned long long)xr.calibration.epoch,
                2 * atan2(xr.calibration.heading_xyzw[1], xr.calibration.heading_xyzw[3]) * (180.0 / 3.14159265358979323846),
                xr.calibration.origin_xyz[0], xr.calibration.origin_xyz[1], xr.calibration.origin_xyz[2]);
    }
    // 3. Submit the latest eye pair's layers (none when there is no valid pair or tracking), then end the frame. The
    // submission is dropped if a reset, a stop or a reference change came in meanwhile.
    XrCompositionLayerProjection projection = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader *layers[2] = {NULL, NULL};
    const uint64_t copied_before = eye_gpu.copied;
    const uint32_t layer_count = eye_submit_layers(frame.predictedDisplayTime, native_views_valid, &projection, layers);
    bool image_submitted = layer_count > 0;
    const bool image_lock_held = image_submitted;
    if (image_lock_held) {
        pthread_mutex_lock(&eye_submission_lock);
        image_submitted = epoch == atomic_load(&reset_epoch) &&
            located_generation == atomic_load(&reference_generation) && !atomic_load(&stop_requested);
    }
    XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = frame.predictedDisplayTime,
        .environmentBlendMode = xr.blend, .layerCount = image_submitted ? layer_count : 0, .layers = image_submitted ? layers : NULL};
    const uint64_t end_began = xr_perf.enabled ? SDL_GetTicksNS() : 0;
    const XrResult ended = XR_CALL(image_submitted ? "xrEndFrame(game-eyes)" : "xrEndFrame(zero-layers)",
        xr.EndFrame(xr.session, &end));
    if (xr_perf.enabled) xr_perf_add(&xr_perf.end, SDL_GetTicksNS() - end_began);
    if (image_lock_held) pthread_mutex_unlock(&eye_submission_lock);
    if (!xr_ok(ended, "xrEndFrame")) goto fail;
    FrameOutcome outcome = FRAME_IMAGE;
    if (!image_submitted) {
        if (xr.terminal) outcome = FRAME_TERMINAL;
        else if (epoch != atomic_load(&reset_epoch) || atomic_load(&stop_requested)) outcome = FRAME_CANCELLED;
        else if (!frame.shouldRender) outcome = FRAME_NO_RENDER;
        else if (!visible) outcome = FRAME_NOT_VISIBLE;
        else if (xr.pending_reference_change || located_generation != atomic_load(&reference_generation)) outcome = FRAME_REFERENCE;
        else if (!native_views_valid) outcome = FRAME_TRACKING;
        else outcome = eye_gpu.zero_outcome;
    }
    ++xr.outcomes[outcome];
    if (image_submitted && xr.state == XR_SESSION_STATE_VISIBLE) ++xr.visible_images;
    if (image_submitted) ++eye_gpu.submitted;
    if (xr_perf.enabled) {
        ++xr_perf.frames;
        if (game_present_id) {
            if (!xr_perf.first_game) xr_perf.first_game = game_present_id;
            xr_perf.last_game = game_present_id;
        }
        if (image_submitted) {
            const uint64_t now = xr_runtime_pose_clock_ns();
            if (now >= eye_gpu.layer_published_ns) {
                const uint64_t age = now - eye_gpu.layer_published_ns;
                xr_perf_add(&xr_perf.age, age);
                if (eye_gpu.copied != copied_before) xr_perf_add(&xr_perf.new_age, age);
            }
            if (eye_gpu.copied != copied_before) {
                ++xr_perf.new_images;
                if (xr_perf_showing) ++xr_perf.shown[(xr_perf_showing < 4 ? xr_perf_showing : 4) - 1];
                xr_perf_showing = 1;
            } else {
                ++xr_perf.repeated_images;
                if (xr_perf_showing) ++xr_perf_showing;
            }
        } else xr_perf_showing = 0;  // (nothing shown: the run of display frames ends uncounted)
    }
    if (xr.runtime_queue_completion) {
        // xr_vulkan_lock is held and EndFrame has completed. No image ownership
        // or game update changes: establish completion before this runtime can
        // recycle its private command buffers on the following frame.
        const uint64_t began = SDL_GetTicksNS();
        const VkResult completed = vkQueueWaitIdle(xr.queue);
        const uint64_t elapsed = SDL_GetTicksNS() - began;
        ++xr.queue_completion_waits;
        if (!image_submitted) ++xr.queue_completion_zero_layers;
        xr.queue_completion_wait_ns += elapsed;
        if (xr_perf.enabled) xr_perf_add(&xr_perf.completion, elapsed);
        if (elapsed > xr.queue_completion_max_wait_ns) xr.queue_completion_max_wait_ns = elapsed;
        if (completed != VK_SUCCESS) {
            XRLOG("runtime queue completion failed: %d; restart required\n", completed);
            frame_failed();
            return; // EndFrame already succeeded and its outcome was counted.
        }
    }
    if (xr_perf.frames >= 120) xr_perf_report();
    const char *provenance = !image_submitted ? "none" : eye_gpu.layer_panel ? "dev-unproven-panel" :
        eye_gpu.layer_over ? "dev-unproven-over-scene" : eye_gpu.layer_hud ? "dev-unproven-hud" :
        eye_gpu.layer_projection == XR_EYE_PROJECTION_DEV_UNPROVEN ? "dev-unproven" : "native-rendered";
    // 4. Publish the tracking sample. Poll focus and loss changes first, so a just-lost session cannot leave a valid
    // sample until the next game present.
    poll_events();
    valid = valid && xr.running && !xr.terminal &&
        (xr.state == XR_SESSION_STATE_VISIBLE || xr.state == XR_SESSION_STATE_FOCUSED) &&
        !atomic_load(&stop_requested) && epoch == atomic_load(&reset_epoch) &&
        !xr.pending_reference_change && located_generation == atomic_load(&reference_generation);
    if (!game_present_id) {
        if (xr.frame_id <= 5 || xr.frame_id % 120 == 0)
            XRLOG("display frame=%llu predicted=%lld layers=%d provenance=%s game-eye-submissions=%llu copied-pairs=%llu\n",
                (unsigned long long)xr.frame_id, (long long)frame.predictedDisplayTime, image_submitted, provenance,
                (unsigned long long)eye_gpu.submitted, (unsigned long long)eye_gpu.copied);
        if (!xr.display_rate_tracking) return;
    }
    if (valid != xr.last_good) {
        invalidate_pose(valid ? "tracking recovered; calibration kept" : "tracking unavailable", true);
        xr.last_good = valid;
        if (valid && native_views_valid)
            XRLOG("eye views fov_lrud=%.4f,%.4f,%.4f,%.4f/%.4f,%.4f,%.4f,%.4f separation=%.5f\n",
                views[0].fov.angleLeft, views[0].fov.angleRight, views[0].fov.angleUp, views[0].fov.angleDown,
                views[1].fov.angleLeft, views[1].fov.angleRight, views[1].fov.angleUp, views[1].fov.angleDown,
                sqrt(pow(views[1].pose.position.x - views[0].pose.position.x, 2) +
                     pow(views[1].pose.position.y - views[0].pose.position.y, 2) +
                     pow(views[1].pose.position.z - views[0].pose.position.z, 2)));
    }
    XrPoseSnapshot snapshot = {.frame_id = xr.frame_id, .game_present_id = game_present_id,
        .generation = atomic_load(&reference_generation), .published_monotonic_ns = located_monotonic_ns,
        .predicted_display_time_ns = frame.predictedDisplayTime,
        .orientation_xyzw = {head.pose.orientation.x, head.pose.orientation.y, head.pose.orientation.z, head.pose.orientation.w},
        .active = valid, .focused = xr.state == XR_SESSION_STATE_FOCUSED,
        .orientation_valid = valid, .orientation_tracked = valid, .views_valid = valid,
        .native_views_valid = valid && native_views_valid, .calibration = xr.calibration};
    // The runtime owns velocity estimation. Never inspect members without the
    // corresponding flag, and never publish a rate from a discarded pose/frame.
    if (valid && (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) &&
        isfinite(velocity.angularVelocity.x) && isfinite(velocity.angularVelocity.y) &&
        isfinite(velocity.angularVelocity.z)) {
        snapshot.angular_velocity_xyz[0] = velocity.angularVelocity.x;
        snapshot.angular_velocity_xyz[1] = velocity.angularVelocity.y;
        snapshot.angular_velocity_xyz[2] = velocity.angularVelocity.z;
        snapshot.angular_velocity_valid = true;
    }
    for (unsigned i = 0; i < 2; ++i) {
        snapshot.eyes[i] = (XrEyeSnapshot){
            .orientation_xyzw = {views[i].pose.orientation.x, views[i].pose.orientation.y,
                                 views[i].pose.orientation.z, views[i].pose.orientation.w},
            .position_xyz = {views[i].pose.position.x, views[i].pose.position.y, views[i].pose.position.z},
            .fov_left_right_up_down = {views[i].fov.angleLeft, views[i].fov.angleRight,
                                       views[i].fov.angleUp, views[i].fov.angleDown}};
    }
    if (game_present_id) xr.bench_last_present = game_present_id;
    if (xr.bench_head_start && valid && xr.bench_last_present >= xr.bench_head_start) {
        // Benchmark only: turn the sample before anything reads it. Calibration above used the real pose; the
        // eyes rendered with this sample are submitted with it, and the compositor reprojects to the real head.
        const uint64_t step = xr.bench_last_present - xr.bench_head_start;
        double yaw, pitch;
        if (xr_bench_head_turn(step, &yaw, &pitch)) {
            // The headset's rate is not the script's: no consumer may extrapolate the turned pose with it.
            snapshot.angular_velocity_valid = false;
            memset(snapshot.angular_velocity_xyz, 0, sizeof snapshot.angular_velocity_xyz);
            if (!xr_bench_head_apply(yaw, pitch, snapshot.orientation_xyzw, snapshot.eyes)) {
                snapshot.active = snapshot.orientation_valid = snapshot.orientation_tracked = false;
                snapshot.views_valid = snapshot.native_views_valid = false;
            } else if (step == 0 || step == 600 || step == 900 || step == XR_BENCH_HEAD_STEPS - 1)
                XRLOG("benchmark head step=%llu present=%llu yaw-deg=%.1f pitch-deg=%.1f\n", (unsigned long long)step,
                      (unsigned long long)xr.bench_last_present, yaw * 57.29577951308232, pitch * 57.29577951308232);
        }
    }
    pthread_mutex_lock(&pose_lock);
    if (epoch == atomic_load(&reset_epoch) && !atomic_load(&stop_requested)) published = snapshot;
    pthread_mutex_unlock(&pose_lock);
    if (xr.frame_id <= 5 || xr.frame_id % 120 == 0)
        XRLOG("tracking frame=%llu game-present=%llu predicted=%lld generation=%llu valid=%d angular-velocity-valid=%d zero-layers=%d provenance=%s game-eye-submissions=%llu copied-pairs=%llu\n",
            (unsigned long long)xr.frame_id, (unsigned long long)game_present_id, (long long)frame.predictedDisplayTime,
            (unsigned long long)snapshot.generation, valid, snapshot.angular_velocity_valid, !image_submitted, provenance,
            (unsigned long long)eye_gpu.submitted, (unsigned long long)eye_gpu.copied);
    return;
fail:
    frame_failed();
}

// One complete frame on the calling thread (fixtures and direct callers).
static void tracking_frame(uint64_t game_present_id) {
    XrFrameState frame;
    if (!wait_frame(&frame)) {
        frame_failed();
        return;
    }
    run_frame(&frame, game_present_id);
}

static void *frame_owner(void *unused) {
    (void)unused;
    while (!atomic_load(&stop_requested)) {
        pthread_mutex_lock(&xr_vulkan_lock);
        poll_events();
        eye_retire_obsolete();
        if (xr.terminal) {
            pthread_mutex_unlock(&xr_vulkan_lock);
            break;
        }
        const bool running = xr.running;
        pthread_mutex_unlock(&xr_vulkan_lock);
        if (running) {
            // Display cadence, independent of the game's frame rate. Game
            // presents only select which frame publishes a tracking sample.
            XrFrameState frame;
            const bool waited = wait_frame(&frame);
            pthread_mutex_lock(&xr_vulkan_lock);
            if (!waited) frame_failed();
            else run_frame(&frame, atomic_exchange(&latest_present, 0));
            pthread_mutex_unlock(&xr_vulkan_lock);
            continue;
        }
        // Before the session runs, the timeout services session events. It
        // never authorizes a frame or pose sample.
        struct timespec deadline;
        timespec_get(&deadline, TIME_UTC);
        deadline.tv_nsec += 20000000;
        if (deadline.tv_nsec >= 1000000000) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000;
        }
        pthread_mutex_lock(&wake_lock);
        if (!atomic_load(&stop_requested)) pthread_cond_timedwait(&wake_cond, &wake_lock, &deadline);
        pthread_mutex_unlock(&wake_lock);
    }
    atomic_store(&accepting, false);
    invalidate_pose("frame owner stopped", true);
    pthread_mutex_lock(&xr_vulkan_lock);
    if (xr.running && !xr.terminal) {
        lifecycle_marker("xrRequestExitSession", "begin", 0);
        const XrResult requested = XR_CALL("xrRequestExitSession", xr.RequestExitSession(xr.session));
        lifecycle_marker("xrRequestExitSession", "end", requested);
        if (xr_ok(requested, "xrRequestExitSession")) {
            const uint64_t until = SDL_GetTicksNS() + 500000000;
            do {
                poll_events();
                if (!xr.running || xr.terminal) break;
                SDL_Delay(5);
            } while (SDL_GetTicksNS() < until);
        }
    }
    pthread_mutex_unlock(&xr_vulkan_lock);
    return NULL;
}

bool xr_runtime_bind_vulkan(VkInstance instance, VkPhysicalDevice physical,
                            VkDevice device, uint32_t queue_family, uint32_t queue_index) {
    if (!xr.instance || !device || xr.session || xr.teardown_pending) return false;
    if (!eye_gpu_bind(device, queue_family)) return false;
    const PcVrConfig *config = pc_vr_config();
    eye_images_enabled = config->enabled;  // (the play configuration has the eye images)
    atomic_store(&eye_offer_trace, config->diagnostics.offer_trace);
    XRLOG("eye admission=%s\n", eye_images_enabled ?
          "strict+dev-unproven (the PC VR play configuration; not a production rule)" : "strict");
    vkGetDeviceQueue(device, queue_family, queue_index, &xr.queue);
    if (!xr.queue) return false;
    XrGraphicsBindingVulkan2KHR binding = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = instance, .physicalDevice = physical, .device = device,
        .queueFamilyIndex = queue_family, .queueIndex = queue_index};
    XrSessionCreateInfo session_info = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = xr.system};
    if (!xr_ok(XR_CALL("xrCreateSession", xr.CreateSession(xr.instance, &session_info, &xr.session)), "xrCreateSession(shared game device)")) goto fail;
    XrReferenceSpaceCreateInfo space = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL, .poseInReferenceSpace = {.orientation = {.w = 1}}};
    if (!xr_ok(xr.CreateReferenceSpace(xr.session, &space, &xr.local), "create LOCAL space")) goto fail;
    xr.local_epoch = ++local_definitions;
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!xr_ok(xr.CreateReferenceSpace(xr.session, &space, &xr.view), "create VIEW space")) goto fail;
    atomic_store(&stop_requested, false);
    atomic_store(&accepting, true);
    if (pthread_create(&xr.thread, NULL, frame_owner, NULL) != 0) {
        XRLOG("frame-owner thread creation failed\n");
        goto fail;
    }
    xr.thread_started = true;
    XRLOG("shared game Vulkan session created queue-family=%u reserved-queue=%u; display-rate frame loop, tracking published per %s\n",
          queue_family, queue_index, xr.display_rate_tracking ? "display frame" : "game present");
    return true;
fail:
    xr_runtime_shutdown();
    return false;
}

void xr_runtime_notify_game_present(uint64_t game_present_id) {
    if (!game_present_id || !atomic_load(&accepting)) return;
    atomic_store(&latest_present, game_present_id);
    pthread_cond_signal(&wake_cond);
}

XrPoseReadResult xr_runtime_get_pose(XrPoseSnapshot *out) {
    if (!out) return XR_POSE_UNAVAILABLE;
    memset(out, 0, sizeof *out);
    if (pthread_mutex_trylock(&pose_lock) != 0) return XR_POSE_BUSY;
    *out = published;
    pthread_mutex_unlock(&pose_lock);
    const uint64_t now = xr_runtime_pose_clock_ns();
    if (!out->active || !out->orientation_valid || !out->orientation_tracked ||
        now < out->published_monotonic_ns ||
        now - out->published_monotonic_ns > 250000000 || !atomic_load(&accepting)) {
        out->angular_velocity_valid = false;
        memset(out->angular_velocity_xyz, 0, sizeof out->angular_velocity_xyz);
        return XR_POSE_UNAVAILABLE;
    }
    return XR_POSE_VALID;
}

void xr_runtime_reset_game(void) {
    pthread_mutex_lock(&eye_submission_lock);
    atomic_fetch_add(&reset_epoch, 1);
    atomic_store(&latest_present, 0);
    invalidate_pose("game reset", true);
    pthread_mutex_unlock(&eye_submission_lock);
}

bool xr_runtime_shutdown_checked(void) {
    // A normal UI close calls this before exit(); late and early exit guards may
    // call it again. An empty owner must not touch a destructed loader singleton.
    pthread_mutex_lock(&shutdown_lock);
    if (!xr.instance && !xr.thread_started && !xr.queue) {
        pthread_mutex_unlock(&shutdown_lock);
        return true;
    }
    xr.teardown_pending = true;
    atomic_store(&accepting, false);
    atomic_store(&stop_requested, true);
    pthread_cond_signal(&wake_cond);
    if (xr.thread_started) {
        lifecycle_marker("join-frame-owner", "begin", 0);
        const int joined = pthread_join(xr.thread, NULL);
        lifecycle_marker("join-frame-owner", "end", joined);
        if (joined != 0) {
            XRLOG("owner join failed; refusing to destroy a possibly live session\n");
            pthread_mutex_unlock(&shutdown_lock);
            return false;
        }
        xr.thread_started = false;
    }
    xr_perf_report(); // Owner joined; include the partial final telemetry window.
    lifecycle_marker("shutdown-vulkan-lock", "begin", 0);
    pthread_mutex_lock(&xr_vulkan_lock);
    lifecycle_marker("shutdown-vulkan-lock", "end", 0);
    if (xr.frame_id) {
        XRLOG("frame outcomes frames=%llu visible-images=%llu", (unsigned long long)xr.frame_id,
              (unsigned long long)xr.visible_images);
        for (unsigned i = 0; i < FRAME_OUTCOME_COUNT; ++i)
            fprintf(stderr, " %s=%llu", frame_outcome_names[i], (unsigned long long)xr.outcomes[i]);
        fprintf(stderr, "\n");
        XRLOG("runtime queue completion waits=%llu zero-layer=%llu total-ns=%llu max-ns=%llu\n",
            (unsigned long long)xr.queue_completion_waits, (unsigned long long)xr.queue_completion_zero_layers,
            (unsigned long long)xr.queue_completion_wait_ns,
            (unsigned long long)xr.queue_completion_max_wait_ns);
    }
    bool cleanup_ok = true;
    if (xr.queue) {
        lifecycle_marker("queue-idle-before-destroy", "begin", 0);
        const VkResult idle = vkQueueWaitIdle(xr.queue);
        lifecycle_marker("queue-idle-before-destroy", "end", idle);
        if (idle != VK_SUCCESS) {
            XRLOG("reserved XR queue idle before destruction failed: %d\n", idle);
            pthread_mutex_unlock(&xr_vulkan_lock);
            pthread_mutex_unlock(&shutdown_lock);
            return false;
        }
    }
    if (!eye_gpu_shutdown()) {
        XRLOG("eye-image completion/cleanup unresolved; retaining owned GPU/session resources\n");
        pthread_mutex_unlock(&xr_vulkan_lock);
        pthread_mutex_unlock(&shutdown_lock);
        return false;
    }
#define DESTROY_XR(handle, phase, call) do { \
    lifecycle_marker(phase, "begin", 0); \
    const XrResult destroyed = XR_CALL(phase, call); \
    lifecycle_marker(phase, "end", destroyed); \
    if (xr_destroy_ok(destroyed, phase)) handle = XR_NULL_HANDLE; \
    else cleanup_ok = false; \
} while (0)
    if (xr.view) DESTROY_XR(xr.view, "destroy-VIEW-space",
        xr.DestroySpace ? xr.DestroySpace(xr.view) : XR_ERROR_FUNCTION_UNSUPPORTED);
    if (xr.local) DESTROY_XR(xr.local, "destroy-LOCAL-space",
        xr.DestroySpace ? xr.DestroySpace(xr.local) : XR_ERROR_FUNCTION_UNSUPPORTED);
    if (xr.local || xr.view) goto incomplete;
    if (xr.session) DESTROY_XR(xr.session, "xrDestroySession",
        xr.DestroySession ? xr.DestroySession(xr.session) : XR_ERROR_FUNCTION_UNSUPPORTED);
    if (xr.session) goto incomplete;
    if (xr.instance) DESTROY_XR(xr.instance, "xrDestroyInstance",
        xr.DestroyInstance ? xr.DestroyInstance(xr.instance) : xrDestroyInstance(xr.instance));
#undef DESTROY_XR
    if (xr.instance || !cleanup_ok) goto incomplete;
    XRLOG("OpenXR shutdown complete; Vulkan remains owned by game\n");
    memset(&xr, 0, sizeof xr);
    atomic_store(&latest_present, 0);
    pthread_mutex_unlock(&xr_vulkan_lock);
    invalidate_pose(NULL, true);
    pthread_mutex_unlock(&shutdown_lock);
    return true;
incomplete:
    XRLOG("OpenXR shutdown incomplete; retained children block parent/GPU destruction\n");
    pthread_mutex_unlock(&xr_vulkan_lock);
    pthread_mutex_unlock(&shutdown_lock);
    return false;
}

void xr_runtime_shutdown(void) {
    if (xr_runtime_shutdown_checked()) return;
    XRLOG("OpenXR exit cleanup failed; terminating with status 1 before loader/GPU teardown\n");
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate_status(EXIT_FAILURE);
#endif
    _Exit(EXIT_FAILURE);
}

void xr_runtime_vulkan_idle_begin(void) { pthread_mutex_lock(&xr_vulkan_lock); }
void xr_runtime_vulkan_idle_end(void) { pthread_mutex_unlock(&xr_vulkan_lock); }

#else
// Ordinary builds do not acquire an OpenXR build/runtime dependency.
#include <stdio.h>
void xr_runtime_notify_game_present(uint64_t id) { (void)id; }
XrPoseReadResult xr_runtime_get_pose(XrPoseSnapshot *out) {
    if (out) memset(out, 0, sizeof *out);
    return XR_POSE_UNAVAILABLE;
}
void xr_runtime_reset_game(void) {}
uint64_t xr_runtime_pose_clock_ns(void) { return 0; }
void xr_runtime_shutdown(void) {}
bool xr_runtime_shutdown_checked(void) { return true; }
const char *xr_runtime_current_call(void) { return "OpenXR-disabled"; }
void xr_runtime_vulkan_idle_begin(void) {}
void xr_runtime_vulkan_idle_end(void) {}
bool xr_runtime_discover(void) {
    fprintf(stderr, "[pc-vr] OpenXR was not compiled in; rebuild runtime with OPENXR=1\n");
    return false;
}
#if !defined(__APPLE__)
#include "xr_vulkan.h"
bool xr_runtime_offer_eye_pair(const XrOwnedEyePair *pair) { (void)pair; return false; }
bool xr_runtime_eye_images_enabled(void) { return false; }
VkResult xr_runtime_create_vulkan_instance(const VkInstanceCreateInfo *info, VkInstance *instance) {
    (void)info; (void)instance; return VK_ERROR_INITIALIZATION_FAILED;
}
bool xr_runtime_select_vulkan_device(VkInstance instance, VkPhysicalDevice *physical) {
    (void)instance; (void)physical; return false;
}
VkResult xr_runtime_create_vulkan_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info, VkDevice *device) {
    (void)physical; (void)info; (void)device; return VK_ERROR_INITIALIZATION_FAILED;
}
bool xr_runtime_bind_vulkan(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                            uint32_t queue_family, uint32_t queue_index) {
    (void)instance; (void)physical; (void)device; (void)queue_family; (void)queue_index; return false;
}
#endif
#endif
