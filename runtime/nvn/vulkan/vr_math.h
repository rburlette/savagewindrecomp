// PC VR renderer: geometry on plain numbers (D048 onward).
//
// Responsible for: the small geometric helpers of the PC VR code: the rotation of a vector by a pose's orientation, a
// point on the aim ray and where an eye sees a point (the crosshair's depth sample, the world-tied HUD markers, the
// rune target's aim point), where the aim ray meets the scene (vr_aim.c), and the window mirror's letterbox
// (vr_mirror.c).
// Each function takes values and returns values: no state, no Vulkan, no guest memory, so a fixture
// (tools/test_vr_math.c) checks it without the renderer.
//
// Not owned here: the headset's poses and the D053 calibration (runtime/xr/xr_runtime.h publishes them as
// XrPoseSnapshot and XrCalibration), the game camera's view and projection maths (runtime/hle/pc_vr_*_math.h), and
// what the renderer decides with a result (exec.c).
//
// Conventions, those of OpenXR, which the poses come from:
//  - Spaces: LOCAL is the headset's reference space, in metres: x to the right, y up (against gravity), z toward the
//    player's back, so straight ahead is -z. It is right-handed. An eye's view space has the same axes fixed to that
//    eye: x right, y up, looking down -z.
//  - Orientations are unit quaternions stored as (x, y, z, w), w the real part (OpenXR's XrQuaternionf order). A
//    quaternion turns a vector from the space it is expressed in into the space that holds the pose: an eye's
//    orientation turns view-space directions into LOCAL ones. Its conjugate (-x, -y, -z, w) turns the other way, which
//    is how exec.c brings a LOCAL point into an eye's view.
//  - A positive angle about an axis turns counter-clockwise when that axis points at the viewer (right-hand rule): a
//    quarter turn about +y takes straight ahead (-z) to the left (-x).
//  - Eyes are numbered as OpenXR's views and the render certificates' native_index: 0 the left eye, 1 the right.
//  - Image rectangles (the letterbox) are in the images' own pixels as allocated, from the top-left corner, x to the
//    right and y down, as Vulkan's copy offsets are.
#ifndef SAVAGEWIND_VR_MATH_H
#define SAVAGEWIND_VR_MATH_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

