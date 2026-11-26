// Asset-free checks of PC VR's head view for the game's visibility decisions: the view that holds both eyes' frusta
// (runtime/hle/pc_vr_cull_view_math.h), what the two game patches are answered, and a model's distance for its level
// of detail (runtime/hle/pc_vr_cull_view.c, included here with a mocked guest allocator and the fixture's own
// configuration).
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

// The game's three view volume functions, mocked: the calls are recorded, the copy copies, the two setters leave a
// mark of what they were given (the matrix's first row, the projection's parameters), and every register they may
// change is spoiled, as a real call would.
GameSites g_site;
enum { MOCK_COPY = 0x7000a00000, MOCK_VIEW = 0x7000a00100, MOCK_PROJECTION = 0x7000a00200, MOCK_PROJ_VTABLE = 0x7001d05380 };
typedef struct { uint64_t address, first, second; } GuestCall;
static GuestCall guest_calls[8];
static unsigned guest_call_count;
void guest_call(Cpu *cpu, uint64_t address) {
    CHECK(guest_call_count < 8);
    guest_calls[guest_call_count++] = (GuestCall){address, cpu->x[0], cpu->x[1]};
    uint8_t *volume = (uint8_t *)(uintptr_t)cpu->x[0];
    if (address == MOCK_COPY) memcpy(volume, (const void *)(uintptr_t)cpu->x[1], VOLUME_SIZE);
    else if (address == MOCK_VIEW) memcpy(volume, (const void *)(uintptr_t)cpu->x[1], 12 * sizeof(float));
    else if (address == MOCK_PROJECTION) {
        uint8_t *projection = (uint8_t *)(uintptr_t)cpu->x[1];
        CHECK(projection[PROJECTION_DIRTY] == 1);
        memcpy(volume + 0x100, projection + PROJECTION_FOVY, 7 * sizeof(float));
        memset(projection + PROJECTION_MATRIX, 0x11, 16 * sizeof(float));  // (the game's builder, here a mark)
        projection[PROJECTION_DIRTY] = 0;
    } else CHECK(!"a guest function the runtime has no reason to call");
    for (unsigned i = 0; i < 31; ++i) cpu->x[i] = 0xdead0000 + i;
    cpu->sp -= 0x40;
    for (unsigned i = 0; i < 32; ++i) cpu->v[i] = ~(__uint128_t)i;
}

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
    // A field of view that is not a frustum, or 89 degrees and more.
    float bad_fov[2][4];
    memcpy(bad_fov, link_fov, sizeof bad_fov);
    bad_fov[0][0] = 0.8f;
    CHECK(!pc_vr_cull_view_build(eyes, bad_fov, &view) && !memcmp(&view, &untouched, sizeof view));
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

    // The game's own matrix and field of view until a view is published.
    test_pc_vr_configure((const char *const[]){"vr", "1", NULL});
    CHECK(rt_pc_vr_cull_matrix(base) == base + 8 && !guest_allocations);
    rt_pc_vr_cull_fov(base, fovy_address, aspect_address);
    CHECK(pair[0] == 1.5f && pair[1] == 0.8889f);
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

