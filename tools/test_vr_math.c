// Asset-free check of the PC VR renderer's geometry (runtime/nvn/vulkan/vr_math.h). vr_rotate turns vectors as
// OpenXR's unit quaternions (x, y, z, w) do in its right-handed spaces (x right, y up, -z ahead). Covers the identity,
// quarter turns about each axis (the right-hand rule: a quarter turn about +y takes ahead to the left), a half turn,
// the conjugate undoing a rotation (how exec.c brings a LOCAL point into an eye's view), two rotations in sequence
// matching their product, and length kept by an arbitrary rotation. vr_aim_ray_point_from_eye and vr_eye_image_position
// (where an eye sees a point): the image's centre, a point up and to the right, outside the FOV, behind the eye and
// within the margin, a turned eye, an asymmetric FOV; and on 200,000 random poses, FOVs and points, the same bits as
// the three copies they replaced (the crosshair's depth sample, the world-tied HUD marker, the rune aim point), written
// as they were. vr_letterbox (the window mirror's placement of the left eye) is checked at the scale of 1, a wider and
// a taller window, a larger final image, a tiny window and the cases with nothing to copy. vr_aim_ray_hit (where the
// aim ray meets the scene): a hit between two points, at the first and at the last, misses, stored depths out of range
// or not numbers, a z behind the eye, terms with no z, both depth modes. vr_eye_reprojection (the eye's camera between
// two frames, for the object motion vectors): still points in view near and far reprojected to where the previous
// frame drew them for an eye that moved and turned far from the origin with another sub-pixel offset, the offsets'
// difference, an unmoved eye, a projection with no inverse. No renderer, GPU or headset is involved.
#include "../runtime/nvn/vulkan/vr_math.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; if (failures < 10) printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static const double tolerance = 1e-12;

// Whether two vectors agree to within rounding.
static bool vectors_match(const double actual[3], double expected_x, double expected_y, double expected_z) {
    return fabs(actual[0] - expected_x) < tolerance && fabs(actual[1] - expected_y) < tolerance &&
        fabs(actual[2] - expected_z) < tolerance;
}

// The unit quaternion (x, y, z, w) of a turn by angle_radians about the unit axis (axis_x, axis_y, axis_z).
static void quaternion_about_axis(double axis_x, double axis_y, double axis_z, double angle_radians, double out[4]) {
    const double half_sine = sin(angle_radians / 2);
    out[0] = axis_x * half_sine;
    out[1] = axis_y * half_sine;
    out[2] = axis_z * half_sine;
    out[3] = cos(angle_radians / 2);
}

// The Hamilton product first_then_second = second * first: rotating by it is rotating by first, then by second.
static void quaternion_sequence(const double first[4], const double second[4], double out[4]) {
    out[0] = second[3] * first[0] + second[0] * first[3] + second[1] * first[2] - second[2] * first[1];
    out[1] = second[3] * first[1] - second[0] * first[2] + second[1] * first[3] + second[2] * first[0];
    out[2] = second[3] * first[2] + second[0] * first[1] - second[1] * first[0] + second[2] * first[3];
    out[3] = second[3] * first[3] - second[0] * first[0] - second[1] * first[1] - second[2] * first[2];
}

