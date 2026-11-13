#ifndef SAVAGEWIND_PC_VR_NATIVE_VIEW_MATH_H
#define SAVAGEWIND_PC_VR_NATIVE_VIEW_MATH_H

// Pure, experimental third-person native-view construction. No runtime reads,
// guest writes, calibration updates, default scale or image admission occur.
// OpenXR poses remain in meters. The caller MUST supply game units per meter;
// accepting that explicit scale does not establish the game's physical units.
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "pc_vr_head_look_math.h"
#include "../xr/xr_runtime.h"

// The snapshot's D053 calibration: heading h, origin o, generation = its epoch.
typedef struct {
    float orientation_xyzw[4];
    float origin_xyz[3];
    uint64_t generation;
} PcVrNativeViewReference;

typedef struct {
    bool calibrated_midpoint, native_baseline, per_eye_orientation, original_look_radius;
    bool canted_from_center[2];
    double midpoint_error_in_baselines;
    double baseline_relative_error;
    double max_orientation_error_radians;
    double max_radius_relative_error;
} PcVrNativeViewContracts;

typedef struct {
    PcVrHeadLookCamera eyes[2];
    float anchor_position[3];
    double game_units_per_meter;
    PcVrNativeViewReference reference;
    XrPoseSnapshot snapshot;
    PcVrNativeViewContracts contracts;
} PcVrNativeViewPair;

static inline bool pc_vr_native_view_unit_quaternion(const float in[4], double out[4]) {
    double norm2 = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(in[i])) return false;
        norm2 += (double)in[i] * in[i];
    }
    if (!(norm2 > 0.99 && norm2 < 1.01)) return false;
    const double inverse = 1.0 / sqrt(norm2);
    for (unsigned i = 0; i < 4; ++i) out[i] = in[i] * inverse;
    return true;
}

static inline bool pc_vr_native_view_fov_valid(const float fov[4]) {
    for (unsigned i = 0; i < 4; ++i)
        if (!isfinite(fov[i]) || !(fov[i] > -1.5707963267948966 && fov[i] < 1.5707963267948966)) return false;
    // The original game's positive-aspect perspective supports unflipped FOV.
    return fov[0] < fov[1] && fov[3] < fov[2];
}

static inline void pc_vr_native_view_basis_vector(const double right[3], const double up[3],
    const double back[3], const double local[3], double world[3]) {
    for (unsigned i = 0; i < 3; ++i)
        world[i] = right[i] * local[0] + up[i] * local[1] + back[i] * local[2];
}

static inline double pc_vr_native_view_length(const double vector[3]) {
    return hypot(hypot(vector[0], vector[1]), vector[2]);
}

// Check the original builder's actual row-major 3x4 world-to-view result.
// Comparing every rotation entry fixes handedness and axis order; transforming
// the stored eye position checks all three translation entries. Numerical
// cancellation must stay below 1% of this pair's actual game-space baseline,
// not a tolerance proportional to the absolute world coordinate.
static inline bool pc_vr_native_view_matrix_matches(const PcVrHeadLookCamera *camera,
    const float matrix[12], double game_separation, double *origin_error_in_baselines) {
    if (!camera || !matrix || !origin_error_in_baselines ||
        !isfinite(game_separation) || !(game_separation > 0)) return false;
    double basis[3][3];
    if (!pc_vr_head_look_basis(camera, basis[0], basis[1], basis[2])) return false;
    for (unsigned i = 0; i < 12; ++i) if (!isfinite(matrix[i])) return false;
    double origin[3];
    for (unsigned row = 0; row < 3; ++row) {
        origin[row] = matrix[row * 4 + 3];
        for (unsigned col = 0; col < 3; ++col) {
            if (fabs((double)matrix[row * 4 + col] - basis[row][col]) > 0.00002) return false;
            origin[row] += (double)matrix[row * 4 + col] * camera->position[col];
        }
    }
    const double residual = pc_vr_native_view_length(origin) / game_separation;
    if (!isfinite(residual) || residual > 0.01) return false;
    *origin_error_in_baselines = residual;
    return true;
}

