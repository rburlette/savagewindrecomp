#ifndef SAVAGEWIND_PC_VR_HEAD_LOOK_MATH_H
#define SAVAGEWIND_PC_VR_HEAD_LOOK_MATH_H

// Pure third-person head-look composition. `base` is the controller camera
// for this update; relative_xyzw is a calibrated OpenXR-style right-handed
// local rotation (forward -Z, up +Y). Position is copied byte-for-byte.
// Camera anchoring, recenter/generation policy, guest writes and eye transforms
// belong to the caller. Nothing here accumulates from a prior head-look result.
#include <math.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    float position[3];
    float target[3];
    float up[3];
} PcVrHeadLookCamera;

static inline double pc_vr_head_look_dot(const double first[3], const double second[3]) {
    return first[0] * second[0] + first[1] * second[1] + first[2] * second[2];
}

static inline void pc_vr_head_look_cross(const double first[3], const double second[3], double out[3]) {
    out[0] = first[1] * second[2] - first[2] * second[1];
    out[1] = first[2] * second[0] - first[0] * second[2];
    out[2] = first[0] * second[1] - first[1] * second[0];
}

static inline bool pc_vr_head_look_normalize(double vector[3], double minimum_length) {
    const double length_squared = pc_vr_head_look_dot(vector, vector);
    if (!isfinite(length_squared) || length_squared <= minimum_length * minimum_length) return false;
    const double inverse = 1.0 / sqrt(length_squared);
    for (unsigned i = 0; i < 3; ++i) vector[i] *= inverse;
    return true;
}

// Equivalent to the Khronos OpenXR xr_linear.h quaternion-vector product
// q * (v,0) * inverse(q), with q normalized before this call.
static inline void pc_vr_head_look_rotate(const double rotation[4], const double vector[3], double out[3]) {
    const double doubled_cross[3] = {2 * (rotation[1] * vector[2] - rotation[2] * vector[1]),
                         2 * (rotation[2] * vector[0] - rotation[0] * vector[2]),
                         2 * (rotation[0] * vector[1] - rotation[1] * vector[0])};
    out[0] =
        vector[0] + rotation[3] * doubled_cross[0] + rotation[1] * doubled_cross[2] - rotation[2] * doubled_cross[1];
    out[1] =
        vector[1] + rotation[3] * doubled_cross[1] + rotation[2] * doubled_cross[0] - rotation[0] * doubled_cross[2];
    out[2] =
        vector[2] + rotation[3] * doubled_cross[2] + rotation[0] * doubled_cross[1] - rotation[1] * doubled_cross[0];
}

static inline bool pc_vr_head_look_apply(const PcVrHeadLookCamera *base,
                                         const float relative_xyzw[4],
                                         PcVrHeadLookCamera *out) {
    if (!base || !relative_xyzw || !out) return false;
    double rotation[4], length_squared = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(relative_xyzw[i])) return false;
        rotation[i] = relative_xyzw[i];
        length_squared += rotation[i] * rotation[i];
    }
    if (length_squared < 0.25 || length_squared > 2.25) return false;
    const double q_inverse_length = 1.0 / sqrt(length_squared);
    for (unsigned i = 0; i < 4; ++i) rotation[i] *= q_inverse_length;

    double forward[3], up_hint[3];
    for (unsigned i = 0; i < 3; ++i) {
        if (!isfinite(base->position[i]) || !isfinite(base->target[i]) ||
            !isfinite(base->up[i])) return false;
        forward[i] = (double)base->target[i] - base->position[i];
        up_hint[i] = base->up[i];
    }
    const double radius = sqrt(pc_vr_head_look_dot(forward, forward));
    const double up_length = sqrt(pc_vr_head_look_dot(up_hint, up_hint));
    if (!isfinite(radius) || !isfinite(up_length) || radius <= 1e-9 || up_length <= 1e-9 ||
        !pc_vr_head_look_normalize(forward, 1e-9) ||
        !pc_vr_head_look_normalize(up_hint, 1e-9)) return false;
    double right[3], camera_up[3];
    pc_vr_head_look_cross(forward, up_hint, right);
    if (!pc_vr_head_look_normalize(right, 1e-6)) return false;
    pc_vr_head_look_cross(right, forward, camera_up);

    const double local_forward[3] = {0, 0, -1}, local_up[3] = {0, 1, 0};
    double rotated_forward[3], rotated_up[3], world_forward[3], world_up[3];
    pc_vr_head_look_rotate(rotation, local_forward, rotated_forward);
    pc_vr_head_look_rotate(rotation, local_up, rotated_up);
    for (unsigned i = 0; i < 3; ++i) {
        world_forward[i] = right[i] * rotated_forward[0] +
                           camera_up[i] * rotated_forward[1] - forward[i] * rotated_forward[2];
        world_up[i] = right[i] * rotated_up[0] +
                      camera_up[i] * rotated_up[1] - forward[i] * rotated_up[2];
    }
    if (!pc_vr_head_look_normalize(world_forward, 1e-9)) return false;
    const double up_projection = pc_vr_head_look_dot(world_up, world_forward);
    for (unsigned i = 0; i < 3; ++i) world_up[i] -= up_projection * world_forward[i];
    if (!pc_vr_head_look_normalize(world_up, 1e-9)) return false;

    PcVrHeadLookCamera result;
    memcpy(&result, base, sizeof result);
    for (unsigned i = 0; i < 3; ++i) {
        const double target = (double)base->position[i] + radius * world_forward[i];
        const double up = up_length * world_up[i];
        if (!isfinite(target) || !isfinite(up) ||
            !isfinite((float)target) || !isfinite((float)up)) return false;
        result.target[i] = (float)target;
        result.up[i] = (float)up;
    }
    double delivered_radius_squared = 0;
    for (unsigned i = 0; i < 3; ++i) {
        double delta = (double)result.target[i] - result.position[i];
        delivered_radius_squared += delta * delta;
    }
    if (!isfinite(delivered_radius_squared) || delivered_radius_squared <= 0) return false;
    memcpy(out, &result, sizeof result);
    return true;
}