// The grass: a grass view object whose renderer has two eye layers of the base camera and a third of another camera,
// and the left eye's layer record with its culling volume and matrices.
static void test_grass(void) {
    g_site.pcvr_volume_copy = MOCK_COPY, g_site.pcvr_volume_view = MOCK_VIEW;
    g_site.pcvr_volume_projection = MOCK_PROJECTION, g_site.pcvr_proj_vtable = MOCK_PROJ_VTABLE;
    const double centre[3] = {-612.0, 189.0, 1596.0};
    const Frame head = head_frame(0.8, 0.1, 0);
    PcVrHeadLookCamera eyes[2];
    Frame eye_frames[2];
    eye_pair(centre, &head, 0.436, 0, eyes, eye_frames);
    PcVrCullView expected;
    CHECK(pc_vr_cull_view_build(eyes, link_fov, &expected));
    static float base_camera[32], other_camera[32];
    static uint8_t layers[3][0x400], entries[3 * RENDERER_LAYER_SIZE], renderer[0x200], grass_view[0x200];
    static uint8_t record[0x620], eye_volume[0x300], eye_view[0x30], eye_projection_matrix[0x40];
    const uint64_t base = (uint64_t)(uintptr_t)base_camera;
    for (unsigned index = 0; index < 3; ++index) {
        uint8_t *layer = layers[index];
        const uint64_t layer_address = (uint64_t)(uintptr_t)layer, projection = layer_address + 0x288;
        const uint64_t camera = index == 2 ? (uint64_t)(uintptr_t)other_camera : base, vtable = MOCK_PROJ_VTABLE;
        const float near_far[2] = {0.5f, 10000.0f}, eye_parameters[7] = {1.7f, 0.75f, 0.66f, 1.13f, 0.95f, -0.12f, 0};
        memcpy(layer + LAYER_CAMERA, &camera, 8);
        memcpy(layer + LAYER_PROJECTION, &projection, 8);
        memcpy(layer + 0x288, &vtable, 8);
        memcpy(layer + 0x288 + 0x98, near_far, sizeof near_far);
        memcpy(layer + 0x288 + PROJECTION_FOVY, eye_parameters, sizeof eye_parameters);
        memcpy(entries + index * RENDERER_LAYER_SIZE + RENDERER_LAYER_OBJECT, &layer_address, 8);
    }
    const uint64_t entries_address = (uint64_t)(uintptr_t)entries, renderer_address = (uint64_t)(uintptr_t)renderer;
    const uint32_t layer_count = 3;
    memcpy(renderer + RENDERER_LAYER_COUNT, &layer_count, 4);
    memcpy(renderer + RENDERER_LAYERS, &entries_address, 8);
    memcpy(grass_view + GRASS_VIEW_RENDERER, &renderer_address, 8);
    const uint64_t volume_address = (uint64_t)(uintptr_t)eye_volume, view_address = (uint64_t)(uintptr_t)eye_view;
    const uint64_t projection_address = (uint64_t)(uintptr_t)eye_projection_matrix;
    memset(eye_volume, 0x3c, sizeof eye_volume);
    memcpy(record + 0x18, &volume_address, 8);
    memcpy(record + 0x608, &view_address, 8);
    memcpy(record + 0x610, &projection_address, 8);
    const uint64_t record_address = (uint64_t)(uintptr_t)record, grass = (uint64_t)(uintptr_t)grass_view;
    const uint16_t left_eye = 0, other_layer = 2, missing_layer = 3;
    static Cpu cpu;
    for (unsigned i = 0; i < 31; ++i) cpu.x[i] = 0x1000 + i;
    cpu.sp = 0x72ff000000;
    for (unsigned i = 0; i < 32; ++i) cpu.v[i] = (__uint128_t)i << 64 | i;
    const Cpu before = cpu;

    // Before a view is published: the record's own three, no game call.
    test_pc_vr_configure((const char *const[]){"vr", "1", NULL});
    pc_vr_cull_view_reset();
    const unsigned allocations = guest_allocations;
    memcpy(record, &left_eye, 2);
    CHECK(rt_pc_vr_cull_grass_volume(&cpu, volume_address, record_address, grass) == volume_address);
    CHECK(rt_pc_vr_cull_grass_matrix(record_address, 0, view_address) == view_address);
    CHECK(rt_pc_vr_cull_grass_matrix(record_address, 1, projection_address) == projection_address);
    CHECK(!guest_call_count && guest_allocations == allocations);
    pc_vr_cull_view_publish(base, eyes, link_fov);

    // The left eye's record: a volume of the head view, built by the three game calls in order, every register kept.
    const uint64_t first = rt_pc_vr_cull_grass_volume(&cpu, volume_address, record_address, grass);
    CHECK(first != volume_address && guest_allocations == allocations + 1);
    CHECK(!memcmp(&cpu, &before, sizeof cpu));
    const uint64_t buffer = first - GRASS_BUFFER_VOLUME;
    CHECK(guest_call_count == 3);
    CHECK(guest_calls[0].address == MOCK_COPY && guest_calls[0].first == first && guest_calls[0].second == volume_address);
    CHECK(guest_calls[1].address == MOCK_VIEW && guest_calls[1].first == first &&
          guest_calls[1].second == buffer + GRASS_BUFFER_VIEW);
    CHECK(guest_calls[2].address == MOCK_PROJECTION && guest_calls[2].first == first &&
          guest_calls[2].second == buffer + GRASS_BUFFER_PROJECTION);
    // Its view matrix is the head view's; its projection the layer's own class and planes with the head view's field
    // of view and no offset.
    CHECK(!memcmp((const void *)(uintptr_t)(buffer + GRASS_BUFFER_VIEW), expected.matrix, sizeof expected.matrix));
    CHECK(!memcmp((const void *)(uintptr_t)first, expected.matrix, sizeof expected.matrix));
    const uint8_t *projection = (const uint8_t *)(uintptr_t)(buffer + GRASS_BUFFER_PROJECTION);
    uint64_t vtable;
    float near_far[2], parameters[7];
    memcpy(&vtable, projection, 8);
    memcpy(near_far, projection + 0x98, sizeof near_far);
    memcpy(parameters, (const uint8_t *)(uintptr_t)first + 0x100, sizeof parameters);
    CHECK(vtable == MOCK_PROJ_VTABLE && near_far[0] == 0.5f && near_far[1] == 10000.0f);
    CHECK(parameters[0] == expected.fovy && parameters[4] == expected.aspect && !parameters[5] && !parameters[6]);
    CHECK(fabs(parameters[3] - tan(0.5 * expected.fovy)) < 1e-6 && fabs(parameters[1] - sin(0.5 * expected.fovy)) < 1e-6 &&
          fabs(parameters[2] - cos(0.5 * expected.fovy)) < 1e-6);
    // Everything else in the volume is the eye's own.
    CHECK(((const uint8_t *)(uintptr_t)first)[0x30] == 0x3c && ((const uint8_t *)(uintptr_t)first)[VOLUME_SIZE - 1] == 0x3c);
    // The record's two matrices follow on this thread; another record keeps its own.
    CHECK(rt_pc_vr_cull_grass_matrix(record_address, 0, view_address) == buffer + GRASS_BUFFER_VIEW);
    CHECK(rt_pc_vr_cull_grass_matrix(record_address, 1, projection_address) == buffer + GRASS_BUFFER_PROJECTION +
          PROJECTION_MATRIX);
    CHECK(rt_pc_vr_cull_grass_matrix(record_address + 0x620, 0, view_address) == view_address);

    // The next frame's region is built in the other buffer, and the one after in the first again.
    guest_call_count = 0;
    const uint64_t second = rt_pc_vr_cull_grass_volume(&cpu, volume_address, record_address, grass);
    CHECK(second != first && second - first == GRASS_BUFFER_SIZE && guest_call_count == 3);
    CHECK(rt_pc_vr_cull_grass_volume(&cpu, volume_address, record_address, grass) == first);
    CHECK(guest_allocations == allocations + 1);

    // A layer of another camera, a layer the renderer does not have, a projection of another class: the record's own,
    // and the matrices after them too.
    guest_call_count = 0;
    memcpy(record, &other_layer, 2);
    CHECK(rt_pc_vr_cull_grass_volume(&cpu, volume_address, record_address, grass) == volume_address);
    CHECK(rt_pc_vr_cull_grass_matrix(record_address, 0, view_address) == view_address);
    memcpy(record, &missing_layer, 2);
    CHECK(rt_pc_vr_cull_grass_volume(&cpu, volume_address, record_address, grass) == volume_address);
    memcpy(record, &left_eye, 2);
    const uint64_t other_vtable = 0x7001d00000;
    memcpy(layers[0] + 0x288, &other_vtable, 8);
    CHECK(rt_pc_vr_cull_grass_volume(&cpu, volume_address, record_address, grass) == volume_address);
    CHECK(rt_pc_vr_cull_grass_matrix(record_address, 1, projection_address) == projection_address);
    CHECK(!guest_call_count);
    const uint64_t vtable_back = MOCK_PROJ_VTABLE;
    memcpy(layers[0] + 0x288, &vtable_back, 8);

    // A game reset: the record's own until the next view, then new buffers.
    CHECK(rt_pc_vr_cull_grass_volume(&cpu, volume_address, record_address, grass) != volume_address);
    pc_vr_cull_view_reset();
    CHECK(rt_pc_vr_cull_grass_matrix(record_address, 0, view_address) == view_address);
    CHECK(rt_pc_vr_cull_grass_volume(&cpu, volume_address, record_address, grass) == volume_address);
    pc_vr_cull_view_publish(base, eyes, link_fov);
    CHECK(rt_pc_vr_cull_grass_volume(&cpu, volume_address, record_address, grass) != volume_address);
    CHECK(guest_allocations == allocations + 2 && !memcmp(&cpu, &before, sizeof cpu));
}

