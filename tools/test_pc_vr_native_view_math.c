// Asset-free native-view math. Uses the already-pinned Khronos quaternion
// implementation as the independent reference; no runtime or guest is opened.
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/hle/pc_vr_native_view_math.h"
#include "../../.cache/openxr-sdk-source/src/common/xr_linear.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "native view math failed line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static XrQuaternionf axis_rotation(float x, float y, float z, double radians) {
    const double norm = sqrt((double)x * x + (double)y * y + (double)z * z);
    const double factor = sin(0.5 * radians) / norm;
    return (XrQuaternionf){(float)(x * factor), (float)(y * factor), (float)(z * factor), (float)cos(0.5 * radians)};
}
static void copy_q(float out[4], XrQuaternionf q) {
    out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
}
static XrQuaternionf xr_q(const float q[4]) { return (XrQuaternionf){q[0], q[1], q[2], q[3]}; }
static PcVrHeadLookCamera camera(XrQuaternionf orientation, XrVector3f position, float radius, float up_length) {
    const XrVector3f forward = {0, 0, -radius}, up = {0, up_length, 0};
    XrVector3f directed, rotated_up;
    XrQuaternionf_RotateVector3f(&directed, &orientation, &forward);
    XrQuaternionf_RotateVector3f(&rotated_up, &orientation, &up);
    return (PcVrHeadLookCamera){{position.x, position.y, position.z},
        {position.x + directed.x, position.y + directed.y, position.z + directed.z},
        {rotated_up.x, rotated_up.y, rotated_up.z}};
}
static XrPoseSnapshot pose(XrQuaternionf central, float baseline, bool canted) {
    XrPoseSnapshot p = {.frame_id = 91, .game_present_id = 40, .generation = 7,
        .published_monotonic_ns = UINT64_C(10000000000), .predicted_display_time_ns = INT64_C(9000000000),
        .active = true, .focused = true, .orientation_valid = true, .orientation_tracked = true,
        .views_valid = true, .native_views_valid = true};
    copy_q(p.orientation_xyzw, central);
    for (unsigned side = 0; side < 2; ++side) {
        const XrVector3f local = {baseline * (side ? 0.5f : -0.5f), 0, 0};
        XrVector3f located;
        XrQuaternionf_RotateVector3f(&located, &central, &local);
        p.eyes[side].position_xyz[0] = located.x;
        p.eyes[side].position_xyz[1] = located.y + 1.5f;
        p.eyes[side].position_xyz[2] = located.z;
        const XrPosef head = {central, {0, 0, 0}};
        const XrPosef optical = {axis_rotation(0, 1, 0, canted ? (side ? -0.12 : 0.12) : 0), {0, 0, 0}};
        XrPosef combined;
        XrPosef_Multiply(&combined, &head, &optical);
        copy_q(p.eyes[side].orientation_xyzw, combined.orientation);
        const float fov[4] = {side ? -0.9f : -0.8f, side ? 0.8f : 0.9f, 0.85f, -0.7f};
        memcpy(p.eyes[side].fov_left_right_up_down, fov, sizeof fov);
    }
    return p;
}
// The snapshot carries the reference as its own D053 calibration.
static void calibrate(XrPoseSnapshot *p, const PcVrNativeViewReference *r) {
    p->calibration.epoch = r->generation;
    memcpy(p->calibration.heading_xyzw, r->orientation_xyzw, sizeof r->orientation_xyzw);
    memcpy(p->calibration.origin_xyz, r->origin_xyz, sizeof r->origin_xyz);
}
static bool close_float(float actual, float expected) {
    return isfinite(actual) && isfinite(expected) && fabs((double)actual - expected) <=
        0.00002 + 4.0 * FLT_EPSILON * fmax(fabs(actual), fabs(expected));
}

