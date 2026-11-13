#ifndef SAVAGEWIND_PC_VR_PROJECTION_MATH_H
#define SAVAGEWIND_PC_VR_PROJECTION_MATH_H

// Pure mapping into the pinned game's existing perspective parameters. This
// is not a replacement projection/tracking framework: the original logical
// writer and posture/device conversion remain authoritative for guest output.
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "pc_vr_head_look_math.h"

static inline bool pc_vr_projection_close(double actual, double expected) {
    return isfinite(actual) && isfinite(expected) && fabs(actual - expected) <= 0.00002 * fmax(1.0, fabs(expected));
}

static inline bool pc_vr_projection_native_rays(const float fov[4], const float matrix[16]) {
    if (!fov || !matrix) return false;
    for (unsigned i = 0; i < 16; ++i) if (!isfinite(matrix[i])) return false;
    double tangents[4];
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(fov[i]) || !(fov[i] > -1.5707963267948966 && fov[i] < 1.5707963267948966)) return false;
        tangents[i] = tan(fov[i]);
    }
    if (!(tangents[0] < tangents[1] && tangents[3] < tangents[2])) return false;
    // Test the four corner rays at two distances, including the actual
    // homogeneous divisor. This catches offset/shear/w-row errors too.
    for (unsigned distance = 1; distance <= 2; ++distance)
        for (unsigned corner_x = 0; corner_x < 2; ++corner_x) for (unsigned corner_y = 0; corner_y < 2; ++corner_y) {
                const double corner[4] = {distance * tangents[corner_x], distance * tangents[corner_y ? 2 : 3],
                                          -(double)distance, 1};
                double clip[4] = {0};
                for (unsigned row = 0; row < 4; ++row)
                    for (unsigned col = 0; col < 4; ++col) clip[row] += matrix[row * 4 + col] * corner[col];
                if (!(clip[3] > 0) || !pc_vr_projection_close(clip[0] / clip[3], corner_x ? 1 : -1) ||
                    !pc_vr_projection_close(clip[1] / clip[3], corner_y ? 1 : -1))
                    return false;
        }
    return true;
}

// Parameters are n,f,fovy,sinHalf,cosHalf,tanHalf,aspect,centerX,centerY.
// The row-major logical matrix has RH -Z forward, +Y up and [-1,1] depth.
// It matches the transposed Khronos xr_linear.h GRAPHICS_OPENGL projection.
// Never pre-flip Y or pre-convert depth here: da1034 performs the game's own
// posture/depth conversion before the existing renderer consumes the result.
static inline bool pc_vr_projection_logical(const float params[9], float out[16]) {
    if (!params || !out) return false;
    for (unsigned i = 0; i < 9; ++i) if (!isfinite(params[i])) return false;
    if (!(params[0] > 0 && params[1] > params[0] && params[2] > 0 && params[2] < 3.141592653589793 &&
          params[3] > 0 && params[4] > 0 && params[5] > 0 && params[6] > 0) ||
        !pc_vr_projection_close(params[3], sin(0.5 * params[2])) ||
        !pc_vr_projection_close(params[4], cos(0.5 * params[2])) ||
        !pc_vr_projection_close(params[5], tan(0.5 * params[2]))) return false;
    const double near_plane = params[0], far_plane = params[1];
    const double values[16] = {1.0 / ((double)params[5] * params[6]), 0, 2.0 * params[7], 0,
        0, 1.0 / params[5], 2.0 * params[8], 0, 0, 0, -(far_plane + near_plane) / (far_plane - near_plane),
        -2.0 * far_plane * near_plane / (far_plane - near_plane), 0, 0, -1, 0};
    float result[16];
    for (unsigned i = 0; i < 16; ++i) {
        if (!isfinite(values[i]) || !isfinite((float)values[i])) return false;
        result[i] = (float)values[i];
    }
    if (!(result[0] > 0 && result[5] > 0 && result[11] < 0)) return false;
    memcpy(out, result, sizeof result);
    return true;
}

