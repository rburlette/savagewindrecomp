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

// The temporal anti-aliasing offset (DLAA per eye): Halton's points, a frame record's offset as [-0.5, 0.5] pixels in
// NDC, the centre moved so the logical matrix moves the picture by exactly that, and the frustum check that expects it.
static void test_jitter(void) {
    CHECK(pc_vr_projection_halton(1, 2) == 0.5 && pc_vr_projection_halton(2, 2) == 0.25 &&
          pc_vr_projection_halton(3, 2) == 0.75 && pc_vr_projection_halton(0, 2) == 0);
    CHECK(fabs(pc_vr_projection_halton(1, 3) - 1.0 / 3) < 1e-15 && fabs(pc_vr_projection_halton(2, 3) - 2.0 / 3) < 1e-15 &&
          fabs(pc_vr_projection_halton(3, 3) - 1.0 / 9) < 1e-15);
    const unsigned width = 2042, height = 2203;
    float offsets[8][2];
    for (uint64_t serial = 0; serial < 8; ++serial) {
        float *offset = offsets[serial];
        CHECK(pc_vr_projection_jitter(serial + 80, 8, width, height, offset));  // (the phase is the serial's mod 8)
        const double index = (double)(serial + 1);
        CHECK(fabs(offset[0] - (pc_vr_projection_halton((uint64_t)index, 2) - 0.5) * 2.0 / width) < 1e-9);
        CHECK(fabs(offset[1] - (pc_vr_projection_halton((uint64_t)index, 3) - 0.5) * 2.0 / height) < 1e-9);
        CHECK(fabs(offset[0]) * width <= 1.0 + 1e-6 && fabs(offset[1]) * height <= 1.0 + 1e-6);  // (half a pixel)
        for (uint64_t earlier = 0; earlier < serial; ++earlier)
            CHECK(offsets[earlier][0] != offset[0] || offsets[earlier][1] != offset[1]);
    }
    CHECK(offsets[0][0] == 0);  // (phase 1: Halton's base-2 point 0.5, the pixel's centre)
    float untouched[2] = {7, 7};
    CHECK(!pc_vr_projection_jitter(1, 8, 0, height, untouched) && !pc_vr_projection_jitter(1, 8, width, 0, untouched) &&
          !pc_vr_projection_jitter(1, 0, width, height, untouched) && untouched[0] == 7 && untouched[1] == 7);
    const float frusta[][4] = {{-0.7f, 0.7f, 0.7f, -0.7f}, {-0.94f, 0.79f, 0.84f, -0.91f}, {-0.35f, 1.15f, 1.1f, -0.3f}};
    for (unsigned i = 0; i < sizeof frusta / sizeof frusta[0]; ++i)
        for (unsigned phase = 0; phase < 8; ++phase) {
            float original[9] = {0.125f, 25000}, parameters[9], plain[16], moved[16];
            CHECK(pc_vr_projection_from_fov(frusta[i], original, parameters));
            CHECK(pc_vr_projection_logical(parameters, plain));
            pc_vr_projection_offset_centre(parameters, offsets[phase]);
            CHECK(pc_vr_projection_logical(parameters, moved));
            // (only the centre terms change, by minus the offset: the picture moves by the offset)
            CHECK(fabs(moved[2] - (plain[2] - offsets[phase][0])) < 1e-6 && fabs(moved[6] - (plain[6] - offsets[phase][1])) < 1e-6);
            for (unsigned k = 0; k < 16; ++k) if (k != 2 && k != 6) CHECK(moved[k] == plain[k]);
            CHECK(pc_vr_projection_native_rays_offset(frusta[i], offsets[phase], moved));
            if (phase) {  // (an offset of a fraction of a pixel is past the check's 2e-5 without its offset, or with another)
                CHECK(!pc_vr_projection_native_rays(frusta[i], moved));
                const float reversed[2] = {-offsets[phase][0], -offsets[phase][1]};
                CHECK(!pc_vr_projection_native_rays_offset(frusta[i], reversed, moved));
            }
        }
    const float not_a_number[2] = {NAN, 0};
    float logical[16];
    float parameters[9], original[9] = {0.125f, 25000};
    CHECK(pc_vr_projection_from_fov(frusta[0], original, parameters) && pc_vr_projection_logical(parameters, logical));
    CHECK(!pc_vr_projection_native_rays_offset(frusta[0], not_a_number, logical) &&
          !pc_vr_projection_native_rays_offset(frusta[0], NULL, logical));
}

int main(void) {
    test_fov_mapping(); test_camera_correspondence(); test_jitter();
    printf("PC VR projection math: %u checks passed (pinned Khronos reference; no guest writes)\n", checks);
    return 0;
}
