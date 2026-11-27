// Pure, asset-free checks of the sound listener's head offset (runtime/hle/pc_vr_listener_math.h).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/hle/pc_vr_listener_math.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "listener check failed line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

static const double degrees = 3.14159265358979323846 / 180.0;

// The game's look-at view: rows right, up, back; translation -R * position.
static void view_of(const PcVrHeadLookCamera *camera, float view[12]) {
    double right[3], up[3], back[3];
    CHECK(pc_vr_head_look_basis(camera, right, up, back));
    const double *rows[3] = {right, up, back};
    for (unsigned row = 0; row < 3; ++row) {
        double translation = 0;
        for (unsigned column = 0; column < 3; ++column) {
            view[row * 4 + column] = (float)rows[row][column];
            translation -= rows[row][column] * camera->position[column];
        }
        view[row * 4 + 3] = (float)translation;
    }
}

static void transform(const float view[12], const double point[3], double out[3]) {
    for (unsigned row = 0; row < 3; ++row)
        out[row] = view[row * 4] * point[0] + view[row * 4 + 1] * point[1] + view[row * 4 + 2] * point[2] +
                   view[row * 4 + 3];
}

static double largest_difference(const float first[12], const float second[12], double rotation_scale) {
    double largest = 0;
    for (unsigned index = 0; index < 12; ++index) {
        const double difference = fabs((double)first[index] - second[index]) * (index % 4 == 3 ? 1 : rotation_scale);
        if (difference > largest) largest = difference;
    }
    return largest;
}

// A base camera at `position` looking along heading `yaw` (about +Y, 0 = -Z), level, 6 units to its target.
static PcVrHeadLookCamera camera_at(const double position[3], double yaw) {
    PcVrHeadLookCamera camera;
    for (unsigned axis = 0; axis < 3; ++axis) camera.position[axis] = (float)position[axis];
    camera.target[0] = (float)(position[0] - 6 * sin(yaw));
    camera.target[1] = (float)position[1];
    camera.target[2] = (float)(position[2] - 6 * cos(yaw));
    camera.up[0] = 0, camera.up[1] = 1, camera.up[2] = 0;
    return camera;
}

// Two eyes of a head turned by `yaw` and `pitch` from `camera` and moved by `lean` (camera right, up, back), 0.9 units
// apart, each canted outward by `cant`, with a look radius and an up length of the game's own (not unit).
static void eyes_of(const PcVrHeadLookCamera *camera, double yaw, double pitch, const double lean[3], double cant,
                    PcVrHeadLookCamera eyes[2], double head_position[3]) {
    double right[3], up[3], back[3];
    CHECK(pc_vr_head_look_basis(camera, right, up, back));
    // the head's axes in the camera's frame: yaw about up, then pitch about the turned right
    const double local_forward[3] = {-sin(yaw) * cos(pitch), sin(pitch), -cos(yaw) * cos(pitch)};
    const double local_up[3] = {sin(yaw) * sin(pitch), cos(pitch), cos(yaw) * sin(pitch)};
    const double local_right[3] = {cos(yaw), 0, -sin(yaw)};
    double forward[3], head_up[3], head_right[3];
    for (unsigned axis = 0; axis < 3; ++axis) {
        forward[axis] = local_forward[0] * right[axis] + local_forward[1] * up[axis] + local_forward[2] * back[axis];
        head_up[axis] = local_up[0] * right[axis] + local_up[1] * up[axis] + local_up[2] * back[axis];
        head_right[axis] = local_right[0] * right[axis] + local_right[1] * up[axis] + local_right[2] * back[axis];
        head_position[axis] = camera->position[axis] + lean[0] * right[axis] + lean[1] * up[axis] + lean[2] * back[axis];
    }
    for (unsigned eye = 0; eye < 2; ++eye) {
        const double side = eye ? 1 : -1, eye_cant = side * cant;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const double eye_forward = cos(eye_cant) * forward[axis] + sin(eye_cant) * head_right[axis];
            eyes[eye].position[axis] = (float)(head_position[axis] + side * 0.45 * head_right[axis]);
            eyes[eye].target[axis] = (float)(eyes[eye].position[axis] + 6.0 * eye_forward);
            eyes[eye].up[axis] = (float)(2.0 * head_up[axis]);
        }
    }
}