// OpenXR angles in radians: left,right,up,down. Reject reversed/flipped or
// collapsed frusta; that legal OpenXR case is not supported by this game's
// positive-aspect perspective contract. Preserve original near/far exactly.
// Failure leaves output untouched; input/output may alias.
static inline bool pc_vr_projection_from_fov(const float fov[4],
                                            const float original[9], float out[9]) {
    if (!fov || !original || !out || !isfinite(original[0]) || !isfinite(original[1]) ||
        !(original[0] > 0 && original[1] > original[0])) return false;
    double tangents[4];
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(fov[i]) || !(fov[i] > -1.5707963267948966 && fov[i] < 1.5707963267948966)) return false;
        tangents[i] = tan(fov[i]);
    }
    const double width = tangents[1] - tangents[0], height = tangents[2] - tangents[3];
    if (!(width > 0 && height > 0)) return false;
    float result[9] = {original[0], original[1], (float)(2.0 * atan(height * 0.5)), 0, 0, 0,
        (float)(width / height), (float)((tangents[1] + tangents[0]) / (2.0 * width)),
        (float)((tangents[2] + tangents[3]) / (2.0 * height))};
    // Derive every cache from the actual stored float angle, avoiding mixed
    // high-precision/rounded fields which disagree when the game reads them.
    const double half = 0.5 * result[2];
    result[3] = (float)sin(half);
    result[4] = (float)cos(half);
    result[5] = (float)tan(half);
    float logical[16];
    if (!pc_vr_projection_logical(result, logical)) return false;
    // Check all four native boundary rays in the game logical convention.
    // Extreme angles/rounding that cannot represent those edges fail closed.
    if (!pc_vr_projection_native_rays(fov, logical)) return false;
    memcpy(out, result, sizeof result);
    return true;
}

typedef struct {
    bool right_matches, parallel_eye, lateral_eye;
    int offset_sign;
} PcVrProjectionEyeBasis;

// Checks the stored base-view right row against the accepted camera's RH
// basis and the original eye copy before head rotation. No IPD/unit assumption.
static inline bool pc_vr_projection_eye_basis(const PcVrHeadLookCamera *base,
    const PcVrHeadLookCamera *eye, const float stored_right[3], PcVrProjectionEyeBasis *out) {
    if (!base || !eye || !stored_right || !out) return false;
    double right[3], up[3], back[3];
    if (!pc_vr_head_look_basis(base, right, up, back)) return false;
    PcVrProjectionEyeBasis result = {.right_matches = true, .parallel_eye = true};
    double offset[3], offset_length2 = 0, along = 0, world_scale = 1;
    for (unsigned i = 0; i < 3; ++i) {
        if (!isfinite(stored_right[i]) || !isfinite(eye->position[i]) ||
            !isfinite(eye->target[i]) || !isfinite(eye->up[i])) return false;
        result.right_matches &= fabs(stored_right[i] - right[i]) <= 0.0001;
        offset[i] = (double)eye->position[i] - base->position[i];
        along += offset[i] * right[i];
        offset_length2 += offset[i] * offset[i];
        const double source = (double)base->target[i] - base->position[i];
        const double copied = (double)eye->target[i] - eye->position[i];
        const double component_scale = fmax(fmax(fabs(base->position[i]), fabs(base->target[i])),
            fmax(fabs(eye->position[i]), fabs(eye->target[i])));
        world_scale = fmax(world_scale, component_scale);
        const double tolerance = 0.0001 * fmax(1.0, fabs(source)) + 4.0 * FLT_EPSILON * component_scale;
        result.parallel_eye &= fabs(source - copied) <= tolerance && fabs(base->up[i] - eye->up[i]) <= 0.0001;
    }
    double transverse2 = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const double transverse = offset[i] - along * right[i];
        transverse2 += transverse * transverse;
    }
    const double tolerance = 0.0001 * sqrt(offset_length2) + 8.0 * FLT_EPSILON * world_scale;
    result.lateral_eye = sqrt(transverse2) <= tolerance;
    result.offset_sign = along > 0.00001 ? 1 : along < -0.00001 ? -1 : 0;
    *out = result;
    return true;
}

// PRIMARY_STEREO specifies native index 0=left, 1=right. Verify that their
// separation points along the located central orientation's +X axis. The
// snapshot has no central position; do not invent one or infer meter scale.
static inline bool pc_vr_projection_native_order(const float central_q[4],
    const float left[3], const float right[3]) {
    if (!central_q || !left || !right) return false;
    double rotation[4], length_squared = 0, delta[3], length2 = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(central_q[i])) return false;
        rotation[i] = central_q[i]; length_squared += rotation[i] * rotation[i];
    }
    if (!(length_squared > 0.99 && length_squared < 1.01)) return false;
    for (unsigned i = 0; i < 4; ++i) rotation[i] /= sqrt(length_squared);
    for (unsigned i = 0; i < 3; ++i) {
        if (!isfinite(left[i]) || !isfinite(right[i])) return false;
        delta[i] = (double)right[i] - left[i]; length2 += delta[i] * delta[i];
    }
    if (!(length2 > 1e-18)) return false;
    const double local_right[3] = {1, 0, 0};
    double located_right[3];
    pc_vr_head_look_rotate(rotation, local_right, located_right);
    const double along = pc_vr_head_look_dot(delta, located_right);
    return along > 0 && along / sqrt(length2) >= 0.99;
}

#endif