static void compare_reference(const PcVrHeadLookCamera *base, XrQuaternionf controller,
    const PcVrNativeViewReference *reference, const XrPoseSnapshot *input, double scale,
    const PcVrNativeViewPair *result) {
    XrQuaternionf neutral_inverse, neutral = xr_q(reference->orientation_xyzw);
    XrQuaternionf_Invert(&neutral_inverse, &neutral);
    const double dx = (double)base->target[0] - base->position[0];
    const double dy = (double)base->target[1] - base->position[1];
    const double dz = (double)base->target[2] - base->position[2];
    const float radius = (float)sqrt(dx * dx + dy * dy + dz * dz);
    const float up_length = (float)sqrt((double)base->up[0] * base->up[0] +
        (double)base->up[1] * base->up[1] + (double)base->up[2] * base->up[2]);
    for (unsigned side = 0; side < 2; ++side) {
        XrVector3f relative, world;
        float components[3];
        for (unsigned i = 0; i < 3; ++i)
            components[i] = (float)((double)input->eyes[side].position_xyz[i] - reference->origin_xyz[i]);
        const XrVector3f offset = {components[0], components[1], components[2]};
        XrQuaternionf_RotateVector3f(&relative, &neutral_inverse, &offset);
        XrQuaternionf_RotateVector3f(&world, &controller, &relative);
        const float expected_position[3] = {(float)(base->position[0] + scale * world.x),
            (float)(base->position[1] + scale * world.y), (float)(base->position[2] + scale * world.z)};
        const XrQuaternionf optical = xr_q(input->eyes[side].orientation_xyzw);
        const XrVector3f forward = {0, 0, -radius}, up = {0, up_length, 0};
        XrVector3f located_forward, located_up, neutral_forward, neutral_up, world_forward, world_up;
        XrQuaternionf_RotateVector3f(&located_forward, &optical, &forward);
        XrQuaternionf_RotateVector3f(&located_up, &optical, &up);
        XrQuaternionf_RotateVector3f(&neutral_forward, &neutral_inverse, &located_forward);
        XrQuaternionf_RotateVector3f(&neutral_up, &neutral_inverse, &located_up);
        XrQuaternionf_RotateVector3f(&world_forward, &controller, &neutral_forward);
        XrQuaternionf_RotateVector3f(&world_up, &controller, &neutral_up);
        const float expected_target[3] = {expected_position[0] + world_forward.x,
            expected_position[1] + world_forward.y, expected_position[2] + world_forward.z};
        const float expected_up[3] = {world_up.x, world_up.y, world_up.z};
        for (unsigned i = 0; i < 3; ++i) {
            CHECK(close_float(result->eyes[side].position[i], expected_position[i]));
            CHECK(close_float(result->eyes[side].target[i], expected_target[i]));
            CHECK(close_float(result->eyes[side].up[i], expected_up[i]));
        }
    }
}

