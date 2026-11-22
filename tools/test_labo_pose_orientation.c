// Asset-free orientation adapter checks. Compile with clang -std=c11 -O1
// -Wall -Wextra -Werror tools/test_labo_pose_orientation.c -o test_labo_pose_orientation
#include <stdio.h>
#include <stdlib.h>

#include "../runtime/hle/labo_pose_orientation.h"

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "orientation check failed at line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static int near(double actual, double expected) { return fabs(actual - expected) < 0.000001; }

static void check_quaternion(const LaboSensorSample *sample, const double xyzw[4]) {
    const double converted[4] = {-sample->quaternion_30[0], sample->quaternion_30[2],
                                  sample->quaternion_30[1], sample->quaternion_30[3]};
    double norm_squared = 0;
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(isfinite(converted[i]));
        CHECK(near(converted[i], xyzw[i]));
        norm_squared += converted[i] * converted[i];
    }
    CHECK(near(norm_squared, 1));
}

static void check_reset(const LaboPoseOrientation *state) {
    CHECK(!state->referenced && state->generation == 0);
    for (unsigned i = 0; i < 4; ++i) CHECK(state->reference[i] == 0);
}

static void check_invalid(const float input[4]) {
    LaboPoseOrientation state = {.referenced = true, .reference = {0, 0, 0, 1}, .generation = 7};
    LaboSensorSample out, before;
    memset(&out, 0xa5, sizeof out);
    memcpy(&before, &out, sizeof before);
    CHECK(!labo_pose_orientation_sample(&state, input, 7, 2, &out));
    check_reset(&state);
    CHECK(!memcmp(&out, &before, sizeof out));
}

static void check_motion_vector(const LaboSensorSample *sample, double x, double y, double z) {
    // The pinned helper's raw +0x24 mapping is (-raw.x, raw.z, raw.y).
    CHECK(fabs(-sample->vector_24[0] - x) < 0.000001);
    CHECK(fabs( sample->vector_24[2] - y) < 0.000001);
    CHECK(fabs( sample->vector_24[1] - z) < 0.000001);
    for (unsigned i = 0; i < 3; ++i) CHECK(sample->vector_18[i] == 0);
}

