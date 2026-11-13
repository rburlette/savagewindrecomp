// Pure, asset-free third-person head-look math checks.
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/hle/pc_vr_head_look_math.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "head-look check failed line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

static bool near(double a, double b) { return fabs(a - b) < 0.00001; }

static double length_between(const float a[3], const float b[3]) {
    double sum = 0;
    for (unsigned i = 0; i < 3; ++i) {
        double d = (double)a[i] - b[i];
        sum += d * d;
    }
    return sqrt(sum);
}

static double length(const float a[3]) {
    const float zero[3] = {0, 0, 0};
    return length_between(a, zero);
}

static void check_camera(const PcVrHeadLookCamera *base,
                         const PcVrHeadLookCamera *actual,
                         const double target[3], const double up[3]) {
    CHECK(!memcmp(base->position, actual->position, sizeof base->position));
    CHECK(near(length_between(base->position, base->target),
               length_between(actual->position, actual->target)));
    CHECK(near(length(base->up), length(actual->up)));
    double orthogonal = 0;
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(near(actual->target[i], target[i]));
        CHECK(near(actual->up[i], up[i]));
        orthogonal += ((double)actual->target[i] - actual->position[i]) * actual->up[i];
    }
    CHECK(fabs(orthogonal) < 0.0001);
}

static void check_invalid(const PcVrHeadLookCamera *base, const float q[4]) {
    PcVrHeadLookCamera out, before;
    memset(&out, 0xa5, sizeof out);
    before = out;
    CHECK(!pc_vr_head_look_apply(base, q, &out));
    CHECK(!memcmp(&out, &before, sizeof out));
}

static void check_eye_invariants(const PcVrHeadLookCamera *center,
                                 const PcVrHeadLookCamera *eye,
                                 const PcVrHeadLookCamera *actual) {
    CHECK(near(length_between(center->position, eye->position),
               length_between(center->position, actual->position)));
    CHECK(near(length_between(eye->position, eye->target),
               length_between(actual->position, actual->target)));
    CHECK(near(length(eye->up), length(actual->up)));
}

