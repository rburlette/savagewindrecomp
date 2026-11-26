#ifndef SAVAGEWIND_PC_VR_PROJECTION_MATH_H
#define SAVAGEWIND_PC_VR_PROJECTION_MATH_H

// Pure mapping into the pinned game's existing perspective parameters. This
// is not a replacement projection/tracking framework: the original logical
// writer and posture/device conversion remain authoritative for guest output.
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "pc_vr_head_look_math.h"

static inline bool pc_vr_projection_close(double actual, double expected) {
    return isfinite(actual) && isfinite(expected) && fabs(actual - expected) <= 0.00002 * fmax(1.0, fabs(expected));
}

// The logical matrix gives the FOV's frustum with its picture moved by offset_ndc (x, y in NDC: a temporal
// anti-aliasing offset, pc_vr_projection_jitter; 0, 0 for none): each corner ray lands at its corner of the
// [-1, 1] square plus the offset.
static inline bool pc_vr_projection_native_rays_offset(const float fov[4], const float offset_ndc[2],
                                                       const float matrix[16]) {
    if (!fov || !offset_ndc || !matrix || !isfinite(offset_ndc[0]) || !isfinite(offset_ndc[1])) return false;
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
                if (!(clip[3] > 0) ||
                    !pc_vr_projection_close(clip[0] / clip[3], (corner_x ? 1.0 : -1.0) + offset_ndc[0]) ||
                    !pc_vr_projection_close(clip[1] / clip[3], (corner_y ? 1.0 : -1.0) + offset_ndc[1]))
                    return false;
        }
    return true;
}

static inline bool pc_vr_projection_native_rays(const float fov[4], const float matrix[16]) {
    static const float no_offset[2] = {0, 0};
    return pc_vr_projection_native_rays_offset(fov, no_offset, matrix);
}

// The radical inverse of index in base (Halton's sequence), in [0, 1).
static inline double pc_vr_projection_halton(uint64_t index, unsigned base) {
    double fraction = 1, result = 0;
    for (; index; index /= base) fraction /= base, result += fraction * (double)(index % base);
    return result;
}

// A frame record's sub-pixel offset for temporal anti-aliasing per eye: the Halton (2, 3) point of its phase
// ((serial % phases) + 1: Halton's point 0 is 0, 0), taken from [0, 1) to [-0.5, 0.5] pixels of a width x height
// image, as the picture's move in NDC (2 / width per pixel). false, out untouched: no size or no phases.
static inline bool pc_vr_projection_jitter(uint64_t serial, unsigned phases, unsigned width, unsigned height,
                                           float out[2]) {
    if (!out || !phases || !width || !height) return false;
    const uint64_t index = serial % phases + 1;
    out[0] = (float)((pc_vr_projection_halton(index, 2) - 0.5) * 2.0 / width);
    out[1] = (float)((pc_vr_projection_halton(index, 3) - 0.5) * 2.0 / height);
    return true;
}

// The parameters (n, f, fovy, sin, cos, tan, aspect, centre x, centre y) with the centre moved so that the logical
// matrix built from them (pc_vr_projection_logical: [0][2] = 2 centre x, [1][2] = 2 centre y, and NDC = ... - [i][2])
// moves the picture by offset_ndc.
static inline void pc_vr_projection_offset_centre(float params[9], const float offset_ndc[2]) {
    params[7] = (float)((double)params[7] - 0.5 * offset_ndc[0]);
    params[8] = (float)((double)params[8] - 0.5 * offset_ndc[1]);
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
    bool right_matches, lateral_eye;
    int offset_sign;
} PcVrProjectionEyeBasis;

// Checks the stored base-view right row against the accepted camera's RH
// basis, and where the eye copy sits from the base camera (that it is a copy
// of it, head look's freshly_copied_eye says). No IPD/unit assumption.
static inline bool pc_vr_projection_eye_basis(const PcVrHeadLookCamera *base,
    const PcVrHeadLookCamera *eye, const float stored_right[3], PcVrProjectionEyeBasis *out) {
    if (!base || !eye || !stored_right || !out) return false;
    double right[3], up[3], back[3];
    if (!pc_vr_head_look_basis(base, right, up, back)) return false;
    PcVrProjectionEyeBasis result = {.right_matches = true};
    double offset[3], offset_length2 = 0, along = 0, world_scale = 1;
    for (unsigned i = 0; i < 3; ++i) {
        if (!isfinite(stored_right[i]) || !isfinite(eye->position[i]) ||
            !isfinite(eye->target[i]) || !isfinite(eye->up[i])) return false;
        result.right_matches &= fabs(stored_right[i] - right[i]) <= 0.0001;
        offset[i] = (double)eye->position[i] - base->position[i];
        along += offset[i] * right[i];
        offset_length2 += offset[i] * offset[i];
        world_scale = fmax(world_scale, fmax(fmax(fabs(base->position[i]), fabs(base->target[i])),
                                             fmax(fabs(eye->position[i]), fabs(eye->target[i]))));
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

#endif