static double vector_length(const double vector[3]) {
    return sqrt(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
}

// ---- The three places that turned a point into its place in an eye's image before vr_eye_image_position, written as
// they were (exec.c at bb8d783), beside the same work done through vr_math.h. The shared functions must give the same
// bits: the crosshair's depth sample (pixels of the depth image), the world-tied HUD marker (into the eye's half of
// the final image) and the rune target's aim point (0 to 1 across the eye's image).
typedef struct {
    float origin[3], eye_position[3], eye_orientation[4], fov[4];  // as the pose snapshot holds them
    double heading[4];        // the calibrated heading
    double along, units_per_meter;
    double marker_point[3];   // a marker's point from the calibration origin, metres, before the heading turns it
    double viewport[4];       // the eye's half of the final image (x, y, width, height)
    uint32_t image_width, image_height;  // the depth image
} EyeCase;

static bool crosshair_pixel_before(const EyeCase *eye_case, double pixel[2], double eye_depth[1]) {
    const double to_eye[4] = {-eye_case->eye_orientation[0], -eye_case->eye_orientation[1],
                              -eye_case->eye_orientation[2], eye_case->eye_orientation[3]};
    const double ahead_vector[3] = {0, 0, -1};
    double direction[3];
    vr_rotate(eye_case->heading, ahead_vector, direction);
    const double left = tan(eye_case->fov[0]), right = tan(eye_case->fov[1]);
    const double up = tan(eye_case->fov[2]), down = tan(eye_case->fov[3]);
    double from_eye[3], view[3];
    for (unsigned j = 0; j < 3; ++j)
        from_eye[j] = eye_case->origin[j] + direction[j] * eye_case->along / eye_case->units_per_meter -
                      eye_case->eye_position[j];
    vr_rotate(to_eye, from_eye, view);
    if (!(view[2] < -1e-6)) return false;
    const double tan_x = view[0] / -view[2], tan_y = view[1] / -view[2];
    pixel[0] = (tan_x - left) / (right - left) * eye_case->image_width;
    pixel[1] = (up - tan_y) / (up - down) * eye_case->image_height;
    eye_depth[0] = -view[2] * eye_case->units_per_meter;
    return true;
}

static bool crosshair_pixel_now(const EyeCase *eye_case, double pixel[2], double eye_depth[1]) {
    const double ahead_vector[3] = {0, 0, -1};
    double direction[3];
    vr_rotate(eye_case->heading, ahead_vector, direction);
    const double fov_tangents[4] = {tan(eye_case->fov[0]), tan(eye_case->fov[1]), tan(eye_case->fov[2]),
                                    tan(eye_case->fov[3])};
    double from_eye[3], view[3], image[2];
    vr_aim_ray_point_from_eye(eye_case->origin, direction, eye_case->along, eye_case->units_per_meter,
                              eye_case->eye_position, from_eye);
    if (!vr_eye_image_position(from_eye, eye_case->eye_orientation, fov_tangents, 1e-6, view, image)) return false;
    pixel[0] = image[0] * eye_case->image_width;
    pixel[1] = image[1] * eye_case->image_height;
    eye_depth[0] = -view[2] * eye_case->units_per_meter;
    return true;
}

// (view: the point in the eye's view, which exec.c goes on to use for the world's up at the marker)
static bool marker_place_before(const EyeCase *eye_case, double eye_at[2], double view[3]) {
    const double to_eye[4] = {-eye_case->eye_orientation[0], -eye_case->eye_orientation[1],
                              -eye_case->eye_orientation[2], eye_case->eye_orientation[3]};
    double local[3], from_eye[3];
    vr_rotate(eye_case->heading, eye_case->marker_point, local);
    for (unsigned i = 0; i < 3; ++i) from_eye[i] = eye_case->origin[i] + local[i] - eye_case->eye_position[i];
    vr_rotate(to_eye, from_eye, view);
    if (!(view[2] < -1e-4)) return false;
    const double tan_x = view[0] / -view[2], tan_y = view[1] / -view[2];
    const double left = tan(eye_case->fov[0]), right = tan(eye_case->fov[1]);
    const double up = tan(eye_case->fov[2]), down = tan(eye_case->fov[3]);
    const double *viewport = eye_case->viewport;
    eye_at[0] = viewport[0] + (tan_x - left) / (right - left) * viewport[2];
    eye_at[1] = viewport[1] + (up - tan_y) / (up - down) * viewport[3];
    return true;
}

static bool marker_place_now(const EyeCase *eye_case, double eye_at[2], double view[3]) {
    const double left = tan(eye_case->fov[0]), right = tan(eye_case->fov[1]);
    const double up = tan(eye_case->fov[2]), down = tan(eye_case->fov[3]);
    double local[3], from_eye[3], image[2];
    vr_rotate(eye_case->heading, eye_case->marker_point, local);
    for (unsigned i = 0; i < 3; ++i) from_eye[i] = eye_case->origin[i] + local[i] - eye_case->eye_position[i];
    if (!vr_eye_image_position(from_eye, eye_case->eye_orientation, (const double[4]){left, right, up, down}, 1e-4,
                               view, image))
        return false;
    const double *viewport = eye_case->viewport;
    eye_at[0] = viewport[0] + image[0] * viewport[2];
    eye_at[1] = viewport[1] + image[1] * viewport[3];
    return true;
}

static bool rune_point_before(const EyeCase *eye_case, double image[2]) {
    const double to_eye[4] = {-eye_case->eye_orientation[0], -eye_case->eye_orientation[1],
                              -eye_case->eye_orientation[2], eye_case->eye_orientation[3]};
    const double ahead_vector[3] = {0, 0, -1};
    double direction[3], from_eye[3], view[3];
    vr_rotate(eye_case->heading, ahead_vector, direction);
    for (unsigned j = 0; j < 3; ++j)
        from_eye[j] = eye_case->origin[j] + direction[j] * eye_case->along / eye_case->units_per_meter -
                      eye_case->eye_position[j];
    vr_rotate(to_eye, from_eye, view);
    if (!(view[2] < -1e-6)) return false;
    const double left = tan(eye_case->fov[0]), right = tan(eye_case->fov[1]);
    const double up = tan(eye_case->fov[2]), down = tan(eye_case->fov[3]);
    image[0] = (view[0] / -view[2] - left) / (right - left);
    image[1] = (up - view[1] / -view[2]) / (up - down);
    return true;
}

static bool rune_point_now(const EyeCase *eye_case, double image[2]) {
    const double ahead_vector[3] = {0, 0, -1};
    double direction[3], from_eye[3], view[3];
    vr_rotate(eye_case->heading, ahead_vector, direction);
    vr_aim_ray_point_from_eye(eye_case->origin, direction, eye_case->along, eye_case->units_per_meter,
                              eye_case->eye_position, from_eye);
    const double left = tan(eye_case->fov[0]), right = tan(eye_case->fov[1]);
    const double up = tan(eye_case->fov[2]), down = tan(eye_case->fov[3]);
    return vr_eye_image_position(from_eye, eye_case->eye_orientation, (const double[4]){left, right, up, down}, 1e-6,
                                 view, image);
}

// A deterministic pseudo-random sequence (xorshift64), and a number in [low, high) from it.
static uint64_t random_state = 0x2545f4914f6cdd1dull;
static double random_between(double low, double high) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return low + (high - low) * (double)(random_state >> 11) / 9007199254740992.0;
}

// A random unit quaternion: a random axis and angle.
static void random_orientation(double out[4]) {
    double axis[3] = {random_between(-1, 1), random_between(-1, 1), random_between(-1, 1)};
    const double length = vector_length(axis);
    quaternion_about_axis(axis[0] / length, axis[1] / length, axis[2] / length, random_between(-3.1, 3.1), out);
}