// The projection trace has the exact two eye matrices built by the game and the midpoint of their pair.
static float rendered_views[2][12], rendered_midpoint[3];
static bool rendered_views_available;

bool pc_vr_projection_view_midpoint(const float view[12], float midpoint[3]) {
    if (!rendered_views_available || (memcmp(view, rendered_views[0], sizeof rendered_views[0]) &&
                                      memcmp(view, rendered_views[1], sizeof rendered_views[1]))) return false;
    memcpy(midpoint, rendered_midpoint, sizeof rendered_midpoint);
    return true;
}

// The first member of a real volume is a float matrix. Give each fixture volume that alignment too.
enum { MODEL_VOLUME_STRIDE = (VOLUME_SIZE + sizeof(float) - 1) / sizeof(float) * sizeof(float) };
static void capture_model_views(const uint8_t volumes[2][MODEL_VOLUME_STRIDE], const double centre[3]) {
    for (unsigned side = 0; side < 2; ++side)
        memcpy(rendered_views[side], volumes[side], sizeof rendered_views[side]);
    for (unsigned axis = 0; axis < 3; ++axis) rendered_midpoint[axis] = (float)centre[axis];
    rendered_views_available = true;
}

// The game keeps its row-major 3x4 view at the start of the volume, plus a projection offset and perspective flag.
static void eye_volume(uint8_t volume[VOLUME_SIZE], const PcVrHeadLookCamera *eye, const Frame *frame, float offset_x) {
    memset(volume, 0, VOLUME_SIZE);
    float view[12] = {0};
    for (unsigned axis = 0; axis < 3; ++axis) {
        view[axis] = (float)frame->right[axis];
        view[4 + axis] = (float)frame->up[axis];
        view[8 + axis] = (float)-frame->forward[axis];
        for (unsigned row = 0; row < 3; ++row) view[4 * row + 3] -= view[4 * row + axis] * eye->position[axis];
    }
    memcpy(volume, view, sizeof view);
    memcpy(volume + VOLUME_OFFSET_X, &offset_x, sizeof offset_x);
    volume[VOLUME_PERSPECTIVE] = 1;
}