static inline bool pc_vr_head_look_basis(const PcVrHeadLookCamera *camera,
                                        double right[3], double up[3], double back[3]) {
    double forward[3], up_hint[3];
    for (unsigned i = 0; i < 3; ++i) {
        if (!isfinite(camera->position[i]) || !isfinite(camera->target[i]) ||
            !isfinite(camera->up[i])) return false;
        forward[i] = (double)camera->target[i] - camera->position[i];
        up_hint[i] = camera->up[i];
    }
    if (!pc_vr_head_look_normalize(forward, 1e-9) ||
        !pc_vr_head_look_normalize(up_hint, 1e-9)) return false;
    pc_vr_head_look_cross(forward, up_hint, right);
    if (!pc_vr_head_look_normalize(right, 1e-6)) return false;
    pc_vr_head_look_cross(right, forward, up);
    for (unsigned i = 0; i < 3; ++i) back[i] = -forward[i];
    return true;
}

static inline void pc_vr_head_look_transform(const double right[3], const double up[3],
                                             const double back[3], const double new_right[3],
                                             const double new_up[3], const double new_back[3],
                                             const double vector[3], double out[3]) {
    const double local[3] = {pc_vr_head_look_dot(vector, right),
                             pc_vr_head_look_dot(vector, up),
                             pc_vr_head_look_dot(vector, back)};
    for (unsigned i = 0; i < 3; ++i)
        out[i] = local[0] * new_right[i] + local[1] * new_up[i] + local[2] * new_back[i];
}

// Compose head rotation onto a render-owned eye camera without moving the
// controller center. Rotate eye position/target offsets about that center and
// rotate eye up by the same B*Q*B^-1 transform. This retains the original eye
// offset/IPD and eye look radius; it does not choose either eye or alter FOV.
// `out_eye` may alias `eye`, but must not alias `center`. Failure is atomic.
static inline bool pc_vr_head_look_apply_eye(const PcVrHeadLookCamera *center,
                                             const PcVrHeadLookCamera *eye,
                                             const float relative_xyzw[4],
                                             PcVrHeadLookCamera *out_eye) {
    if (!center || !eye || !relative_xyzw || !out_eye || out_eye == center) return false;
    PcVrHeadLookCamera rotated_center;
    if (!pc_vr_head_look_apply(center, relative_xyzw, &rotated_center)) return false;
    double right[3], up[3], back[3], new_right[3], new_up[3], new_back[3];
    if (!pc_vr_head_look_basis(center, right, up, back) ||
        !pc_vr_head_look_basis(&rotated_center, new_right, new_up, new_back)) return false;
    double eye_offset[3], target_offset[3], eye_up[3];
    for (unsigned i = 0; i < 3; ++i) {
        if (!isfinite(eye->position[i]) || !isfinite(eye->target[i]) ||
            !isfinite(eye->up[i])) return false;
        eye_offset[i] = (double)eye->position[i] - center->position[i];
        target_offset[i] = (double)eye->target[i] - center->position[i];
        eye_up[i] = eye->up[i];
    }
    double rotated_eye_offset[3], rotated_target_offset[3], rotated_eye_up[3];
    pc_vr_head_look_transform(right, up, back, new_right, new_up, new_back,
                              eye_offset, rotated_eye_offset);
    pc_vr_head_look_transform(right, up, back, new_right, new_up, new_back,
                              target_offset, rotated_target_offset);
    pc_vr_head_look_transform(right, up, back, new_right, new_up, new_back,
                              eye_up, rotated_eye_up);
    PcVrHeadLookCamera result;
    for (unsigned i = 0; i < 3; ++i) {
        double position = (double)center->position[i] + rotated_eye_offset[i];
        double target = (double)center->position[i] + rotated_target_offset[i];
        double up_component = rotated_eye_up[i];
        if (!isfinite(position) || !isfinite(target) || !isfinite(up_component) ||
            !isfinite((float)position) || !isfinite((float)target) ||
            !isfinite((float)up_component)) return false;
        result.position[i] = (float)position;
        result.target[i] = (float)target;
        result.up[i] = (float)up_component;
    }
    if (pc_vr_head_look_dot(rotated_eye_offset, rotated_eye_offset) > 0 &&
        memcmp(result.position, center->position, sizeof result.position) == 0) return false;
    memcpy(out_eye, &result, sizeof result);
    return true;
}

#endif