int main(void) {
    const float neutral[4] = {0, 0, 0, 1};
    const float yaw20[4] = {0, 0.17364817766693033f, 0, 0.98480775301220806f};
    const float pitch20[4] = {0.17364817766693033f, 0, 0, 0.98480775301220806f};
    const float yaw30[4] = {0, 0.25881904510252074f, 0, 0.9659258262890683f};
    const float yaw50[4] = {0, 0.42261826174069944f, 0, 0.9063077870366499f};
    const double bias[4] = {0, 0.008726535498373935, 0, 0.9999619230641713};
    const double yaw21[4] = {0, 0.18223552549214747, 0, 0.9832549075639546};
    const double pitch_bias[4] = {
        0.17364817766693033 * 0.9999619230641713,
        0.98480775301220806 * 0.008726535498373935,
        0.17364817766693033 * 0.008726535498373935,
        0.98480775301220806 * 0.9999619230641713
    };
    LaboPoseOrientation state = {0};
    struct { uint64_t before; LaboSensorSample sample; uint64_t after; } guarded;
    memset(&guarded, 0xa5, sizeof guarded);
    CHECK(labo_pose_orientation_sample(&state, neutral, 42, 1, &guarded.sample));
    CHECK(state.referenced && state.generation == 42);
    CHECK(guarded.before == UINT64_C(0xa5a5a5a5a5a5a5a5) &&
          guarded.after == UINT64_C(0xa5a5a5a5a5a5a5a5));

    // A positive LOCAL-space rad/s yaw becomes positive helper-body turns/s.
    // The one-degree neutral bias rotates X/Z body components but leaves Y.
    LaboPoseOrientation motion = {0};
    LaboSensorSample motion_sample, orientation_only;
    const float yaw_rate[3] = {0, 6.283185307179586f, 0};
    const float x_rate[3] = {6.283185307179586f, 0, 0};
    const float z_rate[3] = {0, 0, 6.283185307179586f};
    CHECK(labo_pose_orientation_sample_motion(&motion, neutral, yaw_rate, 90, 1, &motion_sample));
    check_quaternion(&motion_sample, bias);
    check_motion_vector(&motion_sample, 0, 1, 0);
    CHECK(labo_pose_orientation_sample_motion(&motion, neutral, x_rate, 90, 2, &motion_sample));
    check_motion_vector(&motion_sample, 0.9998476951563913, 0, 0.01745240643728351);
    CHECK(labo_pose_orientation_sample_motion(&motion, neutral, z_rate, 90, 3, &motion_sample));
    check_motion_vector(&motion_sample, -0.01745240643728351, 0, 0.9998476951563913);
    LaboPoseOrientation orientation_state = {0};
    CHECK(labo_pose_orientation_sample(&orientation_state, neutral, 90, 1, &orientation_only));
    CHECK(!memcmp(motion_sample.quaternion_30, orientation_only.quaternion_30,
                  sizeof orientation_only.quaternion_30));

    // Tilted initial reference, then a nonidentity current pose. Body velocity
    // is inverted through both reference and delivered orientation (including
    // bias). World +Z is the tilted headset's positive yaw axis here.
    const float pitch90[4] = {0.7071067811865475f, 0, 0, 0.7071067811865475f};
    const float tilted_yaw20[4] = {
        0.7071067811865475f * 0.984807753012208f,
        0.7071067811865475f * 0.17364817766693033f,
        0.7071067811865475f * 0.17364817766693033f,
        0.7071067811865475f * 0.984807753012208f
    };
    labo_pose_orientation_reset(&motion);
    CHECK(labo_pose_orientation_sample_motion(&motion, pitch90, z_rate, 91, 1, &motion_sample));
    check_quaternion(&motion_sample, bias);
    check_motion_vector(&motion_sample, 0, 1, 0);
    CHECK(labo_pose_orientation_sample_motion(&motion, tilted_yaw20, z_rate, 91, 2, &motion_sample));
    check_quaternion(&motion_sample, yaw21);
    check_motion_vector(&motion_sample, 0, 1, 0);
    const float negative_z_rate[3] = {0, 0, -6.283185307179586f};
    CHECK(labo_pose_orientation_sample_motion(&motion, tilted_yaw20, negative_z_rate,
                                              91, 3, &motion_sample));
    check_motion_vector(&motion_sample, 0, -1, 0);
    CHECK(labo_pose_orientation_sample_motion(&motion, tilted_yaw20, yaw_rate,
                                              91, 4, &motion_sample));
    check_motion_vector(&motion_sample, 0.3583679495453003, 0, -0.9335804264972017);

    // Missing and nonfinite velocity invalidate calibration without touching
    // output. The next valid sample rebases at its own current orientation.
    LaboSensorSample previous_sample = motion_sample;
    CHECK(!labo_pose_orientation_sample_motion(&motion, tilted_yaw20, NULL, 91, 5, &motion_sample));
    check_reset(&motion);
    CHECK(!memcmp(&motion_sample, &previous_sample, sizeof motion_sample));
    const float invalid_rate[3] = {0, NAN, 0};
    CHECK(!labo_pose_orientation_sample_motion(&motion, tilted_yaw20, invalid_rate,
                                               91, 5, &motion_sample));
    check_reset(&motion);
    CHECK(!memcmp(&motion_sample, &previous_sample, sizeof motion_sample));
    CHECK(labo_pose_orientation_sample_motion(&motion, tilted_yaw20, yaw_rate,
                                              91, 5, &motion_sample));
    check_quaternion(&motion_sample, bias);
    CHECK(guarded.sample.sampling_number == 1);
    check_quaternion(&guarded.sample, bias);
    const unsigned char *bytes = (const unsigned char *)&guarded.sample;
    for (unsigned i = 0; i < 0x30; ++i)
        if (i < 8 || i >= 16) CHECK(bytes[i] == 0); // All opaque/vector bytes initialized.
    CHECK(labo_pose_orientation_sample(&state, yaw20, 42, 2, &guarded.sample));
    check_quaternion(&guarded.sample, yaw21);
    CHECK(labo_pose_orientation_sample(&state, pitch20, 42, 3, &guarded.sample));
    check_quaternion(&guarded.sample, pitch_bias); // Includes multiplication order/sign.
    float negative_pitch[4];
    for (unsigned i = 0; i < 4; ++i) negative_pitch[i] = -pitch20[i];
    CHECK(labo_pose_orientation_sample(&state, negative_pitch, 42, 4, &guarded.sample));
    check_quaternion(&guarded.sample, pitch_bias);

    // A new generation rebases at any absolute pose; only subsequent relative
    // orientation affects the sample, with no accumulated frame-to-frame drift.
    CHECK(labo_pose_orientation_sample(&state, yaw30, 43, 5, &guarded.sample));
    CHECK(state.generation == 43);
    check_quaternion(&guarded.sample, bias);
    CHECK(labo_pose_orientation_sample(&state, yaw50, 43, 6, &guarded.sample));
    check_quaternion(&guarded.sample, yaw21);
    // Relative -1 degree cancels the fixed +1-degree neutral bias. The game
    // rejects an exact identity matrix, so the adapter must escape this point.
    labo_pose_orientation_reset(&state);
    CHECK(labo_pose_orientation_sample(&state, neutral, 44, 7, &guarded.sample));
    const float yaw_minus_one[4] = {0, -0.008726535498373935f, 0,
                                     0.9999619230641713f};
    CHECK(labo_pose_orientation_sample(&state, yaw_minus_one, 44, 8, &guarded.sample));
    CHECK(fabs(guarded.sample.quaternion_30[2]) > 0.00008f);
    CHECK(fabs(guarded.sample.quaternion_30[2]) < 0.00009f);
    CHECK(state.referenced && state.generation == 44);
    CHECK(labo_pose_orientation_sample(&state, yaw20, 44, 9, &guarded.sample));
    check_quaternion(&guarded.sample, yaw21);
    labo_pose_orientation_reset(&state);
    check_reset(&state);
    const float half_turn[4] = {-1, 0, 0, 0};
    CHECK(labo_pose_orientation_sample(&state, half_turn, 0, 10, &guarded.sample));
    const float opposite_sign[4] = {1, 0, 0, 0};
    CHECK(labo_pose_orientation_sample(&state, opposite_sign, 0, 11, &guarded.sample));
    check_quaternion(&guarded.sample, bias); // Sign invariance also when w is exactly zero.

    const float invalid[][4] = {{0, 0, 0, 0}, {NAN, 0, 0, 1}, {0, INFINITY, 0, 1},
                                {0, 0, -INFINITY, 1}, {0, 0, 0, 0.49f},
                                {0, 0, 0, 1.51f}, {FLT_MAX, FLT_MAX, 0, 1}};
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) check_invalid(invalid[i]);
    check_invalid(NULL);
    LaboSensorSample before;
    memcpy(&before, &guarded.sample, sizeof before);
    CHECK(!labo_pose_orientation_sample(NULL, neutral, 1, 1, &guarded.sample));
    CHECK(!memcmp(&before, &guarded.sample, sizeof before));
    CHECK(!labo_pose_orientation_sample(&state, neutral, 1, 0, &guarded.sample));
    check_reset(&state);
    CHECK(!memcmp(&before, &guarded.sample, sizeof before));
    CHECK(!labo_pose_orientation_sample(&state, neutral, 1, 1, NULL));
    check_reset(&state);
    const float norm_low[4] = {0, 0, 0, 0.5f}, norm_high[4] = {0, 0, 0, 1.5f};
    CHECK(labo_pose_orientation_sample(&state, norm_low, 1, 1, &guarded.sample));
    check_quaternion(&guarded.sample, bias);
    CHECK(labo_pose_orientation_sample(&state, norm_high, 1, UINT64_MAX, &guarded.sample));
    check_quaternion(&guarded.sample, bias);
    CHECK(guarded.sample.sampling_number == UINT64_MAX);
    CHECK(guarded.before == UINT64_C(0xa5a5a5a5a5a5a5a5) &&
          guarded.after == UINT64_C(0xa5a5a5a5a5a5a5a5));
    printf("Labo pose orientation: %u checks passed\n", checks);
    return 0;
}