static void test_transforms(void) {
    const XrQuaternionf rotations[] = {
        {0, 0, 0, 1}, axis_rotation(0, 1, 0, 1.5707963267948966),
        axis_rotation(1, 0, 0, -0.6), axis_rotation(0, 0, 1, 0.7), axis_rotation(1, 2, 3, 1.1)};
    const double scales[] = {0.25, 1.0, 3.5};
    const float baselines[] = {0.05f, 0.0625f, 0.075f};
    for (unsigned rotation = 0; rotation < 5; ++rotation)
        for (unsigned scale_index = 0; scale_index < 3; ++scale_index)
            for (unsigned cant = 0; cant < 2; ++cant) {
                const XrQuaternionf controller = rotations[(rotation + 2) % 5];
                PcVrHeadLookCamera base = camera(controller, (XrVector3f){10, 2, 4}, 5, 2);
                PcVrNativeViewReference reference = {.origin_xyz = {0.02f, 1.45f, -0.03f}, .generation = 7};
                copy_q(reference.orientation_xyzw, rotations[(rotation + 1) % 5]);
                XrPoseSnapshot input = pose(rotations[rotation], baselines[scale_index], cant != 0);
                calibrate(&input, &reference);
                PcVrNativeViewPair result;
                const PcVrHeadLookCamera base_before = base;
                const XrPoseSnapshot input_before = input;
                CHECK(pc_vr_native_view_build_pair(&base, &reference, &input, scales[scale_index], &result));
                CHECK(!memcmp(&base, &base_before, sizeof base) && !memcmp(&input, &input_before, sizeof input));
                CHECK(!memcmp(&result.snapshot, &input, sizeof input));
                CHECK(!memcmp(result.anchor_position, base.position, sizeof base.position));
                CHECK(result.game_units_per_meter == scales[scale_index] && result.reference.generation == reference.generation);
                CHECK(result.contracts.calibrated_midpoint && result.contracts.native_baseline &&
                    result.contracts.per_eye_orientation && result.contracts.original_look_radius);
                CHECK(result.contracts.canted_from_center[0] == (cant != 0) && result.contracts.canted_from_center[1] == (cant != 0));
                compare_reference(&base, controller, &reference, &input, scales[scale_index], &result);
            }
    const XrQuaternionf identity = {0, 0, 0, 1};
    PcVrHeadLookCamera base = camera(identity, (XrVector3f){0, 0, 0}, 5, 1);
    // Origin at this pose's eye midpoint: the head at o puts the midpoint at C.
    PcVrNativeViewReference reference = {.orientation_xyzw = {0, 0, 0, 1}, .origin_xyz = {0, 1.5f, 0}, .generation = 7};
    XrPoseSnapshot input = pose(identity, 0.0625f, false);
    calibrate(&input, &reference);
    PcVrNativeViewPair original, moved, rescaled;
    CHECK(pc_vr_native_view_build_pair(&base, &reference, &input, 1.0, &original));
    for (unsigned i = 0; i < 3; ++i)
        CHECK(close_float(0.5f * (original.eyes[0].position[i] + original.eyes[1].position[i]), base.position[i]));
    // Head translation from o moves both eyes rigidly, per metre of scale (D053).
    const float step[3] = {0.25f, 0.125f, -0.5f};
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned i = 0; i < 3; ++i) input.eyes[side].position_xyz[i] += step[i];
    CHECK(pc_vr_native_view_build_pair(&base, &reference, &input, 1.0, &moved));
    CHECK(pc_vr_native_view_build_pair(&base, &reference, &input, 2.0, &rescaled));
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned i = 0; i < 3; ++i) {
            CHECK(close_float(moved.eyes[side].position[i], original.eyes[side].position[i] + step[i]));
            CHECK(close_float(moved.eyes[side].target[i], original.eyes[side].target[i] + step[i]));
            CHECK(moved.eyes[side].up[i] == original.eyes[side].up[i]);
            CHECK(close_float(rescaled.eyes[side].target[i] - rescaled.eyes[side].position[i],
                original.eyes[side].target[i] - original.eyes[side].position[i])); // Radius is not scaled.
        }
    CHECK(close_float(rescaled.eyes[0].position[2], 2 * step[2]));
    CHECK(close_float(rescaled.eyes[1].position[0] - rescaled.eyes[0].position[0],
        2 * (original.eyes[1].position[0] - original.eyes[0].position[0])));
    ++input.frame_id; ++input.generation; ++input.published_monotonic_ns;
    copy_q(input.eyes[0].orientation_xyzw, rotations[1]);
    CHECK(original.snapshot.frame_id == 91 && original.snapshot.generation == 7);
    CHECK(original.snapshot.eyes[0].orientation_xyzw[3] == 1); // Later publication cannot alter returned pair.
    // Sign-equivalent quaternions must not change the pose or cant metadata.
    input = pose(identity, 0.0625f, false);
    calibrate(&input, &reference);
    for (unsigned side = 0; side < 2; ++side) input.eyes[side].orientation_xyzw[3] = -1;
    CHECK(pc_vr_native_view_build_pair(&base, &reference, &input, 1.0, &moved));
    CHECK(!memcmp(original.eyes, moved.eyes, sizeof original.eyes) && !moved.contracts.canted_from_center[0]);
    // A head turned 90 degrees from the heading looks 90 degrees off the camera.
    input = pose(rotations[1], 0.0625f, false);
    calibrate(&input, &reference);
    CHECK(pc_vr_native_view_build_pair(&base, &reference, &input, 1.0, &moved));
    for (unsigned side = 0; side < 2; ++side) {
        CHECK(close_float(moved.eyes[side].target[0] - moved.eyes[side].position[0], -5));
        CHECK(close_float(moved.eyes[side].target[2] - moved.eyes[side].position[2], 0));
    }
    // A recentered calibration (new epoch, heading turned with the head) looks ahead.
    ++input.generation; reference.generation = 8;
    copy_q(reference.orientation_xyzw, rotations[1]);
    calibrate(&input, &reference);
    CHECK(pc_vr_native_view_build_pair(&base, &reference, &input, 1.0, &moved));
    for (unsigned side = 0; side < 2; ++side) {
        CHECK(close_float(moved.eyes[side].target[0] - moved.eyes[side].position[0], 0));
        CHECK(close_float(moved.eyes[side].target[2] - moved.eyes[side].position[2], -5));
    }
    CHECK(moved.reference.generation == 8 && moved.snapshot.calibration.epoch == 8);
    reference = (PcVrNativeViewReference){.orientation_xyzw = {0, 0, 0, 1}, .origin_xyz = {0.01f, 1.4f, -0.02f}, .generation = 7};
    input = pose(identity, 0.064f, true);
    input.eyes[0].position_xyz[0] = -0.04f; input.eyes[0].position_xyz[2] = -0.01f;
    input.eyes[1].position_xyz[0] = 0.03f; input.eyes[1].position_xyz[1] += 0.01f;
    input.eyes[1].position_xyz[2] = 0.02f;
    calibrate(&input, &reference);
    CHECK(pc_vr_native_view_build_pair(&base, &reference, &input, 1.0, &moved));
    compare_reference(&base, identity, &reference, &input, 1.0, &moved); // Non-collinear/asymmetric optical origins.
    base = camera(identity, (XrVector3f){4000, 2, 4000}, 5, 1);
    input = pose(identity, 0.064f, true);
    calibrate(&input, &reference);
    CHECK(pc_vr_native_view_build_pair(&base, &reference, &input, 1.0, &moved));
    CHECK(moved.contracts.baseline_relative_error < 0.01 && moved.contracts.max_radius_relative_error < 0.001);
    compare_reference(&base, identity, &reference, &input, 1.0, &moved);
}