// The head's own view, at its position with its axes (what the listener's camera view should become).
static void head_view(const PcVrHeadLookCamera *camera, double yaw, double pitch, const double head_position[3],
                      float view[12]) {
    PcVrHeadLookCamera parallel[2];
    double unused[3];
    const double no_lean[3] = {0, 0, 0};
    eyes_of(camera, yaw, pitch, no_lean, 0, parallel, unused);
    PcVrHeadLookCamera head = parallel[0];
    for (unsigned axis = 0; axis < 3; ++axis) {
        const double forward = (double)parallel[0].target[axis] - parallel[0].position[axis];
        head.position[axis] = (float)head_position[axis];
        head.target[axis] = (float)(head_position[axis] + forward);
    }
    view_of(&head, view);
}

static void test_unturned_head_keeps_the_view(void) {
    const double position[3] = {12, 3, -40};
    const PcVrHeadLookCamera camera = camera_at(position, 30 * degrees);
    PcVrHeadLookCamera eyes[2];
    double head_position[3];
    const double no_lean[3] = {0, 0, 0};
    eyes_of(&camera, 0, 0, no_lean, 0, eyes, head_position);
    PcVrListenerOffset offset;
    CHECK(pc_vr_listener_offset_build(&camera, eyes, &offset));
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned column = 0; column < 3; ++column)
            CHECK(fabs(offset.rotation[row][column] - (row == column)) < 1e-6);
        CHECK(fabs(offset.translation[row]) < 1e-5);
    }
    float view[12], turned[12];
    view_of(&camera, view);
    pc_vr_listener_offset_apply(&offset, view, turned);
    CHECK(largest_difference(view, turned, 1) < 1e-5);
}

// The user's case: the head turned, a sound straight ahead of the game camera moves to the side, and the camera's
// view becomes the head's own; the listener's view 1.8 units ahead keeps that point and turns with the head.
static void test_turned_and_leaning_head(void) {
    // world coordinates of the size the game has (the Plateau), to see the float precision of the result
    const double position[3] = {-665.1084, 173.99132, 1582.137};
    const PcVrHeadLookCamera camera = camera_at(position, -131.4 * degrees);
    const double lean[3] = {1.2, -0.4, 0.7};
    PcVrHeadLookCamera eyes[2];
    double head_position[3];
    eyes_of(&camera, 60 * degrees, -15 * degrees, lean, 0, eyes, head_position);
    PcVrListenerOffset offset;
    CHECK(pc_vr_listener_offset_build(&camera, eyes, &offset));
    float camera_view[12], turned[12], expected[12];
    view_of(&camera, camera_view);
    pc_vr_listener_offset_apply(&offset, camera_view, turned);
    head_view(&camera, 60 * degrees, -15 * degrees, head_position, expected);
    // (the eyes are floats a thousand units out: their directions hold to about 1e-5, so the rotations are compared,
    // and the translations through points near the head, where a sound's place relative to the listener is decided)
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column)
            CHECK(fabs(turned[row * 4 + column] - expected[row * 4 + column]) < 1e-4);
    double right[3], up[3], back[3];
    CHECK(pc_vr_head_look_basis(&camera, right, up, back));
    for (int step = -2; step <= 2; ++step) {
        double point[3], by_offset[3], by_head[3];
        for (unsigned axis = 0; axis < 3; ++axis)
            point[axis] = head_position[axis] + 7.0 * step * right[axis] - 3.0 * step * back[axis] + step * up[axis];
        transform(turned, point, by_offset);
        transform(expected, point, by_head);
        for (unsigned axis = 0; axis < 3; ++axis) CHECK(fabs(by_offset[axis] - by_head[axis]) < 5e-3);
    }

    // a sound 10 units straight ahead of the camera: ahead before, to the right of a head turned 60 degrees left
    double ahead[3], before[3], after[3];
    for (unsigned axis = 0; axis < 3; ++axis) ahead[axis] = position[axis] - 10 * back[axis];
    transform(camera_view, ahead, before);
    transform(turned, ahead, after);
    CHECK(fabs(before[0]) < 1e-3 && before[2] < -9.99);
    CHECK(after[0] > 4.0);  // (to the right)
    double distance_before = 0, distance_after = 0, distance_from_head = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        distance_before += before[axis] * before[axis];
        distance_after += after[axis] * after[axis];
        distance_from_head += (ahead[axis] - head_position[axis]) * (ahead[axis] - head_position[axis]);
    }
    CHECK(fabs(sqrt(distance_before) - 10) < 5e-3);
    CHECK(fabs(sqrt(distance_after) - sqrt(distance_from_head)) < 5e-3);  // (from the head, which leant)

    // the head itself is the turned view's origin
    double head_in_view[3];
    transform(turned, head_position, head_in_view);
    for (unsigned axis = 0; axis < 3; ++axis) CHECK(fabs(head_in_view[axis]) < 5e-3);

    // the listener's view, 1.8 units ahead of the camera: its origin moves with the lean alone, its axes turn
    float listener_view[12];
    memcpy(listener_view, camera_view, sizeof listener_view);
    listener_view[11] += 1.8f;  // (the poser's offset (0, 0, -1.8) in the camera's frame)
    pc_vr_listener_offset_apply(&offset, listener_view, listener_view);  // (in place)
    double listener_point[3], in_turned[3];
    for (unsigned axis = 0; axis < 3; ++axis)
        listener_point[axis] = position[axis] - 1.8 * back[axis] + lean[0] * right[axis] + lean[1] * up[axis] +
                               lean[2] * back[axis];
    transform(listener_view, listener_point, in_turned);
    for (unsigned axis = 0; axis < 3; ++axis) CHECK(fabs(in_turned[axis]) < 5e-3);
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column)
            CHECK(fabs(listener_view[row * 4 + column] - expected[row * 4 + column]) < 1e-4);
}

