#ifndef SAVAGEWIND_LABO_SENSOR_SAMPLE_H
#define SAVAGEWIND_LABO_SENSOR_SAMPLE_H

// Complete records for BOTW 1.8.2's public SevenSix reader seam. Controlled
// diagnostics and the in-process PC VR adapter share the verified layout.
// This is not a HID transfer-memory implementation or a general headset map.
// The verified caller consumes only the ten floats after a positive return count.
// SDK continuity uses +0x08. The words at +0x00 and +0x10 remain opaque here:
// timestamp units and any validity meaning have not been established.
#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    LABO_SENSOR_SAMPLE_OFF = 0,
    LABO_SENSOR_SAMPLE_IDENTITY,
    LABO_SENSOR_SAMPLE_X20,
    LABO_SENSOR_SAMPLE_Y20,
    LABO_SENSOR_SAMPLE_Z20
} LaboSensorSampleMode;

typedef struct {
    uint64_t opaque_00;
    uint64_t sampling_number;
    uint64_t opaque_10;
    float vector_18[3];
    float vector_24[3];
    float quaternion_30[4];
} LaboSensorSample;

_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
               FLT_MAX_EXP == 128, "SevenSix diagnostic needs binary32 floats");
_Static_assert(sizeof(LaboSensorSample) == 0x40, "SevenSix public record size");
_Static_assert(offsetof(LaboSensorSample, opaque_00) == 0x00, "SevenSix +0x00");
_Static_assert(offsetof(LaboSensorSample, sampling_number) == 0x08, "SevenSix +0x08");
_Static_assert(offsetof(LaboSensorSample, opaque_10) == 0x10, "SevenSix +0x10");
_Static_assert(offsetof(LaboSensorSample, vector_18) == 0x18, "SevenSix +0x18");
_Static_assert(offsetof(LaboSensorSample, vector_24) == 0x24, "SevenSix +0x24");
_Static_assert(offsetof(LaboSensorSample, quaternion_30) == 0x30, "SevenSix +0x30");

// Missing, empty and unrecognized settings disable the experiment. Axis names
// refer to the quaternion after the verified helper's (-q0,q2,q1,q3) mapping;
// they do not claim a camera yaw/pitch/roll or an OpenXR reference space.
static inline LaboSensorSampleMode labo_sensor_sample_parse(const char *value) {
    if (!value) return LABO_SENSOR_SAMPLE_OFF;
    if (!strcmp(value, "identity")) return LABO_SENSOR_SAMPLE_IDENTITY;
    if (!strcmp(value, "x20")) return LABO_SENSOR_SAMPLE_X20;
    if (!strcmp(value, "y20")) return LABO_SENSOR_SAMPLE_Y20;
    if (!strcmp(value, "z20")) return LABO_SENSOR_SAMPLE_Z20;
    return LABO_SENSOR_SAMPLE_OFF;
}

static inline const char *labo_sensor_sample_name(LaboSensorSampleMode mode) {
    switch (mode) {
        case LABO_SENSOR_SAMPLE_IDENTITY: return "identity";
        case LABO_SENSOR_SAMPLE_X20: return "x20";
        case LABO_SENSOR_SAMPLE_Y20: return "y20";
        case LABO_SENSOR_SAMPLE_Z20: return "z20";
        default: return "off";
    }
}

// Build in trusted local storage. The caller must separately validate guest
// capacity >= 1 and a writable mapping covering all 0x40 bytes before copying
// ONE record and returning count 1. No raw guest pointer is validated here.
// Call only during the original active VR/SevenSix lifecycle at the verified
// reader seam. Ordinary controller SixAxis input must remain separate.
// Diagnostic sampling numbers start at 1; the owner prevents wraparound.
// Failed calls leave output untouched. Every successful call initializes all
// bytes. In controlled diagnostics opaque words and both vector triples stay
// zero; the production adapter fills +0x24 with body angular velocity after
// this builder. +0x18 remains zero. Header semantics beyond this consumer
// remain a research gate; these records are not general HID samples.
static inline int labo_sensor_sample_build_controlled(LaboSensorSample *out,
                                                     LaboSensorSampleMode mode,
                                                     uint64_t sampling_number,
                                                     int reference) {
    if (!out || !sampling_number || mode < LABO_SENSOR_SAMPLE_IDENTITY ||
        mode > LABO_SENSOR_SAMPLE_Z20) return 0;
    memset(out, 0, sizeof *out);
    out->sampling_number = sampling_number;
    out->quaternion_30[3] = 1.0f;
    if (mode != LABO_SENSOR_SAMPLE_IDENTITY) {
        // sin/cos of 0.5 or 10 degrees, rounded to binary32. Fixed modes avoid
        // parsing arbitrary, nonfinite or unnormalized quaternion data.
        const float sin_half = reference ? 0.008726535498373935f : 0.17364817766693033f;
        const float cos_half = reference ? 0.9999619230641713f : 0.98480775301220806f;
        out->quaternion_30[3] = cos_half;
        if (mode == LABO_SENSOR_SAMPLE_X20) out->quaternion_30[0] = -sin_half;
        if (mode == LABO_SENSOR_SAMPLE_Y20) out->quaternion_30[2] = sin_half;
        if (mode == LABO_SENSOR_SAMPLE_Z20) out->quaternion_30[1] = sin_half;
    }
    return 1;
}

static inline int labo_sensor_sample_build(LaboSensorSample *out,
                                          LaboSensorSampleMode mode,
                                          uint64_t sampling_number) {
    return labo_sensor_sample_build_controlled(out, mode, sampling_number, 0);
}

// The pinned downstream camera validity helper treats an exact identity matrix
// as invalid. A rotated diagnostic therefore starts and returns to a fixed one
// degree reference on the selected axis; the 20 degree stimulus is a 19 degree
// relative movement. Identity mode intentionally remains exact identity for the
// sentinel control. These helper axes still make no claim about camera/world axes.
static inline int labo_sensor_sample_build_reference(LaboSensorSample *out,
                                                    LaboSensorSampleMode mode,
                                                    uint64_t sampling_number) {
    return labo_sensor_sample_build_controlled(out, mode, sampling_number, 1);
}

#endif