// B is the controller camera's existing right/up/back basis, C its unchanged
// position, and h/o the snapshot's D053 calibration heading and origin.
// Both eyes use one snapshot and the SAME rigid transform:
//   position_i = C + scale * B * inverse(h) * (positionXR_i - o)
//   forward_i  = B * inverse(h) * orientationXR_i * (0,0,-1)
//   up_i       = B * inverse(h) * orientationXR_i * (0,1,0).
// The eye midpoint sits at C when the head is at o; head translation from o
// moves both eyes (unclamped; the game's camera collision does not see it).
// Controller orbit/movement remains authoritative. Optical per-eye rotations
// are retained even when canted. Original look radius and up length are kept.
//
// Freshness and root-traversal identity belong to the caller. This helper
// requires the reference to be the snapshot's own calibration but does not
// read a clock or establish render/resource/culling provenance. Failure
// leaves out intact.
static inline bool pc_vr_native_view_build_pair(const PcVrHeadLookCamera *center,
    const PcVrNativeViewReference *reference, const XrPoseSnapshot *snapshot,
    double game_units_per_meter, PcVrNativeViewPair *out) {
    if (!center || !reference || !snapshot || !out || !isfinite(game_units_per_meter) ||
        !(game_units_per_meter > 0) || !reference->generation || !snapshot->generation ||
        reference->generation != snapshot->calibration.epoch ||
        memcmp(reference->orientation_xyzw, snapshot->calibration.heading_xyzw, sizeof reference->orientation_xyzw) ||
        memcmp(reference->origin_xyz, snapshot->calibration.origin_xyz, sizeof reference->origin_xyz) ||
        !isfinite(reference->origin_xyz[0]) || !isfinite(reference->origin_xyz[1]) ||
        !isfinite(reference->origin_xyz[2]) || !snapshot->frame_id ||
        !snapshot->published_monotonic_ns || !snapshot->active ||
        !snapshot->orientation_valid || !snapshot->orientation_tracked ||
        !snapshot->views_valid || !snapshot->native_views_valid) return false;
    double right[3], up[3], back[3];
    if (!pc_vr_head_look_basis(center, right, up, back)) return false;
    double original_forward[3], original_up[3];
    for (unsigned i = 0; i < 3; ++i) {
        original_forward[i] = (double)center->target[i] - center->position[i];
        original_up[i] = center->up[i];
    }
    const double radius = pc_vr_native_view_length(original_forward);
    const double up_length = pc_vr_native_view_length(original_up);
    if (!isfinite(radius) || !isfinite(up_length) || !(radius > 0 && up_length > 0)) return false;
    double neutral[4], central[4], optical[2][4];
    if (!pc_vr_native_view_unit_quaternion(reference->orientation_xyzw, neutral) ||
        !pc_vr_native_view_unit_quaternion(snapshot->orientation_xyzw, central)) return false;
    for (unsigned side = 0; side < 2; ++side) {
        if (!pc_vr_native_view_unit_quaternion(snapshot->eyes[side].orientation_xyzw, optical[side]) ||
            !pc_vr_native_view_fov_valid(snapshot->eyes[side].fov_left_right_up_down)) return false;
        for (unsigned i = 0; i < 3; ++i) if (!isfinite(snapshot->eyes[side].position_xyz[i])) return false;
    }
    const double inverse_neutral[4] = {-neutral[0], -neutral[1], -neutral[2], neutral[3]};
    double midpoint[3], native_baseline[3];
    for (unsigned i = 0; i < 3; ++i) {
        midpoint[i] = 0.5 * ((double)snapshot->eyes[0].position_xyz[i] + snapshot->eyes[1].position_xyz[i]);
        native_baseline[i] = (double)snapshot->eyes[1].position_xyz[i] - snapshot->eyes[0].position_xyz[i];
    }
    const double native_separation = pc_vr_native_view_length(native_baseline);
    const double game_separation = native_separation * game_units_per_meter;
    if (!isfinite(game_separation) || !(native_separation > 0 && game_separation > 0)) return false;
    const double local_right[3] = {1, 0, 0};
    double native_right[3];
    pc_vr_head_look_rotate(central, local_right, native_right);
    // PRIMARY_STEREO indices retain left/right identity under head rotation.
    // Check only the sign, with no fixed IPD or assumed perfect collinearity.
    if (!(pc_vr_head_look_dot(native_baseline, native_right) > 0)) return false;

    PcVrNativeViewPair result = {0};
    double intended_forward[2][3], intended_up[2][3];
    const double local_forward[3] = {0, 0, -1}, local_up[3] = {0, 1, 0};
    for (unsigned side = 0; side < 2; ++side) {
        double offset[3], neutral_offset[3], world_offset[3];
        for (unsigned i = 0; i < 3; ++i) offset[i] = (double)snapshot->eyes[side].position_xyz[i] - reference->origin_xyz[i];
        pc_vr_head_look_rotate(inverse_neutral, offset, neutral_offset);
        pc_vr_native_view_basis_vector(right, up, back, neutral_offset, world_offset);
        double located_forward[3], located_up[3], neutral_forward[3], neutral_up[3];
        pc_vr_head_look_rotate(optical[side], local_forward, located_forward);
        pc_vr_head_look_rotate(optical[side], local_up, located_up);
        pc_vr_head_look_rotate(inverse_neutral, located_forward, neutral_forward);
        pc_vr_head_look_rotate(inverse_neutral, located_up, neutral_up);
        pc_vr_native_view_basis_vector(right, up, back, neutral_forward, intended_forward[side]);
        pc_vr_native_view_basis_vector(right, up, back, neutral_up, intended_up[side]);
        double center_dot = 0;
        for (unsigned i = 0; i < 4; ++i) center_dot += central[i] * optical[side][i];
        result.contracts.canted_from_center[side] = fabs(center_dot) < 1.0 - 1e-6;
        for (unsigned i = 0; i < 3; ++i) {
            const double position = center->position[i] + game_units_per_meter * world_offset[i];
            const double target = position + radius * intended_forward[side][i];
            const double eye_up = up_length * intended_up[side][i];
            if (!isfinite(position) || !isfinite(target) || !isfinite(eye_up) ||
                !isfinite((float)position) || !isfinite((float)target) || !isfinite((float)eye_up)) return false;
            result.eyes[side].position[i] = (float)position;
            result.eyes[side].target[i] = (float)target;
            result.eyes[side].up[i] = (float)eye_up;
        }
        double delivered_right[3], delivered_up[3], delivered_back[3], delivered_forward[3];
        if (!pc_vr_head_look_basis(&result.eyes[side], delivered_right, delivered_up, delivered_back)) return false;
        double forward_dot = 0, up_dot = 0;
        for (unsigned i = 0; i < 3; ++i) {
            forward_dot -= delivered_back[i] * intended_forward[side][i];
            up_dot += delivered_up[i] * intended_up[side][i];
            delivered_forward[i] = (double)result.eyes[side].target[i] - result.eyes[side].position[i];
        }
        const double angular_error = acos(fmax(-1.0, fmin(1.0, fmin(forward_dot, up_dot))));
        const double radius_error = fabs(pc_vr_native_view_length(delivered_forward) / radius - 1.0);
        result.contracts.max_orientation_error_radians = fmax(result.contracts.max_orientation_error_radians, angular_error);
        result.contracts.max_radius_relative_error = fmax(result.contracts.max_radius_relative_error, radius_error);
    }
    double neutral_baseline[3], expected_baseline[3], baseline_error[3], midpoint_error[3], delivered_baseline[3];
    double head_offset[3], neutral_head[3], expected_head[3];
    pc_vr_head_look_rotate(inverse_neutral, native_baseline, neutral_baseline);
    pc_vr_native_view_basis_vector(right, up, back, neutral_baseline, expected_baseline);
    for (unsigned i = 0; i < 3; ++i) head_offset[i] = midpoint[i] - reference->origin_xyz[i];
    pc_vr_head_look_rotate(inverse_neutral, head_offset, neutral_head);
    pc_vr_native_view_basis_vector(right, up, back, neutral_head, expected_head);
    for (unsigned i = 0; i < 3; ++i) {
        delivered_baseline[i] = (double)result.eyes[1].position[i] - result.eyes[0].position[i];
        baseline_error[i] = delivered_baseline[i] - game_units_per_meter * expected_baseline[i];
        midpoint_error[i] = 0.5 * ((double)result.eyes[0].position[i] + result.eyes[1].position[i]) -
            center->position[i] - game_units_per_meter * expected_head[i];
    }
    result.contracts.baseline_relative_error = pc_vr_native_view_length(baseline_error) / game_separation;
    result.contracts.midpoint_error_in_baselines = pc_vr_native_view_length(midpoint_error) / game_separation;
    // Numerical fidelity bounds, not physical-unit or comfort acceptance.
    // Refuse scales/coordinates whose float storage collapses or materially
    // changes stereo. The actual residuals are returned for later measurement.
    result.contracts.calibrated_midpoint = result.contracts.midpoint_error_in_baselines <= 0.01;
    result.contracts.native_baseline = pc_vr_native_view_length(delivered_baseline) > 0 &&
        result.contracts.baseline_relative_error <= 0.01;
    result.contracts.per_eye_orientation = result.contracts.max_orientation_error_radians <= 0.001;
    result.contracts.original_look_radius = result.contracts.max_radius_relative_error <= 0.001;
    if (!result.contracts.calibrated_midpoint || !result.contracts.native_baseline ||
        !result.contracts.per_eye_orientation || !result.contracts.original_look_radius) return false;
    memcpy(result.anchor_position, center->position, sizeof result.anchor_position);
    result.game_units_per_meter = game_units_per_meter;
    memcpy(&result.reference, reference, sizeof *reference);
    memcpy(&result.snapshot, snapshot, sizeof *snapshot);
    memcpy(out, &result, sizeof result);
    return true;
}

#endif
