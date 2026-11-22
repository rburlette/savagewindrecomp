// Standalone, asset-free contract checks:
// clang -std=c11 -O1 -Wall -Wextra -Werror tools/test_labo_sensor_sample.c -o test_labo_sensor_sample
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../runtime/hle/labo_sensor_sample.h"

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static int near(double actual, double expected) {
    return fabs(actual - expected) < 0.000001;
}

// Derived arithmetic from the pinned main helper's nine consecutive stored
// floats (+0x24..+0x44). Test-only: the runtime must keep the original helper.
// Its packing is transposed relative to an active, row-major rotation matrix.
static void helper_packed_matrix(const LaboSensorSample *sample, double out[9]) {
    const double a = sample->quaternion_30[0];
    const double b = sample->quaternion_30[1];
    const double c = sample->quaternion_30[2];
    const double d = sample->quaternion_30[3];
    out[0] = 1 - 2 * (b * b + c * c);
    out[1] = 2 * (b * d - a * c);
    out[2] = -2 * (a * b + c * d);
    out[3] = -2 * (a * c + b * d);
    out[4] = 1 - 2 * (a * a + b * b);
    out[5] = 2 * (b * c - a * d);
    out[6] = 2 * (c * d - a * b);
    out[7] = 2 * (a * d + b * c);
    out[8] = 1 - 2 * (a * a + c * c);
}

static void check_mode(LaboSensorSampleMode mode, const double expected[9],
                       unsigned active_quaternion_lane, double active_value, int reference) {
    LaboSensorSample sample;
    memset(&sample, 0xa5, sizeof sample);
    CHECK(reference ? labo_sensor_sample_build_reference(&sample, mode, UINT64_C(0x102030405060708))
                    : labo_sensor_sample_build(&sample, mode, UINT64_C(0x102030405060708)));
    CHECK(sample.sampling_number == UINT64_C(0x102030405060708));
    CHECK(sample.opaque_00 == 0 && sample.opaque_10 == 0);
    const unsigned char *bytes = (const unsigned char *)&sample;
    for (unsigned i = 0; i < sizeof sample; ++i) {
        if (i >= 8 && i < 16) continue; // Deliberate sampling-number bytes.
        if (i >= 0x3c) continue;        // Scalar quaternion component.
        if (mode != LABO_SENSOR_SAMPLE_IDENTITY &&
            i >= 0x30 + active_quaternion_lane * 4 &&
            i < 0x34 + active_quaternion_lane * 4) continue;
        CHECK(bytes[i] == 0); // Includes every opaque/vector/unused lane byte.
    }
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(sample.vector_18[i] == 0.0f && sample.vector_24[i] == 0.0f);
        CHECK(near(sample.quaternion_30[i],
                   mode != LABO_SENSOR_SAMPLE_IDENTITY && i == active_quaternion_lane ? active_value : 0));
    }
    double norm = 0;
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(isfinite(sample.quaternion_30[i]));
        norm += (double)sample.quaternion_30[i] * sample.quaternion_30[i];
    }
    CHECK(near(norm, 1));
    double actual[9];
    helper_packed_matrix(&sample, actual);
    for (unsigned i = 0; i < 9; ++i) CHECK(near(actual[i], expected[i]));
    // Orthonormal basis and positive determinant guard rotation/reflection errors.
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned s = 0; s < 3; ++s) {
            double dot = 0;
            for (unsigned i = 0; i < 3; ++i) dot += actual[3 * r + i] * actual[3 * s + i];
            CHECK(near(dot, r == s ? 1 : 0));
        }
    }
    const double determinant = actual[0] * (actual[4] * actual[8] - actual[5] * actual[7]) -
                               actual[1] * (actual[3] * actual[8] - actual[5] * actual[6]) +
                               actual[2] * (actual[3] * actual[7] - actual[4] * actual[6]);
    CHECK(near(determinant, 1));
}