int main(void) {
    const double half = 0.7071067811865475244;
    const float neutral[4] = {0, 0, 0, 1};
    const float negative_neutral[4] = {0, 0, 0, -1};
    const float yaw90[4] = {0, (float)half, 0, (float)half};
    const float negative_yaw90[4] = {0, (float)-half, 0, (float)-half};
    const float yaw_minus90[4] = {0, (float)-half, 0, (float)half};
    const float pitch90[4] = {(float)half, 0, 0, (float)half};
    const float roll90[4] = {0, 0, (float)half, (float)half};
    const PcVrHeadLookCamera base = {
        .position = {1, 2, 3}, .target = {1, 2, -7}, .up = {0, 1, 0}
    };
    PcVrHeadLookCamera out, previous;

    CHECK(pc_vr_head_look_apply(&base, neutral, &out));
    check_camera(&base, &out, (double[]){1, 2, -7}, (double[]){0, 1, 0});
    CHECK(!memcmp(&base, &out, sizeof base));
    CHECK(pc_vr_head_look_apply(&base, negative_neutral, &out));
    CHECK(!memcmp(&base, &out, sizeof base));
    CHECK(pc_vr_head_look_apply(&base, yaw90, &out));
    check_camera(&base, &out, (double[]){-9, 2, 3}, (double[]){0, 1, 0});
    previous = out;
    CHECK(pc_vr_head_look_apply(&base, negative_yaw90, &out));
    CHECK(!memcmp(&out, &previous, sizeof out)); // q and -q are one rotation.
    CHECK(pc_vr_head_look_apply(&base, yaw_minus90, &out));
    check_camera(&base, &out, (double[]){11, 2, 3}, (double[]){0, 1, 0});
    CHECK(pc_vr_head_look_apply(&base, pitch90, &out));
    check_camera(&base, &out, (double[]){1, 12, 3}, (double[]){0, 0, 1});
    CHECK(pc_vr_head_look_apply(&base, roll90, &out));
    check_camera(&base, &out, (double[]){1, 2, -7}, (double[]){-1, 0, 0});

    // A noncanonical controller basis: +X forward and +Z up. Local +Y yaw
    // follows that basis instead of a fixed world yaw axis.
    const PcVrHeadLookCamera sideways = {
        .position = {3, -4, 5}, .target = {8, -4, 5}, .up = {0, 0, 2}
    };
    CHECK(pc_vr_head_look_apply(&sideways, yaw90, &out));
    check_camera(&sideways, &out, (double[]){3, 1, 5}, (double[]){0, 0, 2});
    CHECK(pc_vr_head_look_apply(&sideways, neutral, &out));
    check_camera(&sideways, &out, (double[]){8, -4, 5}, (double[]){0, 0, 2});

    // A controller pan changes the base every time. Applying the same head
    // rotation uses the new base and never modifies its position.
    const PcVrHeadLookCamera moved = {
        .position = {100, 20, 40}, .target = {100, 20, 30}, .up = {0, 1, 0}
    };
    CHECK(pc_vr_head_look_apply(&moved, yaw90, &out));
    check_camera(&moved, &out, (double[]){90, 20, 40}, (double[]){0, 1, 0});
    previous = out;
    for (unsigned i = 0; i < 100; ++i) {
        CHECK(pc_vr_head_look_apply(&moved, yaw90, &out));
        CHECK(!memcmp(&out, &previous, sizeof out));
    }
    PcVrHeadLookCamera in_place = moved;
    CHECK(pc_vr_head_look_apply(&in_place, yaw90, &in_place));
    CHECK(!memcmp(&in_place, &previous, sizeof out));

    // Normalize harmless quaternion scale, but reject missing tracking data
    // and degenerate look-at bases without altering output.
    const float scaled_yaw[4] = {0, (float)(half * 1.4), 0, (float)(half * 1.4)};
    CHECK(pc_vr_head_look_apply(&base, scaled_yaw, &out));
    check_camera(&base, &out, (double[]){-9, 2, 3}, (double[]){0, 1, 0});
    const float bad_q[][4] = {
        {0, 0, 0, 0}, {NAN, 0, 0, 1}, {0, INFINITY, 0, 1},
        {0, 0, 0, 0.49f}, {0, 0, 0, 1.51f}
    };
    for (unsigned i = 0; i < sizeof bad_q / sizeof bad_q[0]; ++i)
        check_invalid(&base, bad_q[i]);
    check_invalid(&base, NULL);
    PcVrHeadLookCamera invalid = base;
    invalid.target[0] = invalid.position[0];
    invalid.target[1] = invalid.position[1];
    invalid.target[2] = invalid.position[2];
    check_invalid(&invalid, neutral);
    invalid = base; invalid.up[0] = invalid.up[1] = invalid.up[2] = 0;
    check_invalid(&invalid, neutral);
    invalid = base; invalid.up[0] = 0; invalid.up[1] = 0; invalid.up[2] = -1;
    check_invalid(&invalid, neutral); // Up parallel to forward.
    invalid = base; invalid.position[0] = NAN;
    check_invalid(&invalid, neutral);
    invalid = base; invalid.target[1] = INFINITY;
    check_invalid(&invalid, neutral);
    invalid = base; invalid.up[2] = NAN;
    check_invalid(&invalid, neutral);
    invalid = base; invalid.position[0] = FLT_MAX; invalid.target[0] = -FLT_MAX;
    check_invalid(&invalid, yaw90); // Rotated float output would overflow.
    CHECK(!pc_vr_head_look_apply(NULL, neutral, &out));
    CHECK(!pc_vr_head_look_apply(&base, neutral, NULL));

    // A render-owned stereo eye is rotated about the fixed controller center.
    // Its own position changes, but the original eye offset and look radius
    // retain their lengths. The opposite eye keeps its original separation.
    const PcVrHeadLookCamera center = {
        .position = {0, 0, 0}, .target = {0, 0, -10}, .up = {0, 1, 0}
    };
    const PcVrHeadLookCamera left = {
        .position = {-0.03f, 0, 0}, .target = {-0.03f, 0, -10}, .up = {0, 1, 0}
    };
    const PcVrHeadLookCamera right = {
        .position = {0.03f, 0, 0}, .target = {0.03f, 0, -10}, .up = {0, 1, 0}
    };
    PcVrHeadLookCamera left_out, right_out;
    PcVrHeadLookCamera center_before = center;
    CHECK(pc_vr_head_look_apply_eye(&center, &left, neutral, &left_out));
    CHECK(!memcmp(&left, &left_out, sizeof left));
    CHECK(pc_vr_head_look_apply_eye(&center, &left, yaw90, &left_out));
    check_eye_invariants(&center, &left, &left_out);
    CHECK(near(left_out.position[0], 0) && near(left_out.position[1], 0) &&
          near(left_out.position[2], 0.03));
    CHECK(near(left_out.target[0], -10) && near(left_out.target[1], 0) &&
          near(left_out.target[2], 0.03));
    CHECK(pc_vr_head_look_apply_eye(&center, &right, yaw90, &right_out));
    check_eye_invariants(&center, &right, &right_out);
    CHECK(near(right_out.position[2], -0.03));
    CHECK(near(length_between(left.position, right.position),
               length_between(left_out.position, right_out.position)));
    CHECK(!memcmp(&center, &center_before, sizeof center));
    previous = left_out;
    CHECK(pc_vr_head_look_apply_eye(&center, &left, negative_yaw90, &left_out));
    CHECK(!memcmp(&left_out, &previous, sizeof left_out));
    for (unsigned i = 0; i < 100; ++i) {
        CHECK(pc_vr_head_look_apply_eye(&center, &left, yaw90, &left_out));
        CHECK(!memcmp(&left_out, &previous, sizeof left_out));
    }
    PcVrHeadLookCamera in_place_eye = left;
    CHECK(pc_vr_head_look_apply_eye(&center, &in_place_eye, yaw90, &in_place_eye));
    CHECK(!memcmp(&in_place_eye, &previous, sizeof previous));
    CHECK(pc_vr_head_look_apply_eye(&center, &left, pitch90, &left_out));
    CHECK(near(left_out.position[0], -0.03) && near(left_out.target[1], 10));
    CHECK(near(left_out.up[2], 1));
    CHECK(pc_vr_head_look_apply_eye(&center, &left, roll90, &left_out));
    CHECK(near(left_out.position[1], -0.03) && near(left_out.target[2], -10));
    CHECK(near(left_out.up[0], -1));

    const PcVrHeadLookCamera tilted_eye = {
        .position = {3, -4.04f, 5}, .target = {8, -4.04f, 5}, .up = {0, 0, 2}
    };
    CHECK(pc_vr_head_look_apply_eye(&sideways, &tilted_eye, yaw90, &left_out));
    check_eye_invariants(&sideways, &tilted_eye, &left_out);
    CHECK(near(left_out.position[0], 3.04) && near(left_out.position[1], -4));
    PcVrHeadLookCamera poison;
    memset(&poison, 0xa5, sizeof poison);
    PcVrHeadLookCamera poison_before = poison;
    CHECK(!pc_vr_head_look_apply_eye(&invalid, &left, yaw90, &poison));
    CHECK(!memcmp(&poison, &poison_before, sizeof poison));
    CHECK(!pc_vr_head_look_apply_eye(&center, &left, bad_q[0], &poison));
    CHECK(!memcmp(&poison, &poison_before, sizeof poison));
    PcVrHeadLookCamera bad_eye = left;
    bad_eye.position[0] = NAN;
    CHECK(!pc_vr_head_look_apply_eye(&center, &bad_eye, yaw90, &poison));
    CHECK(!memcmp(&poison, &poison_before, sizeof poison));
    CHECK(!pc_vr_head_look_apply_eye(&center, &left, yaw90,
                                     (PcVrHeadLookCamera *)&center));
    CHECK(!memcmp(&center, &center_before, sizeof center));
    CHECK(!pc_vr_head_look_apply_eye(NULL, &left, yaw90, &poison));
    CHECK(!pc_vr_head_look_apply_eye(&center, NULL, yaw90, &poison));
    CHECK(!pc_vr_head_look_apply_eye(&center, &left, NULL, &poison));
    CHECK(!pc_vr_head_look_apply_eye(&center, &left, yaw90, NULL));
    printf("PC VR head-look math: %u checks passed\n", checks);
    return 0;
}