// A sphere's depth along an eye's view axis, negative in front, as the game hands it over.
static float view_depth(const PcVrHeadLookCamera *eye, const Frame *frame, const float sphere[4]) {
    double along = 0;
    for (unsigned axis = 0; axis < 3; ++axis) along += (sphere[axis] - eye->position[axis]) * frame->forward[axis];
    return (float)-along;
}

// The user's hill by the bridge (2026-11-18): 1289 units away, its change-over distance 1284. By the game's depth it
// is past that distance with the head within about 5 degrees of it and short of it with the head turned further away,
// so it swaps models; by its distance from the point between the eyes times the setting's factor it is one side of it
// at every turn, in both eyes.
static void test_model_detail(void) {
    const double centre[3] = {126.0, 123.0, 1597.0};
    const float hill_distance = 1289.0f, change_over = 1284.0f;
    static const double yaws[5] = {0.0, 0.0524, 0.1047, 0.349, 0.785};  // (straight at it; 3, 6, 20, 45 degrees away)
    static _Alignas(float) uint8_t volumes[2][MODEL_VOLUME_STRIDE], plain[MODEL_VOLUME_STRIDE];
    static const struct { const char *setting; float factor; } settings[] = {
        {NULL, 0.8f}, {"1", 1.0f}, {"0.6", 0.6f}, {"0", 0.0f}, {"3", 0.8f}};  // (3: out of range, the default)
    for (unsigned chosen = 0; chosen < sizeof settings / sizeof settings[0]; ++chosen) {
        test_pc_vr_configure((const char *const[]){"vr", "1",
            settings[chosen].setting ? "vr_model_distance" : NULL, settings[chosen].setting, NULL});
        const float factor = settings[chosen].factor;
        pc_vr_cull_view_reset();
        rendered_views_available = false;
        const Frame straight = head_frame(0.0, 0.24, 0.03);
        float sphere[4] = {0, 0, 0, 40.0f};
        for (unsigned axis = 0; axis < 3; ++axis)
            sphere[axis] = (float)(centre[axis] + hill_distance * straight.forward[axis]);
        const uint64_t sphere_address = (uint64_t)(uintptr_t)sphere;
        for (unsigned turn = 0; turn < 5; ++turn) {
            const Frame head = head_frame(yaws[turn], 0.24, 0.03);
            PcVrHeadLookCamera eyes[2];
            Frame eye_frames[2];
            eye_pair(centre, &head, 0.4527, 0, eyes, eye_frames);
            eye_volume(volumes[0], &eyes[0], &eye_frames[0], -0.1213f);
            eye_volume(volumes[1], &eyes[1], &eye_frames[1], 0.1213f);
            eye_volume(plain, &eyes[0], &eye_frames[0], 0);
            const uint64_t left_volume = (uint64_t)(uintptr_t)volumes[0], right_volume = (uint64_t)(uintptr_t)volumes[1];
            const float depths[2] = {view_depth(&eyes[0], &eye_frames[0], sphere),
                                     view_depth(&eyes[1], &eye_frames[1], sphere)};
            // The game's own measure: past the change-over distance straight on and 3 degrees away, short of it from 6.
            CHECK((-depths[0] > change_over) == (turn < 2) && (-depths[1] > change_over) == (turn < 2));
            // Until a view is published, the game's own depth.
            if (!turn) CHECK(rt_pc_vr_model_depth(left_volume, sphere_address, depths[0]) == depths[0]);
            capture_model_views(volumes, centre);
            const float left = rt_pc_vr_model_depth(left_volume, sphere_address, depths[0]);
            const float right = rt_pc_vr_model_depth(right_volume, sphere_address, depths[1]);
            if (factor > 0) {
                // Minus the distance times the factor, in both eyes and at every turn.
                CHECK(fabsf(left + factor * hill_distance) < 5e-3f && fabsf(right - left) < 5e-3f);
                // (at 1 the hill is past its change-over distance at every turn, the coarse model; at 0.8 short of it)
                CHECK((-left > change_over) == (factor == 1.0f));
            } else CHECK(left == depths[0] && right == depths[1]);  // (0: the game's own, swap and all)
            // A volume whose projection is centred is no eye's: the game's own depth.
            CHECK(rt_pc_vr_model_depth((uint64_t)(uintptr_t)plain, sphere_address, depths[0]) == depths[0]);
            // A subsequent off-centre view can reuse this exact volume address. One changed matrix element rejects it.
            const float other_translation = rendered_views[0][3] + 25.0f;
            memcpy(volumes[0] + 3 * sizeof(float), &other_translation, sizeof other_translation);
            CHECK(rt_pc_vr_model_depth(left_volume, sphere_address, depths[0]) == depths[0]);
            memcpy(volumes[0], rendered_views[0], sizeof rendered_views[0]);
            CHECK(rt_pc_vr_model_depth(left_volume, sphere_address, depths[0]) == left);
            // Nor is one that is off its centre but no perspective (a shadow's).
            volumes[0][VOLUME_PERSPECTIVE] = 0;
            CHECK(rt_pc_vr_model_depth(left_volume, sphere_address, depths[0]) == depths[0]);
        }
    }
    // A game reset: the game's own depth until the next view.
    test_pc_vr_configure((const char *const[]){"vr", "1", NULL});
    pc_vr_cull_view_reset();
    rendered_views_available = false;
    const float sphere[4] = {0, 0, 0, 1};
    CHECK(rt_pc_vr_model_depth((uint64_t)(uintptr_t)volumes[1], (uint64_t)(uintptr_t)sphere, -7.0f) == -7.0f);
}

