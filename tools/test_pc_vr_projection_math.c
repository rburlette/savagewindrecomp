// Asset-free parameter mapping against the already-pinned Khronos reference.
// No game, OpenXR runtime, headset, guest writes or image submission is used.
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/hle/pc_vr_projection_math.h"
#include "../../.cache/openxr-sdk-source/src/common/xr_linear.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "projection math failed line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void test_fov_mapping(void) {
    const float frusta[][4] = {
        {-0.7f, 0.7f, 0.7f, -0.7f}, {-0.8f, 0.95f, 0.85f, -0.6f},
        {-1.2f, 0.4f, 0.7f, -1.1f}, {-0.35f, 1.15f, 1.1f, -0.3f},
        {0.2f, 0.7f, 0.5f, 0.1f}, {-0.7f, -0.2f, -0.1f, -0.5f},
        {-0.001f, 0.002f, 0.003f, -0.001f}, {-1.4f, 1.35f, 1.3f, -1.4f}};
    const float depths[][2] = {{0.125f, 8000}, {1, 100}, {0.01f, 0.02f}, {3.5f, 100000}};
    for (unsigned i = 0; i < sizeof frusta / sizeof frusta[0]; ++i)
        for (unsigned j = 0; j < sizeof depths / sizeof depths[0]; ++j) {
            float original[9] = {depths[j][0], depths[j][1]}, parameters[9], logical[16];
            CHECK(pc_vr_projection_from_fov(frusta[i], original, parameters));
            CHECK(!memcmp(original, parameters, 2 * sizeof(float)));
            CHECK(pc_vr_projection_logical(parameters, logical));
            const XrFovf fov = {frusta[i][0], frusta[i][1], frusta[i][2], frusta[i][3]};
            XrMatrix4x4f reference;
            XrMatrix4x4f_CreateProjectionFov(&reference, GRAPHICS_OPENGL, fov, original[0], original[1]);
            for (unsigned row = 0; row < 4; ++row) for (unsigned col = 0; col < 4; ++col)
                CHECK(pc_vr_projection_close(logical[row * 4 + col], reference.m[col * 4 + row]));
            // The matrix maps asymmetric boundary rays correctly at different
            // depths. Angle averages would fail these off-center cases.
            for (unsigned depth = 1; depth <= 3; ++depth) {
                const double z = -(double)depth * original[0];
                CHECK(pc_vr_projection_close((logical[0] * (-z) * tan(fov.angleLeft) + logical[2] * z) / (-z), -1));
                CHECK(pc_vr_projection_close((logical[0] * (-z) * tan(fov.angleRight) + logical[2] * z) / (-z), 1));
                CHECK(pc_vr_projection_close((logical[5] * (-z) * tan(fov.angleUp) + logical[6] * z) / (-z), 1));
                CHECK(pc_vr_projection_close((logical[5] * (-z) * tan(fov.angleDown) + logical[6] * z) / (-z), -1));
            }
            CHECK(pc_vr_projection_close((-logical[10] * original[0] + logical[11]) / original[0], -1));
            CHECK(pc_vr_projection_close((-logical[10] * original[1] + logical[11]) / original[1], 1));
            // Original near/far survive aliasing too; old caches are replaced.
            memcpy(original, parameters, sizeof original);
            CHECK(pc_vr_projection_from_fov(frusta[(i + 1) % 8], original, original));
            CHECK(original[0] == depths[j][0] && original[1] == depths[j][1]);
        }
    float original[9] = {0.125f, 8000}, output[9], sentinel[9], fov[4] = {-0.8f, 0.9f, 0.85f, -0.7f};
    memset(sentinel, 0x55, sizeof sentinel);
