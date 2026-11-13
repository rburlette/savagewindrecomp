#ifndef SAVAGEWIND_LABO_POSE_ORIENTATION_H
#define SAVAGEWIND_LABO_POSE_ORIENTATION_H

// Orientation and angular-velocity formatting/recentering at the verified
// SevenSix seam. Tracking, transport, freshness and guest writes belong to callers.
#include <math.h>
#include <stdbool.h>

#include "labo_sensor_sample.h"

typedef struct {
    bool referenced;
    float reference[4]; // Normalized absolute quaternion, x/y/z/w.
    uint64_t generation;
} LaboPoseOrientation;

static inline void labo_pose_orientation_reset(LaboPoseOrientation *state) {
    if (state) memset(state, 0, sizeof *state);
}

static inline void labo_pose_orientation_multiply(const double first[4], const double second[4],
                                                  double out[4]) {
    out[0] = first[3] * second[0] + first[0] * second[3] + first[1] * second[2] - first[2] * second[1];
    out[1] = first[3] * second[1] - first[0] * second[2] + first[1] * second[3] + first[2] * second[0];
    out[2] = first[3] * second[2] + first[0] * second[1] - first[1] * second[0] + first[2] * second[3];
    out[3] = first[3] * second[3] - first[0] * second[0] - first[1] * second[1] - first[2] * second[2];
}

// Zero-initialize state before first use. Inputs and output must use separate
// storage. A generation change rebases at the current valid orientation. Invalid
// arguments or a quaternion norm outside [0.5, 1.5] clear the reference and leave
// output untouched. A subsequent valid pose establishes a new neutral reference.
// The caller validates tracking flags and age before invoking this function.
static inline bool labo_pose_orientation_sample(LaboPoseOrientation *state,
                                                const float absolute_xyzw[4],
                                                uint64_t generation, uint64_t serial,
                                                LaboSensorSample *out) {
    if (!state) return false;
    if (!absolute_xyzw || !out || !serial) goto invalid;
    double current[4], norm_squared = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(absolute_xyzw[i])) goto invalid;
        current[i] = absolute_xyzw[i];
        norm_squared += current[i] * current[i];
    }
    if (norm_squared < 0.25 || norm_squared > 2.25) goto invalid;
    double scale = 1.0 / sqrt(norm_squared);
    // q and -q represent the same rotation. Canonicalize even the w==0 case so
    // a representation sign change cannot change the captured reference.
    const unsigned order[4] = {3, 0, 1, 2};
    for (unsigned i = 0; i < 4; ++i) {
        if (current[order[i]] == 0) continue;
        if (current[order[i]] < 0) scale = -scale;
        break;
    }
    for (unsigned i = 0; i < 4; ++i) current[i] *= scale;

    const bool rebase = !state->referenced || state->generation != generation;
    double relative[4] = {0, 0, 0, 1};
    if (!rebase) {
        const double inverse_reference[4] = {-state->reference[0], -state->reference[1],
                                             -state->reference[2], state->reference[3]};
        labo_pose_orientation_multiply(inverse_reference, current, relative);
    }
    LaboSensorSample sample;
    if (!labo_sensor_sample_build_reference(&sample, LABO_SENSOR_SAMPLE_Y20, serial)) goto invalid;
    // Compose relative * fixed one-degree Y reference. The neutral sample must
    // avoid the original consumer's exact-identity sentinel. This is a bounded
    // input adapter, not evidence that headset and game world axes are calibrated.
    const double reference_bias[4] = {-sample.quaternion_30[0], sample.quaternion_30[2],
                                      sample.quaternion_30[1], sample.quaternion_30[3]};
    double composed[4];
    labo_pose_orientation_multiply(relative, reference_bias, composed);
    norm_squared = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (!isfinite(composed[i])) goto invalid;
        norm_squared += composed[i] * composed[i];
    }
    if (norm_squared < 0.25 || norm_squared > 2.25) goto invalid;
    scale = 1.0 / sqrt(norm_squared);
    // A head turn of -1 degree about Y exactly cancels the neutral reference.
    // The game's consumer treats an identity rotation matrix as unavailable.
    // Escape only the cancellation neighborhood with a 0.01-degree rotation;
    // this is a validity guard, not an accepted headset-to-camera axis map.
    if (fabs(composed[0]) + fabs(composed[1]) + fabs(composed[2]) < 0.000001) {
        composed[0] = composed[2] = 0;
        composed[1] = composed[3] < 0 ? -0.0000872664625 : 0.0000872664625;
        composed[3] = composed[3] < 0 ? -sqrt(1 - composed[1] * composed[1])
                                        : sqrt(1 - composed[1] * composed[1]);
        scale = 1;
    }
    // Inverse of the pinned helper's raw -> quaternion mapping (-q0,q2,q1,q3).
    sample.quaternion_30[0] = (float)(-composed[0] * scale);
    sample.quaternion_30[1] = (float)( composed[2] * scale);
    sample.quaternion_30[2] = (float)( composed[1] * scale);
    sample.quaternion_30[3] = (float)( composed[3] * scale);
    if (rebase) {
        for (unsigned i = 0; i < 4; ++i) state->reference[i] = (float)current[i];
        state->generation = generation;
        state->referenced = true;
    }
    memcpy(out, &sample, sizeof sample);
    return true;