static void random_eye_case(EyeCase *eye_case) {
    double orientation[4];
    random_orientation(orientation);
    for (unsigned i = 0; i < 4; ++i) eye_case->eye_orientation[i] = (float)orientation[i];
    // (the calibrated heading: a yaw about +y only, as the calibration takes it)
    quaternion_about_axis(0, 1, 0, random_between(-3.1, 3.1), eye_case->heading);
    for (unsigned i = 0; i < 3; ++i) {
        eye_case->origin[i] = (float)random_between(-0.5, 0.5) + (i == 1 ? 1.6f : 0.0f);
        eye_case->eye_position[i] = eye_case->origin[i] + (float)random_between(-0.2, 0.2);
        eye_case->marker_point[i] = random_between(-20, 20);
    }
    eye_case->fov[0] = (float)random_between(-1.0, -0.6);
    eye_case->fov[1] = (float)random_between(0.6, 1.0);
    eye_case->fov[2] = (float)random_between(0.6, 1.0);
    eye_case->fov[3] = (float)random_between(-1.0, -0.6);
    eye_case->along = random_between(0.25, 4000);
    eye_case->units_per_meter = random_between(5, 15);
    const double viewport[4] = {random_between(0, 1000), 0, random_between(800, 2000), random_between(900, 2200)};
    for (unsigned i = 0; i < 4; ++i) eye_case->viewport[i] = viewport[i];
    eye_case->image_width = (uint32_t)random_between(800, 4000);
    eye_case->image_height = (uint32_t)random_between(900, 4500);
}

// Whether two results are the same bits (a NaN or a negative zero included).
static bool same_bits(const double *first, const double *second, unsigned count) {
    return !memcmp(first, second, count * sizeof first[0]);
}

// One sampling of the aim ray, as vr_aim.c keeps it (the fields its read uses).
#define AIM_POINTS 56
typedef struct {
    unsigned steps;
    double ray_distance[AIM_POINTS], eye_depth[AIM_POINTS];
    float depth_terms[4];
    bool gl_depth_range;
    float stored_depth[AIM_POINTS];  // (what the GPU copied: the read's `depth`)
} AimSample;

static double aim_hit_now(const AimSample *sample, unsigned *hit_step) {
    return vr_aim_ray_hit(sample->stored_depth, sample->ray_distance, sample->eye_depth, sample->steps,
                          sample->depth_terms, sample->gl_depth_range, hit_step);
}

// A hand-made sampling: points along the ray straight ahead (each point's depth its distance), the stored depths, the
// depth terms and the mode.
static AimSample aim_sample(unsigned steps, const double *along, const float *stored, const float terms[4],
                            bool gl_depth_range) {
    AimSample sample = {.steps = steps, .gl_depth_range = gl_depth_range};
    for (unsigned i = 0; i < steps; ++i) {
        sample.ray_distance[i] = along[i];
        sample.eye_depth[i] = along[i];
        sample.stored_depth[i] = stored[i];
    }
    memcpy(sample.depth_terms, terms, sizeof sample.depth_terms);
    return sample;
}

// DLAA per eye (vr_dlaa_camera, vr_dlaa_cut, vr_dlaa_jitter_pixels): the camera from a certificate's view and device
// projection with the offset taken out, at the origin and at a place in the world with the eye turned; its entries
// rounded to float; the eye's position and direction; cuts by distance and by turn; the offset in pixels for either
// window origin.
static void test_dlaa_camera(void) {
    // An off-axis projection (left, right, up, down tangents), with the depth row the device conversion gives.
    const double left = -1.3, right = 1.0, up = 1.1, down = -1.2, near_plane = 0.25, far_plane = 25000;
    double plain[16] = {0};
    plain[0] = 2 / (right - left); plain[2] = (right + left) / (right - left);
    plain[5] = 2 / (up - down); plain[6] = (up + down) / (up - down);
    plain[10] = -far_plane / (far_plane - near_plane); plain[11] = -far_plane * near_plane / (far_plane - near_plane);
    plain[14] = -1;
    const float offset[2] = {0.25f * 2 / 2042, -0.1f * 2 / 2203};  // (a quarter pixel right, a tenth down in NDC)
    float device[16];
    for (unsigned i = 0; i < 16; ++i) device[i] = (float)plain[i];
    device[2] = (float)(plain[2] - offset[0]);
    device[6] = (float)(plain[6] - offset[1]);
    // At the origin, unturned: the camera is the projection less its offset.
    const float identity_view[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    VrDlaaCamera camera;
    CHECK(vr_dlaa_camera(identity_view, device, offset, &camera));
    for (unsigned i = 0; i < 16; ++i) {
        CHECK(fabs(camera.view_projection[i] - plain[i]) < 1e-6 * fmax(1, fabs(plain[i])));
        CHECK(camera.view_projection[i] == (double)(float)camera.view_projection[i]);
    }
    CHECK(vectors_match(camera.position, 0, 0, 0) && vectors_match(camera.forward, 0, 0, -1));
    // At a place in the world, turned (yaw 0.7, pitch 0.2): the rows of the view are its right, up and back axes.
    const double yaw = 0.7, pitch = 0.2, place[3] = {-672.5, 171.25, 1816};
    const double axes[3][3] = {{cos(yaw), 0, -sin(yaw)},
                               {sin(yaw) * sin(pitch), cos(pitch), cos(yaw) * sin(pitch)},
                               {sin(yaw) * cos(pitch), -sin(pitch), cos(yaw) * cos(pitch)}};
    float view[12];
    for (unsigned row = 0; row < 3; ++row) {
        double translation = 0;
        for (unsigned k = 0; k < 3; ++k) view[row * 4 + k] = (float)axes[row][k], translation -= axes[row][k] * place[k];
        view[row * 4 + 3] = (float)translation;
    }
    CHECK(vr_dlaa_camera(view, device, offset, &camera));
    CHECK(fabs(camera.position[0] - place[0]) < 1e-3 && fabs(camera.position[1] - place[1]) < 1e-3 &&
          fabs(camera.position[2] - place[2]) < 1e-3);
    CHECK(fabs(camera.forward[0] + axes[2][0]) < 1e-6 && fabs(camera.forward[1] + axes[2][1]) < 1e-6 &&
          fabs(camera.forward[2] + axes[2][2]) < 1e-6);
    // A world point through the camera lands where the unmoved projection puts the point in the eye's view.
    const double point[3] = {-660.0, 175.0, 1800.0};
    double in_view[4] = {0, 0, 0, 1}, expected[4] = {0}, actual[4] = {0};
    for (unsigned row = 0; row < 3; ++row) {
        in_view[row] = view[row * 4 + 3];
        for (unsigned k = 0; k < 3; ++k) in_view[row] += (double)view[row * 4 + k] * point[k];
    }
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned k = 0; k < 4; ++k) {
            expected[row] += plain[row * 4 + k] * in_view[k];
            actual[row] += camera.view_projection[row * 4 + k] * (k < 3 ? point[k] : 1);
        }
    CHECK(expected[3] > 0 && fabs(actual[0] / actual[3] - expected[0] / expected[3]) < 1e-4 &&
          fabs(actual[1] / actual[3] - expected[1] / expected[3]) < 1e-4 &&
          fabs(actual[2] / actual[3] - expected[2] / expected[3]) < 1e-4);
    float broken[12];
    memcpy(broken, view, sizeof broken);
    broken[5] = NAN;
    CHECK(!vr_dlaa_camera(broken, device, offset, &camera));
    const float no_number[2] = {INFINITY, 0};
    CHECK(!vr_dlaa_camera(view, device, no_number, &camera));
    // Cuts: over 10 game units, or a turn past about 35 degrees.
    VrDlaaCamera before = {0}, after = {0};
    before.forward[2] = after.forward[2] = -1;
    CHECK(!vr_dlaa_cut(&after, &before));
    after.position[0] = 9.9;
    CHECK(!vr_dlaa_cut(&after, &before));
    after.position[0] = 10.1;
    CHECK(vr_dlaa_cut(&after, &before));
    after.position[0] = 0;
    const double degrees = acos(-1.0) / 180;
    after.forward[0] = -sin(30 * degrees), after.forward[2] = -cos(30 * degrees);
    CHECK(!vr_dlaa_cut(&after, &before));
    after.forward[0] = -sin(40 * degrees), after.forward[2] = -cos(40 * degrees);
    CHECK(vr_dlaa_cut(&after, &before));
    after.forward[0] = 0, after.forward[2] = -1, after.position[1] = NAN;
    CHECK(vr_dlaa_cut(&after, &before));
    // The offset in pixels: a quarter pixel right; a tenth of a pixel along the rows, its sign by the window's origin.
    float pixels[2];
    vr_dlaa_jitter_pixels(offset, 2042, 2203, 0, pixels);
    CHECK(fabsf(pixels[0] - 0.25f) < 1e-5f && fabsf(pixels[1] + 0.1f) < 1e-5f);
    vr_dlaa_jitter_pixels(offset, 2042, 2203, 1, pixels);
    CHECK(fabsf(pixels[0] - 0.25f) < 1e-5f && fabsf(pixels[1] - 0.1f) < 1e-5f);
}