// Rotates vector by the unit quaternion rotation (x, y, z, w) and writes the result to out. The PC VR code uses it to
// carry the aim ray and the HUD markers' points between the calibrated game frame, LOCAL and each eye's view (the
// crosshair's depth sample, the world-tied HUD markers and the rune target's aim point, exec.c). It computes
// v + w t + q x t with t = 2 (q x v) (q here the quaternion's vector part), the expansion of q v q* that needs no full
// quaternion product. That expansion assumes unit length: for any other quaternion the result is not a rotation of
// vector at all, so callers pass orientations as OpenXR gives them (normalised).
static void vr_rotate(const double rotation[4], const double vector[3], double out[3]) {
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

// A point on the aim ray (D073), seen from an eye: the ray starts at the calibration origin (origin_xyz, LOCAL metres)
// and runs along direction (the calibrated heading's forward, a unit vector in LOCAL); the point lies `along` game
// units out, which units_per_meter (the eye's world scale) turns into metres. Writes the point less the eye's position
// (eye_position_xyz, LOCAL metres) to from_eye, ready for vr_eye_image_position. The crosshair's depth sample
// (vr_aim.c) and the rune target's aim point (exec.c vr_rune_aim_patch) both place their points this way.
static inline void vr_aim_ray_point_from_eye(const float origin_xyz[3], const double direction[3], double along,
                                             double units_per_meter, const float eye_position_xyz[3],
                                             double from_eye[3]) {
    for (unsigned j = 0; j < 3; ++j)
        from_eye[j] = origin_xyz[j] + direction[j] * along / units_per_meter - eye_position_xyz[j];
}

// Where an eye sees a point. from_eye is the point less the eye's position, in LOCAL metres; the inverse of the eye's
// orientation (eye_orientation_xyzw, as OpenXR gives it) turns it into the eye's view: view, metres, x right, y up, -z
// ahead. False (image untouched) when the point is not at least ahead_margin metres in front of the eye. Otherwise
// image is where it lands across the eye's image, from the tangents of the eye's FOV (fov_tangents: left, right, up,
// down, left and down negative): image[0] from 0 at the left edge to 1 at the right, image[1] from 0 at the top to 1 at
// the bottom. A point outside the FOV lands outside 0..1, and a FOV with right <= left or up <= down gives no
// meaningful place: the callers check what they need. Used by the crosshair's depth sample (margin 1e-6, scaled to the
// depth image's pixels), the world-tied HUD markers (margin 1e-4, into the eye's half of the final image) and the rune
// target's aim point (margin 1e-6, kept in 0..1).
static inline bool vr_eye_image_position(const double from_eye[3], const float eye_orientation_xyzw[4],
                                         const double fov_tangents[4], double ahead_margin, double view[3],
                                         double image[2]) {
    // (the eye's orientation inverted: LOCAL to the eye's view)
    const double to_eye[4] = {-eye_orientation_xyzw[0], -eye_orientation_xyzw[1], -eye_orientation_xyzw[2],
                              eye_orientation_xyzw[3]};
    vr_rotate(to_eye, from_eye, view);
    if (!(view[2] < -ahead_margin)) return false;
    const double tan_x = view[0] / -view[2], tan_y = view[1] / -view[2];
    image[0] = (tan_x - fov_tangents[0]) / (fov_tangents[1] - fov_tangents[0]);
    image[1] = (fov_tangents[2] - tan_y) / (fov_tangents[2] - fov_tangents[3]);
    return true;
}

// Where the aim ray meets the scene (D073), from one sampling of it (vr_aim.c): its first `steps` points in the eye's
// view, in order along the ray, each with its distance along the ray (ray_distance, game units), its depth in the
// eye's view (eye_depth, game units, positive ahead) and the scene's depth the eye's depth image stored at the pixel
// where the eye sees it (stored_depth, the D32 value). The stored value goes back to the scene's view-space z through
// the eye projection's depth terms (the certificate's depth_terms: device depth = (terms[0] z + terms[1]) /
// (terms[2] z + terms[3])), after mapping it from 0..1 to -1..1 when the game's depth mode was OpenGL's
// (gl_depth_range). A stored value not strictly between 0 and 1 (cleared, far, or not a number), a z not ahead of the
// eye, or terms that give no z count as no scene at that point. The first point at or behind the scene is the hit: from
// the point before it (in front of the scene) to this one, the scene lies where the ray's depth crosses it, by linear
// interpolation; at the first point, or with both points on the scene, the hit is the point itself. Returns the hit's
// distance along the ray (game units), 0 when every point is in front of the scene (a miss: the sky, or past the eye's
// view); hit_step (for the log) is the point at the hit, 0 on a miss. The caller caps the distance (VR_AIM_MAX_M).
// Known properties, kept from the code it came from: a hit that computes to 0 does not end the search (the ray's
// points start 0.25 game units out, so it does not happen); a point whose depth is not a number counts as behind the
// scene, and one infinitely far in front can make the next hit a NaN, which vr_aim.c drops (the sampler's depths are
// finite).
static inline double vr_aim_ray_hit(const float *stored_depth, const double *ray_distance, const double *eye_depth,
                                    unsigned steps, const float depth_terms[4], bool gl_depth_range,
                                    unsigned *hit_step) {
    double hit = 0;     // the distance along the ray where it meets the scene (0: not yet)
    double before = 0;  // how far in front of the scene the previous point was
    *hit_step = 0;
    for (unsigned i = 0; i < steps && !hit; ++i) {
        const double stored = stored_depth[i];
        const double point_depth = eye_depth[i], along = ray_distance[i];
        const double device_depth = gl_depth_range ? 2 * stored - 1 : stored;
        // (the view-space z, negative ahead, from device_depth = (terms[0] z + terms[1]) / (terms[2] z + terms[3]))
        const double denominator = device_depth * depth_terms[2] - depth_terms[0];
        const double view = denominator ? (depth_terms[1] - device_depth * depth_terms[3]) / denominator : 0;
        const double scene = stored > 0 && stored < 1 && view < 0 ? -view : 1e30;
        if (point_depth < scene) {
            before = scene - point_depth;
            continue;
        }
        const double past = point_depth - scene, previous = i ? ray_distance[i - 1] : 0;
        hit = i && before + past > 0 ? previous + (along - previous) * before / (before + past) : along;
        *hit_step = i;
    }
    return hit;
}

// Where the window mirror (vr_mirror.c, D085) puts the final image's left half, the left eye, in the presented image:
// scaled by one factor to fit inside it, and centred, so black bars fill whichever direction the presented image has
// room left in after the fit (the sides, or above and below).
typedef struct {
    uint32_t half_width, half_height;     // the left half of the final image: the part copied
    int32_t fitted_width, fitted_height;  // its size once scaled to fit the presented image (rounded to whole pixels)
    int32_t left, top;                    // where its top-left corner lands in the presented image
} VrLetterbox;

// Fills *box for a presented image of presented_width x presented_height and a final image of final_width x
// final_height. False when there is nothing to copy: the final image has no left half, or the fit rounds to no pixels.
// (Where the scale is 1 the fitted size is the half's own, and the mirror copies with nearest filtering.)
static inline bool vr_letterbox(uint32_t presented_width, uint32_t presented_height, uint32_t final_width,
                                uint32_t final_height, VrLetterbox *box) {
    const uint32_t half_width = final_width / 2, half_height = final_height;
    if (!half_width || !half_height) return false;
    const double fit = fmin((double)presented_width / half_width, (double)presented_height / half_height);
    const int32_t fitted_w = (int32_t)(half_width * fit + 0.5), fitted_h = (int32_t)(half_height * fit + 0.5);
    const int32_t left = ((int32_t)presented_width - fitted_w) / 2, top = ((int32_t)presented_height - fitted_h) / 2;
    if (fitted_w <= 0 || fitted_h <= 0) return false;
    *box = (VrLetterbox){half_width, half_height, fitted_w, fitted_h, left, top};
    return true;
}

#endif