static void test_canted_model_views(void) {
    const double centre[3] = {9, 17, 32};
    const Frame head = head_frame(0.42, 0.27, 0.09);
    PcVrHeadLookCamera eyes[2];
    Frame eye_frames[2];
    eye_pair(centre, &head, 0.45, 0.15, eyes, eye_frames);
    eyes[0].position[1] -= 0.2f;
    eyes[1].position[1] += 0.2f;
    eyes[0].target[1] -= 0.2f;
    eyes[1].target[1] += 0.2f;
    _Alignas(float) uint8_t volumes[2][MODEL_VOLUME_STRIDE];
    const float sphere[4] = {12, 21, 32, 1};  // exactly five units from the midpoint
    test_pc_vr_configure((const char *const[]){"vr", "1", "vr_model_distance", "1", NULL});
    for (unsigned side = 0; side < 2; ++side)
        eye_volume(volumes[side], &eyes[side], &eye_frames[side], side ? 0.12f : -0.12f);
    capture_model_views(volumes, centre);
    for (unsigned side = 0; side < 2; ++side) {
        const float original = view_depth(&eyes[side], &eye_frames[side], sphere);
        CHECK(rt_pc_vr_model_depth((uintptr_t)volumes[side], (uintptr_t)sphere, original) == -5.0f);
    }
    rendered_views_available = false;
    pc_vr_cull_view_reset();
}

int main(void) {
    test_model_detail();
    test_canted_model_views();
    test_parallel_eyes();
    test_turned_heads();
    test_refusals();
    test_patches();
    test_grass();
    printf("PC VR cull view fixture: %u checks passed (no game data)\n", checks);
    return 0;
}