// A world point through a view (3x4) and a projection (4x4, both row-major) to NDC (x, y, z); false: behind the eye.
static bool project_to_ndc(const float view[12], const float projection[16], const double point[3], double ndc[3]) {
    double in_view[4] = {0, 0, 0, 1}, clip[4] = {0};
    for (unsigned row = 0; row < 3; ++row) {
        in_view[row] = view[row * 4 + 3];
        for (unsigned k = 0; k < 3; ++k) in_view[row] += (double)view[row * 4 + k] * point[k];
    }
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned k = 0; k < 4; ++k) clip[row] += (double)projection[row * 4 + k] * in_view[k];
    if (!(clip[3] > 0)) return false;
    for (unsigned axis = 0; axis < 3; ++axis) ndc[axis] = clip[axis] / clip[3];
    return true;
}

// A view (3x4, row-major) at place, turned by yaw and pitch: its rows are the right, up and back axes in the world.
static void turned_view(double yaw, double pitch, const double place[3], float view[12]) {
    const double axes[3][3] = {{cos(yaw), 0, -sin(yaw)},
                               {sin(yaw) * sin(pitch), cos(pitch), cos(yaw) * sin(pitch)},
                               {sin(yaw) * cos(pitch), -sin(pitch), cos(yaw) * cos(pitch)}};
    for (unsigned row = 0; row < 3; ++row) {
        double translation = 0;
        for (unsigned k = 0; k < 3; ++k) view[row * 4 + k] = (float)axes[row][k], translation -= axes[row][k] * place[k];
        view[row * 4 + 3] = (float)translation;
    }
}