static void test_rejection(void) {
    const XrQuaternionf identity = {0, 0, 0, 1};
    const PcVrHeadLookCamera center = camera(identity, (XrVector3f){0, 0, 0}, 5, 1);
    const PcVrNativeViewReference reference = {.orientation_xyzw = {0, 0, 0, 1}, .origin_xyz = {0, 1.5f, 0}, .generation = 7};
    XrPoseSnapshot snapshot = pose(identity, 0.064f, false);
    calibrate(&snapshot, &reference);
    PcVrNativeViewPair output, sentinel;
    memset(&sentinel, 0x5a, sizeof sentinel);
#define REJECT(change) do { \
    PcVrHeadLookCamera bad_center = center; PcVrNativeViewReference bad_reference = reference; \
    XrPoseSnapshot bad_snapshot = snapshot; double bad_scale = 1.0; output = sentinel; change; \
    CHECK(!pc_vr_native_view_build_pair(&bad_center, &bad_reference, &bad_snapshot, bad_scale, &output)); \
    CHECK(!memcmp(&output, &sentinel, sizeof output)); \
} while (0)
    REJECT(bad_scale = 0);
    REJECT(bad_scale = -1);
    REJECT(bad_scale = NAN);
    REJECT(bad_scale = INFINITY);
    REJECT(bad_scale = DBL_MAX);
    REJECT(bad_scale = DBL_MIN);
    REJECT(bad_reference.generation = 0);
    REJECT(++bad_reference.generation);
    REJECT(bad_snapshot.generation = 0);
    REJECT(++bad_snapshot.calibration.epoch);
    REJECT(bad_snapshot.calibration.heading_xyzw[1] = 0.01f);
    REJECT(bad_reference.origin_xyz[1] += 0.01f);
    REJECT(bad_reference.origin_xyz[0] = bad_snapshot.calibration.origin_xyz[0] = NAN);
    REJECT(bad_reference.origin_xyz[2] = bad_snapshot.calibration.origin_xyz[2] = INFINITY);
    REJECT(bad_reference.orientation_xyzw[3] = bad_snapshot.calibration.heading_xyzw[3] = 0);
    REJECT(bad_snapshot.frame_id = 0);
    REJECT(bad_snapshot.published_monotonic_ns = 0);
    REJECT(bad_snapshot.active = false);
    REJECT(bad_snapshot.orientation_valid = false);
    REJECT(bad_snapshot.orientation_tracked = false);
    REJECT(bad_snapshot.views_valid = false);
    REJECT(bad_snapshot.native_views_valid = false);
    REJECT(bad_reference.orientation_xyzw[3] = 0);
    REJECT(bad_reference.orientation_xyzw[0] = INFINITY);
    REJECT(bad_snapshot.orientation_xyzw[3] = 2);
    REJECT(bad_snapshot.orientation_xyzw[1] = NAN);
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned i = 0; i < 4; ++i) {
            REJECT(bad_snapshot.eyes[side].orientation_xyzw[i] = NAN);
            REJECT(bad_snapshot.eyes[side].fov_left_right_up_down[i] = INFINITY);
        }
        for (unsigned i = 0; i < 3; ++i) REJECT(bad_snapshot.eyes[side].position_xyz[i] = NAN);
    }
    REJECT(bad_snapshot.eyes[0].fov_left_right_up_down[0] = 1.0f);
    REJECT(bad_snapshot.eyes[1].fov_left_right_up_down[3] = 1.0f);
    REJECT(bad_snapshot.eyes[0].fov_left_right_up_down[0] = -1.5707964f);
    REJECT(bad_snapshot.eyes[1].fov_left_right_up_down[2] = 1.5707964f);
    REJECT(memcpy(bad_snapshot.eyes[1].position_xyz, bad_snapshot.eyes[0].position_xyz, 3 * sizeof(float)));
    REJECT(bad_snapshot.eyes[0].position_xyz[0] = 0.032f; bad_snapshot.eyes[1].position_xyz[0] = -0.032f);
    REJECT(bad_center.target[2] = 0);
    REJECT(bad_center.up[1] = 0; bad_center.up[2] = 1);
    REJECT(bad_center.position[0] = INFINITY);
    REJECT(bad_center.target[1] = NAN);
    REJECT(bad_center.up[2] = NAN);
    REJECT(bad_center.position[0] = bad_center.target[0] = 1e20f);
    REJECT(bad_center.position[0] = bad_center.target[0] = 4000; bad_scale = 0.00001);
