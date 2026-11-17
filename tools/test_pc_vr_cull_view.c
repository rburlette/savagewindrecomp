// Asset-free checks of PC VR's head view for the game's visibility decisions: the view that holds both eyes' frusta
// (runtime/hle/pc_vr_cull_view_math.h) and what the two game patches are answered (runtime/hle/pc_vr_cull_view.c,
// included here with a mocked guest allocator and the fixture's own configuration).
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/hle/pc_vr_cull_view.c"
#include "test_pc_vr_settings.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "cull view check failed line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

void rt_fatal_trace(void) { fprintf(stderr, "unexpected fatal failure\n"); exit(99); }
static unsigned guest_allocations;
void *hle_alloc(uint64_t size) { ++guest_allocations; return calloc(1, (size_t)size); }

// The headset's eyes on Link (the run logs' `eye views fov_lrud`): left, right, up, down in radians.
static const float link_fov[2][4] = {{-0.9425f, 0.6981f, 0.7679f, -0.9599f}, {-0.6981f, 0.9425f, 0.7679f, -0.9599f}};

typedef struct { double right[3], up[3], forward[3]; } Frame;

// A head frame from yaw (about +y), then pitch (about its right), then roll (about its forward), in the game's
// convention: right = forward x up.
static Frame head_frame(double yaw, double pitch, double roll) {
    Frame frame;
    const double forward[3] = {sin(yaw) * cos(pitch), sin(pitch), cos(yaw) * cos(pitch)};
    const double level_right[3] = {-cos(yaw), 0, sin(yaw)};  // forward x (0, 1, 0), normalised
    double level_up[3];
    pc_vr_cull_view_cross(level_right, forward, level_up);
    for (unsigned i = 0; i < 3; ++i) {
        frame.forward[i] = forward[i];
        frame.right[i] = cos(roll) * level_right[i] + sin(roll) * level_up[i];
        frame.up[i] = cos(roll) * level_up[i] - sin(roll) * level_right[i];
    }
    return frame;
}

// The two eye cameras of a head at `centre`: half the eye distance to each side, each turned outward by `cant`.
static void eye_pair(const double centre[3], const Frame *head, double half_separation, double cant,
                     PcVrHeadLookCamera eyes[2], Frame eye_frames[2]) {
    for (unsigned side = 0; side < 2; ++side) {
        const double sign = side ? 1.0 : -1.0, turn = sign * cant;
        Frame *eye = &eye_frames[side];
        for (unsigned i = 0; i < 3; ++i) {
            eye->forward[i] = cos(turn) * head->forward[i] + sin(turn) * head->right[i];
            eye->right[i] = cos(turn) * head->right[i] - sin(turn) * head->forward[i];
            eye->up[i] = head->up[i];
            eyes[side].position[i] = (float)(centre[i] + sign * half_separation * head->right[i]);
            eyes[side].target[i] = (float)(eyes[side].position[i] + 6.0 * eye->forward[i]);
            eyes[side].up[i] = (float)eye->up[i];
        }
    }
}

// Whether a world point is inside the view, with a slack for the floats the matrix is kept in.
static bool inside_view(const PcVrCullView *view, const double point[3], double slack) {
    double viewed[3];
    for (unsigned row = 0; row < 3; ++row)
        viewed[row] = view->matrix[4 * row] * point[0] + view->matrix[4 * row + 1] * point[1] +
            view->matrix[4 * row + 2] * point[2] + view->matrix[4 * row + 3];
    const double depth = -viewed[2], tangent_vertical = tan(0.5 * view->fovy);
    const double tangent_side = tangent_vertical * view->aspect;
    return depth > -slack && fabs(viewed[0]) <= depth * tangent_side + slack &&
        fabs(viewed[1]) <= depth * tangent_vertical + slack;
}