// Object motion vectors per eye (vr_eye_reprojection, vr_invert4): the reprojection takes this frame's NDC of a still
// point to where the previous frame drew it, for an eye that moved and turned between two frames in the world far from
// the origin, with a different sub-pixel offset in each frame's projection; the offsets' difference as the picture
// moved; an unmoved eye with the same offset is the identity; a projection with no inverse is refused.
static void test_eye_reprojection(void) {
    const double left = -1.3, right = 1.0, up = 1.1, down = -1.2, near_plane = 0.25, far_plane = 25000;
    float projection_now[16] = {0}, projection_previous[16];
    projection_now[0] = (float)(2 / (right - left)); projection_now[2] = (float)((right + left) / (right - left));
    projection_now[5] = (float)(2 / (up - down)); projection_now[6] = (float)((up + down) / (up - down));
    projection_now[10] = (float)(-far_plane / (far_plane - near_plane));
    projection_now[11] = (float)(-far_plane * near_plane / (far_plane - near_plane));
    projection_now[14] = -1;
    memcpy(projection_previous, projection_now, sizeof projection_previous);
    projection_now[2] -= 0.25f * 2 / 1634, projection_now[6] -= -0.1f * 2 / 1763;  // (the offsets: the picture's move)
    projection_previous[2] -= -0.375f * 2 / 1634, projection_previous[6] -= 0.3f * 2 / 1763;
    const double place_now[3] = {-672.5, 171.25, 1816}, place_previous[3] = {-672.9, 171.20, 1816.3};
    float view_now[12], view_previous[12];
    turned_view(0.7, 0.2, place_now, view_now);
    turned_view(0.72, 0.19, place_previous, view_previous);
    float reprojection[16], jitter[2];
    CHECK(vr_eye_reprojection(view_now, projection_now, view_previous, projection_previous, reprojection, jitter));
    CHECK(fabsf(jitter[0] - (0.25f + 0.375f) * 2 / 1634) < 1e-7f && fabsf(jitter[1] - (-0.1f - 0.3f) * 2 / 1763) < 1e-7f);
    // Still points in view, near (5 units ahead), middling and far: this frame's NDC through the reprojection is the
    // previous frame's.
    const double points[3][3] = {{-675.6, 172.2, 1812.3}, {-690.0, 168.0, 1790.0}, {-2500.0, 400.0, -900.0}};
    for (unsigned p = 0; p < 3; ++p) {
        double now[3], previous[3], reprojected[4] = {0};
        CHECK(project_to_ndc(view_now, projection_now, points[p], now));
        CHECK(fabs(now[0]) < 1 && fabs(now[1]) < 1);
        CHECK(project_to_ndc(view_previous, projection_previous, points[p], previous));
        const double position[4] = {now[0], now[1], now[2], 1};
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned k = 0; k < 4; ++k) reprojected[row] += (double)reprojection[row * 4 + k] * position[k];
        CHECK(fabs(reprojected[0] / reprojected[3] - previous[0]) < 1e-5 &&
              fabs(reprojected[1] / reprojected[3] - previous[1]) < 1e-5);
    }
    // An eye that did not move, with the same offset: still points stay where they are (within a hundredth of a pixel:
    // the float view's rotation, not quite orthonormal, times its translation, as upstream's has it), no offset.
    CHECK(vr_eye_reprojection(view_now, projection_now, view_now, projection_now, reprojection, jitter));
    for (unsigned p = 0; p < 3; ++p) {
        double now[3], reprojected[4] = {0};
        CHECK(project_to_ndc(view_now, projection_now, points[p], now));
        const double position[4] = {now[0], now[1], now[2], 1};
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned k = 0; k < 4; ++k) reprojected[row] += (double)reprojection[row * 4 + k] * position[k];
        CHECK(fabs(reprojected[0] / reprojected[3] - now[0]) < 1e-5 && fabs(reprojected[1] / reprojected[3] - now[1]) < 1e-5);
    }
    CHECK(jitter[0] == 0 && jitter[1] == 0);
    // A projection with no inverse.
    float flat[16] = {0};
    CHECK(!vr_eye_reprojection(view_now, flat, view_previous, projection_previous, reprojection, jitter));
    double singular[16] = {0}, inverse[16];
    singular[0] = singular[5] = singular[10] = 1;
    CHECK(!vr_invert4(singular, inverse));
    singular[15] = 2;
    CHECK(vr_invert4(singular, inverse) && fabs(inverse[15] - 0.5) < 1e-15 && inverse[0] == 1);
}

