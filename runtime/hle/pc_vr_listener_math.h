#ifndef SAVAGEWIND_PC_VR_LISTENER_MATH_H
#define SAVAGEWIND_PC_VR_LISTENER_MATH_H

// Pure math for the sound listener that follows the head (pc_vr_listener.c): the head's offset from the game camera,
// taken from the frame record's eye cameras, and its application to a view matrix of the game's. No guest access.
//
// The game's views are row-major 3x4 matrices from world space to a view space whose rows are the camera's right, up
// and back (its look-at builder; the camera looks along -back), with the translation -R * position in the last column.
#include <math.h>
#include <stdbool.h>
#include "pc_vr_head_look_math.h"

// The head's offset from the camera, from the camera's view space to the head's: the head's view is this offset times
// the camera's view. Rotation rows are the head's axes in the camera's frame.
typedef struct {
    double rotation[3][3];
    double translation[3];
} PcVrListenerOffset;

// The head of a pair of eye cameras: at their midpoint, looking along their mean forward with their mean up (the
// same as either eye when they are parallel; canted eyes average). False: an eye without a direction or an up.
static inline bool pc_vr_listener_head_axes(const PcVrHeadLookCamera eyes[2], double axes[3][3], double position[3]) {
    double forward[3] = {0, 0, 0}, up_hint[3] = {0, 0, 0};
    for (unsigned eye = 0; eye < 2; ++eye) {
        double eye_forward[3], eye_up[3];
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (!isfinite(eyes[eye].position[axis]) || !isfinite(eyes[eye].target[axis]) ||
                !isfinite(eyes[eye].up[axis])) return false;
            eye_forward[axis] = (double)eyes[eye].target[axis] - eyes[eye].position[axis];
            eye_up[axis] = eyes[eye].up[axis];
        }
        if (!pc_vr_head_look_normalize(eye_forward, 1e-9) || !pc_vr_head_look_normalize(eye_up, 1e-9)) return false;
        for (unsigned axis = 0; axis < 3; ++axis) {
            forward[axis] += eye_forward[axis];
            up_hint[axis] += eye_up[axis];
        }
    }
    if (!pc_vr_head_look_normalize(forward, 1e-6) || !pc_vr_head_look_normalize(up_hint, 1e-6)) return false;
    double right[3], up[3];
    pc_vr_head_look_cross(forward, up_hint, right);
    if (!pc_vr_head_look_normalize(right, 1e-6)) return false;
    pc_vr_head_look_cross(right, forward, up);
    for (unsigned axis = 0; axis < 3; ++axis) {
        axes[0][axis] = right[axis];
        axes[1][axis] = up[axis];
        axes[2][axis] = -forward[axis];
        position[axis] = 0.5 * ((double)eyes[0].position[axis] + eyes[1].position[axis]);
    }
    return true;
}

// The offset of the head of `eyes` from `camera` (the base camera the eyes were built from). False, out untouched:
// a camera or an eye without a basis.
static inline bool pc_vr_listener_offset_build(const PcVrHeadLookCamera *camera, const PcVrHeadLookCamera eyes[2],
                                               PcVrListenerOffset *out) {
    double camera_axes[3][3], head_axes[3][3], head_position[3];
    if (!camera || !eyes || !out ||
        !pc_vr_head_look_basis(camera, camera_axes[0], camera_axes[1], camera_axes[2]) ||
        !pc_vr_listener_head_axes(eyes, head_axes, head_position)) return false;
    PcVrListenerOffset offset;
    double camera_from_head[3];
    for (unsigned axis = 0; axis < 3; ++axis) camera_from_head[axis] = (double)camera->position[axis] - head_position[axis];
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned column = 0; column < 3; ++column)
            offset.rotation[row][column] = pc_vr_head_look_dot(head_axes[row], camera_axes[column]);
        offset.translation[row] = pc_vr_head_look_dot(head_axes[row], camera_from_head);
    }
    *out = offset;
    return true;
}

// A view of the game's (world to a frame it places by the camera) turned and moved with the head: offset times view.
// `out` may alias `view`.
static inline void pc_vr_listener_offset_apply(const PcVrListenerOffset *offset, const float view[12], float out[12]) {
    float result[12];
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 4; ++column) {
            double sum = column == 3 ? offset->translation[row] : 0;
            for (unsigned inner = 0; inner < 3; ++inner) sum += offset->rotation[row][inner] * view[inner * 4 + column];
            result[row * 4 + column] = (float)sum;
        }
    for (unsigned index = 0; index < 12; ++index) out[index] = result[index];
}

#endif