// Every sampled point of both eyes' frusta lies inside the view.
static bool holds_both_eyes(const PcVrCullView *view, const PcVrHeadLookCamera eyes[2], const Frame eye_frames[2],
                            const float fov[2][4]) {
    static const double depths[] = {0.02, 1.0, 40.0, 3000.0};
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned across = 0; across <= 8; ++across)
            for (unsigned along = 0; along <= 8; ++along)
                for (unsigned d = 0; d < sizeof depths / sizeof *depths; ++d) {
                    const double x = tan(fov[side][0] + (fov[side][1] - fov[side][0]) * across / 8.0);
                    const double y = tan(fov[side][3] + (fov[side][2] - fov[side][3]) * along / 8.0);
                    double point[3];
                    for (unsigned i = 0; i < 3; ++i)
                        point[i] = eyes[side].position[i] + depths[d] * (eye_frames[side].forward[i] +
                            x * eye_frames[side].right[i] + y * eye_frames[side].up[i]);
                    if (!inside_view(view, point, 0.02 + 2e-3 * depths[d])) return false;
                }
    return true;
}

static void test_parallel_eyes(void) {
    const double centre[3] = {-665.1, 174.0, 1582.1};
    const Frame head = head_frame(0.8487, 0, 0);  // (the bench spot's camera yaw, 48.6 degrees)
    PcVrHeadLookCamera eyes[2];
    Frame eye_frames[2];
    eye_pair(centre, &head, 0.436, 0, eyes, eye_frames);
    PcVrCullView view;
    CHECK(pc_vr_cull_view_build(eyes, link_fov, &view));
    // The larger of up and down, twice; the larger outward angle over it.
    CHECK(fabs(view.fovy - 2.0 * 0.9599) < 1e-4);
    CHECK(fabs(view.aspect - tan(0.9425) / tan(0.9599)) < 1e-4);
    // The game's layout: right, up and back rows, each with its translation.
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(fabs(view.matrix[i] - head.right[i]) < 1e-5);
        CHECK(fabs(view.matrix[4 + i] - head.up[i]) < 1e-5);
        CHECK(fabs(view.matrix[8 + i] + head.forward[i]) < 1e-5);
    }
    // Its point is midway between the eyes, pulled back by half the eye distance over the outward tangent (to the
    // precision of cameras kept in floats at coordinates in the thousands).
    const double pull_back = 0.436 / tan(0.9425);
    double point[3];
    for (unsigned i = 0; i < 3; ++i) point[i] = centre[i] - pull_back * head.forward[i];
    CHECK(fabs(view.matrix[3] + pc_vr_head_look_dot(head.right, point)) < 0.02);
    CHECK(fabs(view.matrix[7] + pc_vr_head_look_dot(head.up, point)) < 0.02);
    CHECK(fabs(view.matrix[11] - pc_vr_head_look_dot(head.forward, point)) < 0.02);
    CHECK(holds_both_eyes(&view, eyes, eye_frames, link_fov));
    // Not wider than that: a point 3 degrees outside the left eye's outward edge, far away, is outside.
    double outside[3];
    for (unsigned i = 0; i < 3; ++i)
        outside[i] = eyes[0].position[i] + 2000.0 * (head.forward[i] + tan(-0.9425 - 0.0524) * head.right[i]);
    CHECK(!inside_view(&view, outside, 0.02));
    // The game camera's own view (50 degrees, 16:9, changed by Labo: 39.7 to each side, 43 up and down) does not hold
    // the eyes: a point 50 degrees to the side is in the head view and outside the game's.
    double side_point[3];
    for (unsigned i = 0; i < 3; ++i)
        side_point[i] = centre[i] + 60.0 * (head.forward[i] + tan(0.8727) * head.right[i]);
    CHECK(inside_view(&view, side_point, 0.02));
    CHECK(tan(0.8727) > 2.0 * tan(0.4363) * (16.0 / 9.0) / 2.0);
}