int main(void) {
    const double quarter_turn = acos(-1.0) / 2;
    const double right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, ahead[3] = {0, 0, -1};
    double rotated[3];

    // The identity leaves any vector as it is.
    const double identity[4] = {0, 0, 0, 1};
    const double arbitrary[3] = {0.3, -1.7, 2.9};
    vr_rotate(identity, arbitrary, rotated);
    CHECK(vectors_match(rotated, 0.3, -1.7, 2.9));

    // A quarter turn about +y (a yaw to the left, seen from above): ahead goes to the left, right goes to ahead, and
    // up stays up.
    double yaw_left[4];
    quaternion_about_axis(0, 1, 0, quarter_turn, yaw_left);
    vr_rotate(yaw_left, ahead, rotated);
    CHECK(vectors_match(rotated, -1, 0, 0));
    vr_rotate(yaw_left, right, rotated);
    CHECK(vectors_match(rotated, 0, 0, -1));
    vr_rotate(yaw_left, up, rotated);
    CHECK(vectors_match(rotated, 0, 1, 0));

    // A quarter turn about +x (a pitch up): ahead goes to up, up goes to the back.
    double pitch_up[4];
    quaternion_about_axis(1, 0, 0, quarter_turn, pitch_up);
    vr_rotate(pitch_up, ahead, rotated);
    CHECK(vectors_match(rotated, 0, 1, 0));
    vr_rotate(pitch_up, up, rotated);
    CHECK(vectors_match(rotated, 0, 0, 1));

    // A quarter turn about +z (a roll to the left): right goes to up, up goes to the left, ahead stays.
    double roll_left[4];
    quaternion_about_axis(0, 0, 1, quarter_turn, roll_left);
    vr_rotate(roll_left, right, rotated);
    CHECK(vectors_match(rotated, 0, 1, 0));
    vr_rotate(roll_left, up, rotated);
    CHECK(vectors_match(rotated, -1, 0, 0));
    vr_rotate(roll_left, ahead, rotated);
    CHECK(vectors_match(rotated, 0, 0, -1));

    // A half turn about +y turns ahead to the back and right to the left.
    double turn_around[4];
    quaternion_about_axis(0, 1, 0, 2 * quarter_turn, turn_around);
    vr_rotate(turn_around, ahead, rotated);
    CHECK(vectors_match(rotated, 0, 0, 1));
    vr_rotate(turn_around, right, rotated);
    CHECK(vectors_match(rotated, -1, 0, 0));

    // An eye's orientation turns its view's directions into LOCAL; its conjugate (exec.c's to_eye) turns them back.
    double eye_orientation[4];
    const double axis_length = sqrt(0.2 * 0.2 + 0.9 * 0.9 + 0.4 * 0.4);
    quaternion_about_axis(0.2 / axis_length, 0.9 / axis_length, 0.4 / axis_length, 0.7, eye_orientation);
    const double to_eye[4] = {-eye_orientation[0], -eye_orientation[1], -eye_orientation[2], eye_orientation[3]};
    double in_local[3], back_in_view[3];
    vr_rotate(eye_orientation, arbitrary, in_local);
    vr_rotate(to_eye, in_local, back_in_view);
    CHECK(vectors_match(back_in_view, 0.3, -1.7, 2.9));
    CHECK(!vectors_match(in_local, 0.3, -1.7, 2.9));

    // Any unit rotation keeps a vector's length.
    CHECK(fabs(vector_length(in_local) - vector_length(arbitrary)) < tolerance);

    // Rotating by a pitch and then a yaw is rotating once by their product: successive calls compose as quaternion
    // products do (exec.c applies the calibration heading, then an eye's inverse orientation, by successive calls).
    double pitch_then_yaw[4], step_by_step[3], at_once[3];
    quaternion_sequence(pitch_up, yaw_left, pitch_then_yaw);
    vr_rotate(pitch_up, right, rotated);
    vr_rotate(yaw_left, rotated, step_by_step);
    vr_rotate(pitch_then_yaw, right, at_once);
    CHECK(vectors_match(at_once, step_by_step[0], step_by_step[1], step_by_step[2]));
    CHECK(vectors_match(step_by_step, 0, 0, -1));  // (right is untouched by the pitch, then yawed to ahead)

    // The window mirror's letterbox: the final image's left half, scaled by one factor to fit the presented image and
    // centred in it.
    VrLetterbox box;
    // A window the final image's size: the 960x1080 half at its own size (scale 1), centred between bars at the sides.
    CHECK(vr_letterbox(1920, 1080, 1920, 1080, &box));
    CHECK(box.half_width == 960 && box.half_height == 1080 && box.fitted_width == 960 && box.fitted_height == 1080);
    CHECK(box.left == 480 && box.top == 0);
    // A larger final image in a smaller window: scaled down by the height (2/3), still bars at the sides.
    CHECK(vr_letterbox(2560, 1440, 3840, 2160, &box));
    CHECK(box.half_width == 1920 && box.fitted_width == 1280 && box.fitted_height == 1440);
    CHECK(box.left == 640 && box.top == 0);
    // A window narrower than the half's shape: scaled by the width, bars above and below.
    CHECK(vr_letterbox(480, 1080, 1920, 1080, &box));
    CHECK(box.fitted_width == 480 && box.fitted_height == 540 && box.left == 0 && box.top == 270);
    // An odd fit rounds to the nearest pixel: 960 x (600 / 1080) = 533.3, centred in 800 (133 on the left).
    CHECK(vr_letterbox(800, 600, 1920, 1080, &box));
    CHECK(box.fitted_width == 533 && box.fitted_height == 600 && box.left == 133 && box.top == 0);
    // A window larger than the half: scaled up (here by 2, the height's ratio), bars at the sides.
    CHECK(vr_letterbox(3840, 2160, 1920, 1080, &box));
    CHECK(box.fitted_width == 1920 && box.fitted_height == 2160 && box.left == 960 && box.top == 0);
    // A one-pixel window still gets one pixel.
    CHECK(vr_letterbox(1, 1, 1920, 1080, &box));
    CHECK(box.fitted_width == 1 && box.fitted_height == 1 && box.left == 0 && box.top == 0);
    // Nothing to copy: no final image, a final image one pixel wide (no left half), an empty window.
    box = (VrLetterbox){1, 2, 3, 4, 5, 6};
    CHECK(!vr_letterbox(1920, 1080, 0, 1080, &box) && !vr_letterbox(1920, 1080, 1, 1080, &box));
    CHECK(!vr_letterbox(1920, 1080, 1920, 0, &box) && !vr_letterbox(0, 1080, 1920, 1080, &box));
    CHECK(box.half_width == 1 && box.left == 5);  // (left as it was)

    // A point on the aim ray, from an eye: origin + direction x along / scale - the eye's position.
    const float ray_origin[3] = {0, 1.5f, 0}, left_eye_position[3] = {-0.03f, 1.5f, 0};
    double from_eye[3];
    vr_aim_ray_point_from_eye(ray_origin, ahead, 20, 10, left_eye_position, from_eye);
    CHECK(vectors_match(from_eye, 0.03f, 0, -2));  // (20 game units at 10 a metre: 2 m ahead, the eye 3 cm to the left)

    // Where an eye sees a point: an eye looking down -z, a FOV of 45 degrees each way (tangents -1, 1, 1, -1).
    const float facing_ahead[4] = {0, 0, 0, 1};
    const double square_fov[4] = {-1, 1, 1, -1};
    double view[3], image[2];
    const double straight_ahead[3] = {0, 0, -2};
    CHECK(vr_eye_image_position(straight_ahead, facing_ahead, square_fov, 1e-6, view, image));
    CHECK(vectors_match(view, 0, 0, -2) && image[0] == 0.5 && image[1] == 0.5);  // (the image's centre)
    // Up and to the right, half way to the FOV's edges: right of centre, and above it (image y runs down).
    const double up_right[3] = {0.5, 0.5, -1};
    CHECK(vr_eye_image_position(up_right, facing_ahead, square_fov, 1e-6, view, image));
    CHECK(image[0] == 0.75 && image[1] == 0.25);
    // Outside the FOV: outside 0..1, still reported.
    const double far_left[3] = {-3, 0, -1};
    CHECK(vr_eye_image_position(far_left, facing_ahead, square_fov, 1e-6, view, image) && image[0] == -1);
    // Behind the eye, or closer ahead than the margin: no place, image untouched.
    const double behind[3] = {0, 0, 1}, barely_ahead[3] = {0, 0, -5e-5};
    image[0] = image[1] = 7;
    CHECK(!vr_eye_image_position(behind, facing_ahead, square_fov, 1e-6, view, image) && image[0] == 7);
    CHECK(!vr_eye_image_position(barely_ahead, facing_ahead, square_fov, 1e-4, view, image) && image[0] == 7);
    CHECK(vr_eye_image_position(barely_ahead, facing_ahead, square_fov, 1e-6, view, image) && image[0] == 0.5);
    // An eye turned a quarter to the left (yaw +90 degrees) sees a point to the LOCAL left straight ahead.
    double turned_left[4];
    quaternion_about_axis(0, 1, 0, quarter_turn, turned_left);
    const float turned_left_float[4] = {(float)turned_left[0], (float)turned_left[1], (float)turned_left[2],
                                        (float)turned_left[3]};
    const double to_the_left[3] = {-2, 0, 0};
    CHECK(vr_eye_image_position(to_the_left, turned_left_float, square_fov, 1e-6, view, image));
    CHECK(fabs(image[0] - 0.5) < 1e-6 && fabs(image[1] - 0.5) < 1e-6 && view[2] < -1.99);
    // An asymmetric FOV (more to the left than to the right): straight ahead is right of the image's centre.
    const double wide_left_fov[4] = {-1.2, 0.8, 1, -1};
    CHECK(vr_eye_image_position(straight_ahead, facing_ahead, wide_left_fov, 1e-6, view, image));
    CHECK(fabs(image[0] - 0.6) < tolerance && image[1] == 0.5);

    // A degenerate FOV, pinned as the code handles it today (in practice the runtime's FOVs open both ways). With right equal to left and up equal to down the place divides by zero: a point straight ahead lands at
    // minus and plus infinity, which the crosshair sample's pixel bounds and the rune aim's 0..1 bounds then refuse,
    // and before which the world-tied HUD returns on its own check (right > left, up > down). With right below left and
    // up below down the image is mirrored both ways: still 0.5 at the centre, and a point up and to the right lands at
    // 0.25 across and 0.75 down, inside 0..1 (only the world-tied HUD's own check refuses it).
    const double collapsed_fov[4] = {1, 1, 1, 1}, reversed_fov[4] = {1, -1, -1, 1};
    CHECK(vr_eye_image_position(straight_ahead, facing_ahead, collapsed_fov, 1e-6, view, image));
    CHECK(image[0] == -INFINITY && image[1] == INFINITY);
    CHECK(vr_eye_image_position(straight_ahead, facing_ahead, reversed_fov, 1e-6, view, image));
    CHECK(image[0] == 0.5 && image[1] == 0.5);
    CHECK(vr_eye_image_position(up_right, facing_ahead, reversed_fov, 1e-6, view, image));
    CHECK(image[0] == 0.25 && image[1] == 0.75);

    // The three former copies against the shared functions, on random poses, FOVs and points: the same answer to
    // "in front?", and the same bits for every place, and for the marker its point in the eye's view too (exec.c goes
    // on to use it for the world's up there). The last 20,000 cases take degenerate FOVs: 10,000 collapsed (right equal
    // to left, up equal to down) and 10,000 reversed.
    unsigned eye_cases = 0, crosshair_disagreements = 0, marker_disagreements = 0, rune_disagreements = 0;
    unsigned crosshair_ahead = 0, marker_ahead = 0, rune_ahead = 0;
    for (unsigned trial = 0; trial < 220000; ++trial) {
        EyeCase eye_case;
        random_eye_case(&eye_case);
        if (trial >= 200000 && trial < 210000) {  // (collapsed)
            eye_case.fov[1] = eye_case.fov[0];
            eye_case.fov[3] = eye_case.fov[2];
        } else if (trial >= 210000) {  // (reversed)
            const float left_angle = eye_case.fov[0], up_angle = eye_case.fov[2];
            eye_case.fov[0] = eye_case.fov[1];
            eye_case.fov[1] = left_angle;
            eye_case.fov[2] = eye_case.fov[3];
            eye_case.fov[3] = up_angle;
        }
        ++eye_cases;
        double before[5] = {0}, now[5] = {0};
        const bool crosshair_was = crosshair_pixel_before(&eye_case, before, before + 2);
        const bool crosshair_is = crosshair_pixel_now(&eye_case, now, now + 2);
        crosshair_ahead += crosshair_was;
        crosshair_disagreements += crosshair_was != crosshair_is || !same_bits(before, now, 3);
        memset(before, 0, sizeof before);
        memset(now, 0, sizeof now);
        const bool marker_was = marker_place_before(&eye_case, before, before + 2),
                   marker_is = marker_place_now(&eye_case, now, now + 2);
        marker_ahead += marker_was;
        marker_disagreements += marker_was != marker_is || !same_bits(before, now, 5);
        memset(before, 0, sizeof before);
        memset(now, 0, sizeof now);
        const bool rune_was = rune_point_before(&eye_case, before), rune_is = rune_point_now(&eye_case, now);
        rune_ahead += rune_was;
        rune_disagreements += rune_was != rune_is || !same_bits(before, now, 2);
    }
    CHECK(crosshair_disagreements == 0 && marker_disagreements == 0 && rune_disagreements == 0);
    // (the random cases reach both answers: points ahead of the eye and behind it)
    CHECK(crosshair_ahead > 1000 && crosshair_ahead < eye_cases && marker_ahead > 1000 && marker_ahead < eye_cases);
    CHECK(rune_ahead > 1000 && rune_ahead < eye_cases);
    printf("vr_math former copies: %u cases, disagreements crosshair %u, marker %u, rune %u (ahead %u, %u, %u)\n",
           eye_cases, crosshair_disagreements, marker_disagreements, rune_disagreements, crosshair_ahead,
           marker_ahead, rune_ahead);

    // vr_aim_ray_hit (where the aim ray meets the scene). Depth terms {0, b, -1, 0} store b / -z: the scene's depth is
    // b / stored (in OpenGL's mode, b / (2 stored - 1)).
    {
        const float one_over[4] = {0, 1, -1, 0}, three_over[4] = {0, 3, -1, 0};
        unsigned hit_step = 99;
        // Between two points: the scene at 4 (stored 0.25), points at 3 (1 in front) and 5 (1 behind): halfway.
        const double between_along[5] = {1, 2, 3, 5, 8};
        const float between_stored[5] = {0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
        AimSample sample = aim_sample(5, between_along, between_stored, one_over, false);
        CHECK(vr_aim_ray_hit(sample.stored_depth, sample.ray_distance, sample.eye_depth, 5, one_over, false,
                             &hit_step) == 4 && hit_step == 3);
        // At the first point: already behind the scene at 2 (stored 0.5), the point itself.
        const double first_along[3] = {4, 6, 8};
        const float first_stored[3] = {0.5f, 0.5f, 0.5f};
        sample = aim_sample(3, first_along, first_stored, one_over, false);
        CHECK(aim_hit_now(&sample, &hit_step) == 4 && hit_step == 0);
        // At the last point: the scene at 6 (3 / 0.5), points at 4 (2 in front) and 8 (2 behind).
        const double last_along[4] = {1, 2, 4, 8};
        const float last_stored[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        sample = aim_sample(4, last_along, last_stored, three_over, false);
        CHECK(aim_hit_now(&sample, &hit_step) == 6 && hit_step == 3);
        // No hit: every point in front of the scene (at 16), or no scene at all (cleared, far): 0, and step 0.
        const float far_stored[4] = {0.0625f, 0.0625f, 0.0625f, 0.0625f}, cleared[4] = {0, 1, 0, 1};
        sample = aim_sample(4, last_along, far_stored, one_over, false);
        hit_step = 99;
        CHECK(aim_hit_now(&sample, &hit_step) == 0 && hit_step == 0);
        sample = aim_sample(4, last_along, cleared, one_over, false);
        CHECK(aim_hit_now(&sample, &hit_step) == 0 && hit_step == 0);
        // No points.
        hit_step = 99;
        CHECK(vr_aim_ray_hit(sample.stored_depth, sample.ray_distance, sample.eye_depth, 0, one_over, false,
                             &hit_step) == 0 && hit_step == 0);
        // Stored depths not strictly between 0 and 1, or not numbers, are no scene: the scene at 2 is met only at
        // the point at 3, and the hit is pushed to (all but) that point, since the point before had no scene.
        const double odd_along[7] = {1.5, 1.6, 1.7, 1.8, 1.9, 2.5, 3};
        const float odd_stored[7] = {NAN, INFINITY, -0.25f, 1.5f, 1.0f, -INFINITY, 0.5f};
        sample = aim_sample(7, odd_along, odd_stored, one_over, false);
        double hit = aim_hit_now(&sample, &hit_step);
        CHECK(hit > 2.999999 && hit <= 3 && hit_step == 6);
        // A z behind the eye (terms {0, -1, -1, 0}), and terms that give no z (a zero denominator), are no scene.
        const float behind_terms[4] = {0, -1, -1, 0}, no_z_terms[4] = {0.5f, 1, 1, 0};
        sample = aim_sample(4, last_along, last_stored, behind_terms, false);
        CHECK(aim_hit_now(&sample, &hit_step) == 0);
        sample = aim_sample(4, last_along, last_stored, no_z_terms, false);
        CHECK(aim_hit_now(&sample, &hit_step) == 0);
        // The two depth modes read the same stored value differently: 0.75 is a device depth of 0.75 (the scene at
        // 4/3) in the 0..1 mode, of 0.5 (the scene at 2) in OpenGL's; points at 1 and 3.
        const double mode_along[2] = {1, 3};
        const float mode_stored[2] = {0.75f, 0.75f};
        sample = aim_sample(2, mode_along, mode_stored, one_over, true);
        CHECK(aim_hit_now(&sample, &hit_step) == 2 && hit_step == 1);
        sample = aim_sample(2, mode_along, mode_stored, one_over, false);
        CHECK(fabs(aim_hit_now(&sample, &hit_step) - 4.0 / 3) < 1e-12 && hit_step == 1);
        // In OpenGL's mode a stored value below a half is a device depth below 0: with these terms, behind the eye.
        const float low_stored[2] = {0.25f, 0.25f};
        const double low_along[2] = {1, 5};  // (0..1 mode: the scene at 4, between them)
        sample = aim_sample(2, low_along, low_stored, one_over, false);
        CHECK(aim_hit_now(&sample, &hit_step) == 4 && hit_step == 1);
        sample = aim_sample(2, low_along, low_stored, one_over, true);
        CHECK(aim_hit_now(&sample, &hit_step) == 0);
    }

    test_dlaa_camera();
    test_eye_reprojection();
    printf("vr_math fixture: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
