// PC VR: the head's view over both eyes, in the form the game's visibility code takes a view (pc_vr_cull_view.c).
//
// The game builds its visibility views from a look-at view matrix and a symmetric field of view (fovy and aspect, no
// offset). pc_vr_cull_view_build gives it one such view that holds both eyes' frusta: its axis is the eyes' common
// forward, its point lies midway between the eyes, pulled back until its side planes clear both eye positions, and its
// half angles are the largest any corner of either eye's frustum makes with that axis. For parallel eyes that is the
// larger outward angle to the sides and the larger of the up and down angles; eyes turned apart are held as well.
// Pure: no guest memory, no runtime state.
#pragma once
#include <math.h>
#include <stdbool.h>
#include "pc_vr_head_look_math.h"

// A corner of an eye's frustum further than this from the common axis has no symmetric view around it: 80 degrees.
#define PC_VR_CULL_VIEW_MIN_AXIS_COSINE 0.17364817766693033

typedef struct {
    // The view matrix as the game's look-at cameras hold it: three rows of four (right, up, back; translation last).
    float matrix[12];
    float fovy;    // radians, top to bottom
    float aspect;  // width over height
} PcVrCullView;

static inline bool pc_vr_cull_view_normalize(double vector[3]) {
    const double length = sqrt(pc_vr_head_look_dot(vector, vector));
    if (!(length > 1e-9) || !isfinite(length)) return false;
    for (unsigned i = 0; i < 3; ++i) vector[i] /= length;
    return true;
}

static inline void pc_vr_cull_view_cross(const double first[3], const double second[3], double out[3]) {
    out[0] = first[1] * second[2] - first[2] * second[1];
    out[1] = first[2] * second[0] - first[0] * second[2];
    out[2] = first[0] * second[1] - first[1] * second[0];
}

// An eye camera's right, up and forward unit vectors, as the game's look-at builder derives them (right = forward x up).
static inline bool pc_vr_cull_view_eye_axes(const PcVrHeadLookCamera *eye, double right[3], double up[3],
                                            double forward[3]) {
    double stored_up[3];
    for (unsigned i = 0; i < 3; ++i) {
        forward[i] = (double)eye->target[i] - eye->position[i];
        stored_up[i] = eye->up[i];
    }
    if (!pc_vr_cull_view_normalize(forward) || !pc_vr_cull_view_normalize(stored_up)) return false;
    pc_vr_cull_view_cross(forward, stored_up, right);
    // (an up vector along the view gives no right: the cross product of two unit vectors is the sine between them)
    if (!(pc_vr_head_look_dot(right, right) > 1e-6) || !pc_vr_cull_view_normalize(right)) return false;
    pc_vr_cull_view_cross(right, forward, up);
    return true;
}

// eyes: the two eye cameras in game coordinates. fov_left_right_up_down: each eye's field of view in radians as OpenXR
// gives it (left and down negative). False, with *out untouched, when a camera is degenerate, a field of view is not a
// frustum, or a corner lies 80 degrees or more from the common axis.
static inline bool pc_vr_cull_view_build(const PcVrHeadLookCamera eyes[2], const float fov_left_right_up_down[2][4],
                                         PcVrCullView *out) {
    double eye_right[2][3], eye_up[2][3], eye_forward[2][3];
    for (unsigned side = 0; side < 2; ++side) {
        const float *fov = fov_left_right_up_down[side];
        if (!pc_vr_cull_view_eye_axes(&eyes[side], eye_right[side], eye_up[side], eye_forward[side])) return false;
        for (unsigned i = 0; i < 4; ++i)
            if (!isfinite(fov[i]) || fabsf(fov[i]) >= 1.5533430f) return false;  // (89 degrees)
        if (!(fov[0] < fov[1]) || !(fov[3] < fov[2])) return false;
    }
    double forward[3], right[3], up[3], summed_up[3];
    for (unsigned i = 0; i < 3; ++i) {
        forward[i] = eye_forward[0][i] + eye_forward[1][i];
        summed_up[i] = eye_up[0][i] + eye_up[1][i];
    }
    if (!pc_vr_cull_view_normalize(forward)) return false;
    pc_vr_cull_view_cross(forward, summed_up, right);
    if (!pc_vr_cull_view_normalize(right)) return false;
    pc_vr_cull_view_cross(right, forward, up);
    // The largest tangents any corner of either eye's frustum makes with the common axis.
    double tangent_side = 0, tangent_vertical = 0;
    for (unsigned side = 0; side < 2; ++side) {
        const float *fov = fov_left_right_up_down[side];
        for (unsigned corner = 0; corner < 4; ++corner) {
            const double across = tan((double)fov[corner & 1]), along = tan((double)fov[2 + (corner >> 1)]);
            double direction[3];
            for (unsigned i = 0; i < 3; ++i)
                direction[i] = eye_forward[side][i] + across * eye_right[side][i] + along * eye_up[side][i];
            const double depth = pc_vr_head_look_dot(direction, forward);
            if (!(depth > PC_VR_CULL_VIEW_MIN_AXIS_COSINE * sqrt(pc_vr_head_look_dot(direction, direction)))) return false;
            tangent_side = fmax(tangent_side, fabs(pc_vr_head_look_dot(direction, right)) / depth);
            tangent_vertical = fmax(tangent_vertical, fabs(pc_vr_head_look_dot(direction, up)) / depth);
        }
    }
    if (!(tangent_side > 1e-6) || !(tangent_vertical > 1e-6)) return false;
    // The view's point: midway between the eyes, pulled back so that a side plane through it passes outside an eye
    // half the eye distance to the side (and above or below, for eyes that are not level in the common frame).
    double separation[3], point[3];
    for (unsigned i = 0; i < 3; ++i) {
        separation[i] = 0.5 * ((double)eyes[1].position[i] - eyes[0].position[i]);
        point[i] = 0.5 * ((double)eyes[0].position[i] + eyes[1].position[i]);
    }
    const double pull_back = fmax(fabs(pc_vr_head_look_dot(separation, right)) / tangent_side,
                                  fabs(pc_vr_head_look_dot(separation, up)) / tangent_vertical) +
        fabs(pc_vr_head_look_dot(separation, forward));
    for (unsigned i = 0; i < 3; ++i) point[i] -= pull_back * forward[i];
    PcVrCullView view;
    for (unsigned i = 0; i < 3; ++i) {
        view.matrix[i] = (float)right[i];
        view.matrix[4 + i] = (float)up[i];
        view.matrix[8 + i] = (float)-forward[i];
    }
    view.matrix[3] = (float)-pc_vr_head_look_dot(right, point);
    view.matrix[7] = (float)-pc_vr_head_look_dot(up, point);
    view.matrix[11] = (float)pc_vr_head_look_dot(forward, point);
    view.fovy = (float)(2.0 * atan(tangent_vertical));
    view.aspect = (float)(tangent_side / tangent_vertical);
    for (unsigned i = 0; i < 12; ++i)
        if (!isfinite(view.matrix[i])) return false;
    if (!isfinite(view.fovy) || !isfinite(view.aspect)) return false;
    *out = view;
    return true;
}