static void test_turned_heads(void) {
    static const double yaws[] = {-2.9, -1.2, 0, 0.7, 1.5708, 3.0}, pitches[] = {-1.0, -0.3, 0, 0.5, 1.05};
    static const double rolls[] = {-0.5, 0, 0.35}, cants[] = {0, 0.14};
    const double centre[3] = {4210.5, 312.25, -3890.75};
    for (unsigned yaw = 0; yaw < 6; ++yaw)
        for (unsigned pitch = 0; pitch < 5; ++pitch)
            for (unsigned roll = 0; roll < 3; ++roll)
                for (unsigned cant = 0; cant < 2; ++cant) {
                    const Frame head = head_frame(yaws[yaw], pitches[pitch], rolls[roll]);
                    PcVrHeadLookCamera eyes[2];
                    Frame eye_frames[2];
                    eye_pair(centre, &head, 0.47, cants[cant], eyes, eye_frames);
                    PcVrCullView view;
                    CHECK(pc_vr_cull_view_build(eyes, link_fov, &view));
                    CHECK(holds_both_eyes(&view, eyes, eye_frames, link_fov));
                    // Eyes turned apart need a wider view than parallel ones.
                    CHECK(cants[cant] == 0 || view.fovy * view.aspect > 2.0 * 0.9599 * tan(0.9425) / tan(0.9599));
                }
}

static void test_refusals(void) {
    const double centre[3] = {10, 20, 30};
    const Frame head = head_frame(0.3, 0.1, 0);
    PcVrHeadLookCamera eyes[2], broken[2];
    Frame eye_frames[2];
    eye_pair(centre, &head, 0.44, 0, eyes, eye_frames);
    PcVrCullView view, untouched;
    memset(&untouched, 0x5a, sizeof untouched);
    // A camera that looks nowhere.
    memcpy(broken, eyes, sizeof broken);
    memcpy(broken[1].target, broken[1].position, sizeof broken[1].target);
    view = untouched;
    CHECK(!pc_vr_cull_view_build(broken, link_fov, &view) && !memcmp(&view, &untouched, sizeof view));
    // An up vector along the view.
    memcpy(broken, eyes, sizeof broken);
    for (unsigned i = 0; i < 3; ++i) broken[0].up[i] = (float)head.forward[i];
    CHECK(!pc_vr_cull_view_build(broken, link_fov, &view) && !memcmp(&view, &untouched, sizeof view));
    // A field of view that is not a frustum, or not finite, or 89 degrees and more.
    float bad_fov[2][4];
    memcpy(bad_fov, link_fov, sizeof bad_fov);
    bad_fov[0][0] = 0.8f;
    CHECK(!pc_vr_cull_view_build(eyes, bad_fov, &view) && !memcmp(&view, &untouched, sizeof view));
    memcpy(bad_fov, link_fov, sizeof bad_fov);
    bad_fov[1][2] = NAN;
    CHECK(!pc_vr_cull_view_build(eyes, bad_fov, &view));
    memcpy(bad_fov, link_fov, sizeof bad_fov);
    bad_fov[1][1] = 1.56f;
    CHECK(!pc_vr_cull_view_build(eyes, bad_fov, &view));
    // Eyes turned so far apart that a corner is 80 degrees and more from their common axis.
    eye_pair(centre, &head, 0.44, 0.6, eyes, eye_frames);
    CHECK(!pc_vr_cull_view_build(eyes, link_fov, &view) && !memcmp(&view, &untouched, sizeof view));
}