invalid:
    labo_pose_orientation_reset(state);
    return false;
}

// Rotate a vector by a unit quaternion. This is used only for the SevenSix
// angular-velocity field; the game's original matrix/consumer stays in charge
// of interpreting its camera axes.
static inline void labo_pose_orientation_rotate_vector(const double rotation[4],
                                                       const double vector[3], double out[3]) {
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

// The pinned SevenSix helper maps raw +0x24 (-x,z,y) into the angular vector
// used by the original camera. Its current orientation matrix is conventional
// R(H), and the camera dots its Y row with that vector before multiplying by
// 360/30. Accordingly, convert OpenXR LOCAL-space rad/s to sensor-body turns/s
// using H^-1 * reference^-1. Turns/s is inferred from that game arithmetic;
// the private SDK sample's field units are not independently documented.
// The reference/bias exactly match the quaternion delivered in this record.
// Invalid velocity resets calibration and never publishes a partial sample.
static inline bool labo_pose_orientation_sample_motion(LaboPoseOrientation *state,
                                                       const float absolute_xyzw[4],
                                                       const float angular_velocity_xyz[3],
                                                       uint64_t generation, uint64_t serial,
                                                       LaboSensorSample *out) {
    if (!state) return false;
    if (!angular_velocity_xyz || !out) goto invalid;
    double omega[3];
    for (unsigned i = 0; i < 3; ++i) {
        if (!isfinite(angular_velocity_xyz[i])) goto invalid;
        omega[i] = angular_velocity_xyz[i] * (1.0 / 6.28318530717958647692);
    }
    LaboSensorSample sample;
    if (!labo_pose_orientation_sample(state, absolute_xyzw, generation, serial, &sample)) return false;
    const double inverse_reference[4] = {-state->reference[0], -state->reference[1],
                                         -state->reference[2], state->reference[3]};
    const double inverse_helper[4] = {sample.quaternion_30[0], -sample.quaternion_30[2],
                                     -sample.quaternion_30[1], sample.quaternion_30[3]};
    double rebased[3], body[3];
    labo_pose_orientation_rotate_vector(inverse_reference, omega, rebased);
    labo_pose_orientation_rotate_vector(inverse_helper, rebased, body);
    for (unsigned i = 0; i < 3; ++i) if (!isfinite(body[i])) goto invalid;
    sample.vector_24[0] = (float)-body[0];
    sample.vector_24[1] = (float) body[2];
    sample.vector_24[2] = (float) body[1];
    memcpy(out, &sample, sizeof sample);
    return true;

invalid:
    labo_pose_orientation_reset(state);
    return false;
}

#endif