static void check_relative(LaboSensorSampleMode mode, const double expected[9]) {
    LaboSensorSample reference, stimulus;
    CHECK(labo_sensor_sample_build_reference(&reference, mode, 1));
    CHECK(labo_sensor_sample_build(&stimulus, mode, 2));
    double a[9], b[9];
    helper_packed_matrix(&reference, a);
    helper_packed_matrix(&stimulus, b);
    CHECK(a[0] != 1 || a[4] != 1 || a[8] != 1); // Nonidentity camera reference.
    // inverse(reference) * stimulus: the same-axis relative rotation is +19 degrees.
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned col = 0; col < 3; ++col) {
            double value = 0;
            for (unsigned i = 0; i < 3; ++i) value += a[3 * i + row] * b[3 * i + col];
            CHECK(near(value, expected[3 * row + col]));
        }
    }
    // A return to the reference reconstructs the same fully initialized record.
    LaboSensorSample returned;
    memset(&returned, 0xa5, sizeof returned);
    CHECK(labo_sensor_sample_build_reference(&returned, mode, 1));
    CHECK(!memcmp(&returned, &reference, sizeof reference));
}

int main(void) {
    const char *invalid[] = {NULL, "", "off", "1", "true", "Identity", "identity ",
                             " identity", "X20", "y-20", "y20\n", "y20extra",
                             "nan", "inf", "1,0,0,1", "yaw20", "x360"};
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        CHECK(labo_sensor_sample_parse(invalid[i]) == LABO_SENSOR_SAMPLE_OFF);
    const char *names[] = {"off", "identity", "x20", "y20", "z20"};
    for (int i = LABO_SENSOR_SAMPLE_IDENTITY; i <= LABO_SENSOR_SAMPLE_Z20; ++i) {
        CHECK(labo_sensor_sample_parse(names[i]) == (LaboSensorSampleMode)i);
        CHECK(!strcmp(labo_sensor_sample_name((LaboSensorSampleMode)i), names[i]));
    }
    CHECK(!strcmp(labo_sensor_sample_name(LABO_SENSOR_SAMPLE_OFF), "off"));
    CHECK(!strcmp(labo_sensor_sample_name((LaboSensorSampleMode)-1), "off"));
    CHECK(!strcmp(labo_sensor_sample_name((LaboSensorSampleMode)99), "off"));
    LaboSensorSample sample, unchanged;
    memset(&sample, 0xa5, sizeof sample);
    memcpy(&unchanged, &sample, sizeof sample);
    CHECK(!labo_sensor_sample_build(NULL, LABO_SENSOR_SAMPLE_IDENTITY, 1));
    CHECK(!labo_sensor_sample_build(&sample, LABO_SENSOR_SAMPLE_OFF, 1));
    CHECK(!labo_sensor_sample_build(&sample, (LaboSensorSampleMode)-1, 1));
    CHECK(!labo_sensor_sample_build(&sample, (LaboSensorSampleMode)99, 1));
    CHECK(!labo_sensor_sample_build(&sample, LABO_SENSOR_SAMPLE_IDENTITY, 0));
    CHECK(!memcmp(&sample, &unchanged, sizeof sample));
    CHECK(!labo_sensor_sample_build_reference(NULL, LABO_SENSOR_SAMPLE_IDENTITY, 1));
    CHECK(!labo_sensor_sample_build_reference(&sample, LABO_SENSOR_SAMPLE_OFF, 1));
    CHECK(!labo_sensor_sample_build_reference(&sample, (LaboSensorSampleMode)-1, 1));
    CHECK(!labo_sensor_sample_build_reference(&sample, (LaboSensorSampleMode)99, 1));
    CHECK(!labo_sensor_sample_build_reference(&sample, LABO_SENSOR_SAMPLE_IDENTITY, 0));
    CHECK(!memcmp(&sample, &unchanged, sizeof sample));

    // Independent expected axis matrices at +20 degrees in helper quaternion
    // coordinates. Transposed storage is intentional; world axes remain unknown.
    const double c = 0.9396926207859084, s = 0.3420201433256687;
    const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double x20[9] = {1, 0, 0, 0, c, s, 0, -s, c};
    const double y20[9] = {c, 0, -s, 0, 1, 0, s, 0, c};
    const double z20[9] = {c, s, 0, -s, c, 0, 0, 0, 1};
    check_mode(LABO_SENSOR_SAMPLE_IDENTITY, identity, 0, 0, 0);
    check_mode(LABO_SENSOR_SAMPLE_X20, x20, 0, -0.17364817766693033, 0);
    check_mode(LABO_SENSOR_SAMPLE_Y20, y20, 2, 0.17364817766693033, 0);
    check_mode(LABO_SENSOR_SAMPLE_Z20, z20, 1, 0.17364817766693033, 0);
    const double c1 = 0.9998476951563913, s1 = 0.01745240643728351;
    const double x1[9] = {1, 0, 0, 0, c1, s1, 0, -s1, c1};
    const double y1[9] = {c1, 0, -s1, 0, 1, 0, s1, 0, c1};
    const double z1[9] = {c1, s1, 0, -s1, c1, 0, 0, 0, 1};
    check_mode(LABO_SENSOR_SAMPLE_IDENTITY, identity, 0, 0, 1);
    check_mode(LABO_SENSOR_SAMPLE_X20, x1, 0, -0.008726535498373935, 1);
    check_mode(LABO_SENSOR_SAMPLE_Y20, y1, 2, 0.008726535498373935, 1);
    check_mode(LABO_SENSOR_SAMPLE_Z20, z1, 1, 0.008726535498373935, 1);
    const double c19 = 0.9455185755993168, s19 = 0.3255681544571567;
    const double x19[9] = {1, 0, 0, 0, c19, s19, 0, -s19, c19};
    const double y19[9] = {c19, 0, -s19, 0, 1, 0, s19, 0, c19};
    const double z19[9] = {c19, s19, 0, -s19, c19, 0, 0, 0, 1};
    check_relative(LABO_SENSOR_SAMPLE_X20, x19);
    check_relative(LABO_SENSOR_SAMPLE_Y20, y19);
    check_relative(LABO_SENSOR_SAMPLE_Z20, z19);

    // Full-record writes remain bounded even when the reference builder is used.
    struct { uint64_t before; LaboSensorSample record; uint64_t after; } guarded;
    guarded.before = UINT64_C(0x1122334455667788);
    guarded.after = UINT64_C(0x8877665544332211);
    CHECK(labo_sensor_sample_build_reference(&guarded.record, LABO_SENSOR_SAMPLE_Y20, UINT64_MAX));
    CHECK(guarded.before == UINT64_C(0x1122334455667788));
    CHECK(guarded.after == UINT64_C(0x8877665544332211));
    CHECK(guarded.record.sampling_number == UINT64_MAX);

    // The caller may build a deterministic A/B/A timeline without stale fields.
    CHECK(labo_sensor_sample_build(&sample, LABO_SENSOR_SAMPLE_IDENTITY, 1));
    CHECK(labo_sensor_sample_build(&sample, LABO_SENSOR_SAMPLE_Y20, 2));
    CHECK(sample.sampling_number == 2 && sample.quaternion_30[2] > 0);
    CHECK(labo_sensor_sample_build(&sample, LABO_SENSOR_SAMPLE_IDENTITY, 3));
    CHECK(sample.sampling_number == 3 && sample.quaternion_30[2] == 0 && sample.quaternion_30[3] == 1);
    CHECK(labo_sensor_sample_build(&sample, LABO_SENSOR_SAMPLE_IDENTITY, UINT64_MAX));
    CHECK(sample.sampling_number == UINT64_MAX);
    printf("Labo sensor sample: %u checks passed\n", checks);
    return 0;
}