// Canted eyes (each turned outward): the head looks along their mean, the straight view.
static void test_canted_eyes_average(void) {
    const double position[3] = {0, 0, 0};
    const PcVrHeadLookCamera camera = camera_at(position, 0);
    const double no_lean[3] = {0, 0, 0};
    PcVrHeadLookCamera eyes[2];
    double head_position[3];
    eyes_of(&camera, 0, 0, no_lean, 10 * degrees, eyes, head_position);
    PcVrListenerOffset offset;
    CHECK(pc_vr_listener_offset_build(&camera, eyes, &offset));
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column)
            CHECK(fabs(offset.rotation[row][column] - (row == column)) < 1e-6);
}

static void test_unusable_inputs_leave_the_offset(void) {
    const double position[3] = {5, 5, 5};
    const PcVrHeadLookCamera camera = camera_at(position, 0);
    const double no_lean[3] = {0, 0, 0};
    PcVrHeadLookCamera eyes[2];
    double head_position[3];
    eyes_of(&camera, 20 * degrees, 0, no_lean, 0, eyes, head_position);
    PcVrListenerOffset offset, kept;
    memset(&offset, 0x5a, sizeof offset);
    kept = offset;
    PcVrHeadLookCamera broken[2] = {eyes[0], eyes[1]};
    memcpy(broken[1].target, broken[1].position, sizeof broken[1].target);  // (an eye without a direction)
    CHECK(!pc_vr_listener_offset_build(&camera, broken, &offset));
    broken[1] = eyes[1];
    broken[0].up[0] = broken[0].up[1] = broken[0].up[2] = 0;  // (without an up)
    CHECK(!pc_vr_listener_offset_build(&camera, broken, &offset));
    broken[0] = eyes[0];
    broken[0].position[1] = NAN;
    CHECK(!pc_vr_listener_offset_build(&camera, broken, &offset));
    PcVrHeadLookCamera flat = camera;
    flat.target[1] = flat.position[1] + 1, flat.target[0] = flat.position[0], flat.target[2] = flat.position[2];
    CHECK(!pc_vr_listener_offset_build(&flat, eyes, &offset));  // (a camera looking along its up)
    CHECK(!memcmp(&offset, &kept, sizeof offset));
}

int main(void) {
    test_unturned_head_keeps_the_view();
    test_turned_and_leaning_head();
    test_canted_eyes_average();
    test_unusable_inputs_leave_the_offset();
    printf("PC VR listener math fixture: %u checks passed\n", checks);
    return 0;
}