#undef REJECT
    snapshot.focused = false;
    CHECK(pc_vr_native_view_build_pair(&center, &reference, &snapshot, 1.0, &output));
    output = sentinel;
    CHECK(!pc_vr_native_view_build_pair(NULL, &reference, &snapshot, 1.0, &output));
    CHECK(!pc_vr_native_view_build_pair(&center, NULL, &snapshot, 1.0, &output));
    CHECK(!pc_vr_native_view_build_pair(&center, &reference, NULL, 1.0, &output));
    CHECK(!pc_vr_native_view_build_pair(&center, &reference, &snapshot, 1.0, NULL));
    CHECK(!memcmp(&output, &sentinel, sizeof output));
}

static void test_actual_view_matrix(void) {
    const XrQuaternionf rotations[] = {{0, 0, 0, 1}, axis_rotation(1, 2, 3, 0.7),
        axis_rotation(0, 1, 0, -1.2), axis_rotation(1, 0, 0, 0.3)};
    for (unsigned test = 0; test < sizeof rotations / sizeof rotations[0]; ++test) {
        const XrVector3f position = {10, 2, -4}, scale = {1, 1, 1};
        const PcVrHeadLookCamera eye = camera(rotations[test], position, 5, 1.3f);
        XrMatrix4x4f world, view;
        XrMatrix4x4f_CreateTranslationRotationScale(&world, &position, &rotations[test], &scale);
        XrMatrix4x4f_InvertRigidBody(&view, &world);
        float stored[12];
        for (unsigned row = 0; row < 3; ++row) for (unsigned col = 0; col < 4; ++col)
            stored[row * 4 + col] = view.m[col * 4 + row];
        double residual = -1;
        CHECK(pc_vr_native_view_matrix_matches(&eye, stored, 0.064, &residual));
        CHECK(residual >= 0 && residual < 0.01);
        for (unsigned i = 0; i < 12; ++i) {
            float corrupt[12]; memcpy(corrupt, stored, sizeof corrupt); corrupt[i] += 0.01f;
            residual = -1;
            CHECK(!pc_vr_native_view_matrix_matches(&eye, corrupt, 0.064, &residual));
            CHECK(residual == -1);
            corrupt[i] = NAN;
            CHECK(!pc_vr_native_view_matrix_matches(&eye, corrupt, 0.064, &residual));
        }
        CHECK(!pc_vr_native_view_matrix_matches(&eye, stored, 0, &residual));
        CHECK(!pc_vr_native_view_matrix_matches(&eye, stored, INFINITY, &residual));
        CHECK(!pc_vr_native_view_matrix_matches(NULL, stored, 0.064, &residual));
        CHECK(!pc_vr_native_view_matrix_matches(&eye, NULL, 0.064, &residual));
        CHECK(!pc_vr_native_view_matrix_matches(&eye, stored, 0.064, NULL));
    }
}

int main(void) {
    test_transforms(); test_rejection(); test_actual_view_matrix();
    printf("PC VR native view math: %u checks passed (explicit scale; pinned Khronos reference; no guest writes)\n", checks);
    return 0;
}