#define REJECT(change) do { \
    float bad_original[9], bad_fov[4]; memcpy(bad_original, original, sizeof original); memcpy(bad_fov, fov, sizeof fov); \
    memcpy(output, sentinel, sizeof output); change; \
    CHECK(!pc_vr_projection_from_fov(bad_fov, bad_original, output)); \
    CHECK(!memcmp(output, sentinel, sizeof output)); \
} while (0)
    for (unsigned i = 0; i < 4; ++i) {
        REJECT(bad_fov[i] = NAN);
        REJECT(bad_fov[i] = INFINITY);
        REJECT(bad_fov[i] = 1.5707964f);
        REJECT(bad_fov[i] = -1.5707964f);
    }
    REJECT(bad_fov[0] = bad_fov[1]);
    REJECT(bad_fov[0] = bad_fov[1] + 0.1f);
    REJECT(bad_fov[3] = bad_fov[2]);
    REJECT(bad_fov[3] = bad_fov[2] + 0.1f);
    REJECT(bad_original[0] = 0);
    REJECT(bad_original[0] = -1);
    REJECT(bad_original[0] = NAN);
    REJECT(bad_original[1] = INFINITY);
    REJECT(bad_original[1] = bad_original[0]);
    REJECT(bad_original[1] = bad_original[0] * 0.5f);
    REJECT(bad_fov[0] = 0; bad_fov[1] = FLT_TRUE_MIN);
    REJECT(bad_fov[2] = FLT_TRUE_MIN; bad_fov[3] = 0);
#undef REJECT
    CHECK(!pc_vr_projection_from_fov(NULL, original, output));
    CHECK(!pc_vr_projection_from_fov(fov, NULL, output));
    CHECK(!pc_vr_projection_from_fov(fov, original, NULL));
}

static void test_camera_correspondence(void) {
    PcVrHeadLookCamera base = {{10, 2, 4}, {10, 2, 0}, {0, 1, 0}}, eye = base;
    float stored_right[3] = {1, 0, 0};
    PcVrProjectionEyeBasis result;
    eye.position[0] -= 0.04f; eye.target[0] -= 0.04f;
    CHECK(pc_vr_projection_eye_basis(&base, &eye, stored_right, &result));
    CHECK(result.right_matches && result.parallel_eye && result.lateral_eye && result.offset_sign == -1);
    eye = base; eye.position[0] += 0.04f; eye.target[0] += 0.04f;
    CHECK(pc_vr_projection_eye_basis(&base, &eye, stored_right, &result));
    CHECK(result.right_matches && result.parallel_eye && result.lateral_eye && result.offset_sign == 1);
    stored_right[0] = -1;
    CHECK(pc_vr_projection_eye_basis(&base, &eye, stored_right, &result) && !result.right_matches);
    stored_right[0] = 1; eye.target[1] += 0.1f;
    CHECK(pc_vr_projection_eye_basis(&base, &eye, stored_right, &result) && !result.parallel_eye);
    eye = base; eye.position[1] += 0.04f; eye.target[1] += 0.04f;
    CHECK(pc_vr_projection_eye_basis(&base, &eye, stored_right, &result) && !result.lateral_eye && !result.offset_sign);
    eye = base;
    CHECK(pc_vr_projection_eye_basis(&base, &eye, stored_right, &result) && !result.offset_sign);
    eye.up[2] = NAN; CHECK(!pc_vr_projection_eye_basis(&base, &eye, stored_right, &result));
    eye = base; stored_right[1] = INFINITY;
    CHECK(!pc_vr_projection_eye_basis(&base, &eye, stored_right, &result));

    // A rotated center and arbitrary translation must preserve the geometric
    // side correspondence. This checks signs without fixed-IPD assumptions.
    float q[4] = {0, 0, 0, 1}, left[3] = {100, 500, 900}, right[3] = {100.071f, 500, 900};
    CHECK(pc_vr_projection_native_order(q, left, right));
    CHECK(!pc_vr_projection_native_order(q, right, left));
    CHECK(!pc_vr_projection_native_order(q, left, left));
    q[1] = q[3] = sqrtf(0.5f);
    right[0] = left[0]; right[2] = left[2] - 0.055f;
    CHECK(pc_vr_projection_native_order(q, left, right));
    CHECK(!pc_vr_projection_native_order(q, right, left));
    right[1] += 1; CHECK(!pc_vr_projection_native_order(q, left, right));
    right[1] = left[1]; q[3] = 0; CHECK(!pc_vr_projection_native_order(q, left, right));
    q[3] = NAN; CHECK(!pc_vr_projection_native_order(q, left, right));
    q[1] = 0; q[3] = 1; left[0] = NAN;
    CHECK(!pc_vr_projection_native_order(q, left, right));
}

int main(void) {
    test_fov_mapping(); test_camera_correspondence();
    printf("PC VR projection math: %u checks passed (pinned Khronos reference; no guest writes)\n", checks);
    return 0;
}