static void test_patches(void) {
    const double centre[3] = {-612.0, 189.0, 1596.0};
    const Frame head = head_frame(1.2, -0.2, 0.05);
    PcVrHeadLookCamera eyes[2];
    Frame eye_frames[2];
    eye_pair(centre, &head, 0.436, 0, eyes, eye_frames);
    PcVrCullView expected;
    CHECK(pc_vr_cull_view_build(eyes, link_fov, &expected));
    static float base_camera_storage[32], other_camera_storage[32];
    const uint64_t base = (uint64_t)(uintptr_t)base_camera_storage, other = (uint64_t)(uintptr_t)other_camera_storage;
    float pair[2] = {1.5f, 0.8889f};
    const uint64_t fovy_address = (uint64_t)(uintptr_t)&pair[0], aspect_address = (uint64_t)(uintptr_t)&pair[1];

    // The switch off: nothing is kept, and the game keeps its own matrix and field of view.
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL});
    pc_vr_cull_view_publish(base, eyes, link_fov);
    CHECK(rt_pc_vr_cull_matrix(base) == base + 8 && !guest_allocations);
    rt_pc_vr_cull_fov(base, fovy_address, aspect_address);
    CHECK(pair[0] == 1.5f && pair[1] == 0.8889f);

    // On: the game's own until a view is published.
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1", "SAVAGEWIND_PC_VR_HEAD_CULL", "1", NULL});
    CHECK(rt_pc_vr_cull_matrix(base) == base + 8 && !guest_allocations);
    pc_vr_cull_view_publish(base, eyes, link_fov);
    // The eyes' base camera gets the head view's matrix, in this thread's one guest copy.
    const uint64_t given = rt_pc_vr_cull_matrix(base);
    CHECK(given != base + 8 && guest_allocations == 1);
    CHECK(!memcmp((const void *)(uintptr_t)given, expected.matrix, sizeof expected.matrix));
    CHECK(rt_pc_vr_cull_matrix(base) == given && guest_allocations == 1);
    rt_pc_vr_cull_fov(base, fovy_address, aspect_address);
    CHECK(pair[0] == expected.fovy && pair[1] == expected.aspect);
    // Another camera keeps its own.
    pair[0] = 1.5f, pair[1] = 0.8889f;
    CHECK(rt_pc_vr_cull_matrix(other) == other + 8);
    rt_pc_vr_cull_fov(other, fovy_address, aspect_address);
    CHECK(pair[0] == 1.5f && pair[1] == 0.8889f);
    // A frame whose view cannot be built keeps the last one (D101).
    PcVrHeadLookCamera broken[2];
    memcpy(broken, eyes, sizeof broken);
    memcpy(broken[0].target, broken[0].position, sizeof broken[0].target);
    pc_vr_cull_view_publish(base, broken, link_fov);
    CHECK(rt_pc_vr_cull_matrix(base) == given);
    CHECK(!memcmp((const void *)(uintptr_t)given, expected.matrix, sizeof expected.matrix));
    // A new view replaces it: the head turned further.
    const Frame turned = head_frame(2.4, 0.3, 0);
    eye_pair(centre, &turned, 0.436, 0, eyes, eye_frames);
    PcVrCullView turned_view;
    CHECK(pc_vr_cull_view_build(eyes, link_fov, &turned_view));
    pc_vr_cull_view_publish(base, eyes, link_fov);
    CHECK(rt_pc_vr_cull_matrix(base) == given);
    CHECK(!memcmp((const void *)(uintptr_t)given, turned_view.matrix, sizeof turned_view.matrix));
    CHECK(memcmp(turned_view.matrix, expected.matrix, sizeof expected.matrix));
    // A game reset drops the view and the guest copy: the game's own again, then a new copy with the next view.
    pc_vr_cull_view_reset();
    CHECK(rt_pc_vr_cull_matrix(base) == base + 8 && guest_allocations == 1);
    pair[0] = 1.5f, pair[1] = 0.8889f;
    rt_pc_vr_cull_fov(base, fovy_address, aspect_address);
    CHECK(pair[0] == 1.5f && pair[1] == 0.8889f);
    pc_vr_cull_view_publish(base, eyes, link_fov);
    CHECK(rt_pc_vr_cull_matrix(base) != base + 8 && guest_allocations == 2);
}

int main(void) {
    test_parallel_eyes();
    test_turned_heads();
    test_refusals();
    test_patches();
    printf("PC VR cull view fixture: %u checks passed (no game data)\n", checks);
    return 0;
}
