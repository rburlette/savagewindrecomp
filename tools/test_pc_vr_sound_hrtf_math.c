// Pure, asset-free checks of the per-sound binaural rendering's math (runtime/hle/pc_vr_sound_hrtf_math.h).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../runtime/hle/pc_vr_sound_hrtf_math.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "sound HRTF check failed line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

static int near(double a, double b) { return fabs(a - b) < 1e-5; }

static void test_emitters(void) {
    PcVrSoundEmitters emitters;
    pc_vr_sound_emitters_reset(&emitters);
    CHECK(emitters.in_range == 0 && emitters.kept == 0);
    // one emitter in range: kept with its panning-frame position and distance
    const float near_distance[3] = {3, 0, -4}, near_pan[3] = {1, 2, -3};
    pc_vr_sound_emitters_add(&emitters, near_distance, near_pan, 100);
    CHECK(emitters.in_range == 1 && emitters.kept == 1 && near(emitters.distance[0], 5) && emitters.pan[0][1] == 2);
    // one beyond range (and one not finite): not heard, not counted
    const float far_distance[3] = {30, 0, 0}, far_pan[3] = {30, 0, 0}, broken[3] = {NAN, 0, 0};
    pc_vr_sound_emitters_add(&emitters, far_distance, far_pan, 100);
    pc_vr_sound_emitters_add(&emitters, broken, near_pan, 100);
    pc_vr_sound_emitters_add(&emitters, near_distance, broken, 100);
    CHECK(emitters.in_range == 1 && emitters.kept == 1);
    // a farther one goes after it, a nearer one before: nearest first
    const float second_distance[3] = {0, 6, 0}, second_pan[3] = {9, 9, 9};
    const float closest_distance[3] = {0, 1, 0}, closest_pan[3] = {-4, 0, 0};
    pc_vr_sound_emitters_add(&emitters, second_distance, second_pan, 100);
    pc_vr_sound_emitters_add(&emitters, closest_distance, closest_pan, 100);
    CHECK(emitters.in_range == 3 && emitters.kept == 3);
    CHECK(emitters.pan[0][0] == -4 && emitters.pan[1][0] == 1 && emitters.pan[2][0] == 9);
    CHECK(near(emitters.distance[0], 1) && near(emitters.distance[1], 5) && near(emitters.distance[2], 6));
    // more than PC_VR_SOUND_NEAREST: all counted, the nearest kept
    PcVrSoundEmitters many;
    pc_vr_sound_emitters_reset(&many);
    for (int step = PC_VR_SOUND_NEAREST + 4; step >= 1; --step) {
        const float distance[3] = {(float)step, 0, 0}, pan[3] = {(float)step, 0, 0};
        pc_vr_sound_emitters_add(&many, distance, pan, 1000);
    }
    CHECK(many.in_range == PC_VR_SOUND_NEAREST + 4 && many.kept == PC_VR_SOUND_NEAREST);
    CHECK(near(many.distance[0], 1) && near(many.distance[PC_VR_SOUND_NEAREST - 1], PC_VR_SOUND_NEAREST));
    // exactly at the range: beyond, as the game's strict comparison
    PcVrSoundEmitters edge;
    pc_vr_sound_emitters_reset(&edge);
    const float at_range[3] = {10, 0, 0};
    pc_vr_sound_emitters_add(&edge, at_range, at_range, 100);
    CHECK(edge.in_range == 0 && edge.kept == 0);
}

static void add_at(PcVrSoundEmitters *emitters, float x, float y, float z) {
    const float position[3] = {x, y, z};
    pc_vr_sound_emitters_add(emitters, position, position, 1e6f);
}

static double power_of(const PcVrSoundDirections *directions) {
    double power = 0;
    for (unsigned index = 0; index < directions->count; ++index)
        power += (double)directions->weight[index] * directions->weight[index];
    return power;
}

static void test_directions(void) {
    PcVrSoundDirections directions, untouched = {.count = 9};
    PcVrSoundEmitters emitters;
    // none in range: nothing, out untouched
    pc_vr_sound_emitters_reset(&emitters);
    directions = untouched;
    CHECK(!pc_vr_sound_directions(&emitters, &directions) && directions.count == 9);
    // one emitter: one direction at the whole level
    add_at(&emitters, 0, 0, -10);
    CHECK(pc_vr_sound_directions(&emitters, &directions) && directions.count == 1);
    CHECK(near(directions.weight[0], 1) && near(directions.direction[0][2], -1));
    // several on one object (within 20 degrees): still one direction, between them, the whole level
    pc_vr_sound_emitters_reset(&emitters);
    add_at(&emitters, 1, 0, -10);
    add_at(&emitters, -1, 0, -10);
    add_at(&emitters, 0, 1, -10);
    CHECK(pc_vr_sound_directions(&emitters, &directions) && directions.count == 1 && near(directions.weight[0], 1));
    CHECK(fabsf(directions.direction[0][0]) < 0.05f && directions.direction[0][2] < -0.99f);
    // two on opposite sides: two directions; the nearer twice as near weighs twice as much; the power kept
    pc_vr_sound_emitters_reset(&emitters);
    add_at(&emitters, 5, 0, 0);
    add_at(&emitters, -10, 0, 0);
    CHECK(pc_vr_sound_directions(&emitters, &directions) && directions.count == 2 && near(power_of(&directions), 1));
    CHECK(directions.direction[0][0] > 0.99f && directions.direction[1][0] < -0.99f);
    CHECK(near(directions.weight[0] / directions.weight[1], 2));
    // height kept (the game's own panning drops it)
    pc_vr_sound_emitters_reset(&emitters);
    add_at(&emitters, 0, 10, -10);
    CHECK(pc_vr_sound_directions(&emitters, &directions) && near(directions.direction[0][1], 0.7071068));
    // around the listener (a river, rain): at most four directions, the rest joining the closest; the power kept
    pc_vr_sound_emitters_reset(&emitters);
    for (unsigned step = 0; step < 8; ++step) {
        const double angle = step * 3.14159265358979 / 4;
        add_at(&emitters, (float)(cos(angle) * (10 + step)), 0, (float)(sin(angle) * (10 + step)));
    }
    CHECK(pc_vr_sound_directions(&emitters, &directions) && directions.count == PC_VR_SOUND_DIRECTIONS);
    CHECK(near(power_of(&directions), 1));
    // very near emitters weigh no more than one at the near limit
    pc_vr_sound_emitters_reset(&emitters);
    add_at(&emitters, 0.01f, 0, 0);
    add_at(&emitters, -PC_VR_SOUND_NEAR_LIMIT, 0, 0);
    CHECK(pc_vr_sound_directions(&emitters, &directions) && directions.count == 2);
    CHECK(near(directions.weight[0], directions.weight[1]));
}

// Steam Audio 4.8.1's own Ambisonics encoder (iplAmbisonicsEncodeEffectApply, order 3, a constant input of 1) at five
// directions, read from it on 2026-11-27: the field gains must be these.
static void test_field_gains(void) {
    static const float directions[5][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0.48f, 0.6f, -0.64f}};
    static const float steam[5][16] = {
        {0.2821f, -0.4886f, 0, 0, 0, 0, -0.3154f, 0, -0.5463f, 0.5900f, 0, 0.4570f, 0, 0, 0, 0},
        {0.2821f, 0, 0.4886f, 0, 0, 0, 0.6308f, 0, 0, 0, 0, 0, 0.7464f, 0, 0, 0},
        {0.2821f, 0, 0, 0.4886f, 0, 0, -0.3154f, 0, 0.5463f, 0, 0, 0, 0, -0.4570f, 0, 0.5900f},
        {0.2821f, 0, 0, -0.4886f, 0, 0, -0.3154f, 0, 0.5463f, 0, 0, 0, 0, 0.4570f, 0, -0.5900f},
        {0.2821f, -0.2345f, 0.2932f, 0.3127f, -0.3356f, -0.3147f, 0.0252f, 0.4195f, 0.0979f, -0.2828f, -0.5328f,
         -0.1755f, -0.2687f, 0.2340f, 0.1554f, -0.1063f}};
    for (unsigned test = 0; test < 5; ++test) {
        float gains[PC_VR_SOUND_FIELD_CHANNELS];
        pc_vr_sound_field_gains(directions[test], gains);
        for (unsigned channel = 0; channel < PC_VR_SOUND_FIELD_CHANNELS; ++channel)
            CHECK(fabsf(gains[channel] - steam[test][channel]) < 6e-5f);
    }
}

static double energy_of(const float field[PC_VR_SOUND_FIELD_CHANNELS]) {
    double energy = 0;
    for (unsigned channel = 0; channel < PC_VR_SOUND_FIELD_CHANNELS; ++channel)
        energy += (double)field[channel] * field[channel];
    return energy;
}

static void test_field(void) {
    PcVrSoundEmitters emitters;
    float field[PC_VR_SOUND_FIELD_CHANNELS], point[PC_VR_SOUND_FIELD_CHANNELS];
    pc_vr_sound_emitters_reset(&emitters);
    for (unsigned channel = 0; channel < PC_VR_SOUND_FIELD_CHANNELS; ++channel) field[channel] = 7;
    CHECK(!pc_vr_sound_field(&emitters, field) && field[0] == 7);  // (none in range: untouched)
    // one emitter: its point's gains
    add_at(&emitters, 3, 0, -4);
    const float toward[3] = {0.6f, 0, -0.8f};
    pc_vr_sound_field_gains(toward, point);
    CHECK(pc_vr_sound_field(&emitters, field));
    for (unsigned channel = 0; channel < PC_VR_SOUND_FIELD_CHANNELS; ++channel)
        CHECK(fabsf(field[channel] - point[channel]) < 1e-5f);
    // two on opposite sides, the nearer twice as near: the field leans toward the nearer (its left-right gain to the
    // omnidirectional one as a point's times 2/3 - 1/3), and has a point's energy, so the sound is as loud as one
    pc_vr_sound_emitters_reset(&emitters);
    add_at(&emitters, 5, 0, 0);
    add_at(&emitters, -10, 0, 0);
    CHECK(pc_vr_sound_field(&emitters, field));
    CHECK(fabsf(field[1] / field[0] - (-0.48860251f / 0.28209479f) * (2.0f / 3 - 1.0f / 3)) < 1e-4f);  // (left: -x)
    CHECK(near(energy_of(field), PC_VR_SOUND_POINT_ENERGY) && field[0] > 0.28209479f);
    // evenly all round (six directions, alike): every gain but the omnidirectional cancels, which is raised by the
    // most there is, 4, to a point's energy
    PcVrSoundEmitters around;
    pc_vr_sound_emitters_reset(&around);
    add_at(&around, 10, 0, 0);
    add_at(&around, -10, 0, 0);
    add_at(&around, 0, 10, 0);
    add_at(&around, 0, -10, 0);
    add_at(&around, 0, 0, 10);
    add_at(&around, 0, 0, -10);
    float diffuse[PC_VR_SOUND_FIELD_CHANNELS];
    CHECK(pc_vr_sound_field(&around, diffuse));
    CHECK(fabsf(diffuse[0] - 4 * 0.28209479f) < 1e-4f && fabsf(diffuse[1]) < 1e-5f && fabsf(diffuse[3]) < 1e-5f);
    CHECK(fabs(energy_of(diffuse) - PC_VR_SOUND_POINT_ENERGY) < 2e-3);
    // a far emitter joining shifts the field only a little
    float before[PC_VR_SOUND_FIELD_CHANNELS];
    for (unsigned channel = 0; channel < PC_VR_SOUND_FIELD_CHANNELS; ++channel) before[channel] = field[channel];
    add_at(&emitters, 0, 0, 100);
    CHECK(pc_vr_sound_field(&emitters, field));
    double change = 0;  // (the largest change of a channel's gain; a point's gains are within about 0.75)
    for (unsigned channel = 0; channel < PC_VR_SOUND_FIELD_CHANNELS; ++channel)
        change = fmax(change, fabs((double)field[channel] - before[channel]));
    CHECK(change < 0.05);
}

static void test_direction(void) {
    float direction[3];
    // ahead is -z, right +x, up +y: the listener's panning frame and Steam Audio's listener space alike
    const float ahead[3] = {0, 0, -5}, right_up[3] = {3, 4, 0};
    CHECK(pc_vr_sound_direction(ahead, direction) && near(direction[2], -1) && near(direction[0], 0));
    CHECK(pc_vr_sound_direction(right_up, direction) && near(direction[0], 0.6) && near(direction[1], 0.8));
    // a sound 10 units ahead of a head turned 60 degrees left is to the right: what the listener's frame gives
    const float turned[3] = {8.660254f, 0, -5};
    CHECK(pc_vr_sound_direction(turned, direction) && direction[0] > 0.86f && near(direction[2], -0.5));
    // too close or not finite: straight ahead, and said so
    const float origin[3] = {0, 0, 0}, broken[3] = {INFINITY, 0, 0};
    CHECK(!pc_vr_sound_direction(origin, direction) && direction[2] == -1);
    CHECK(!pc_vr_sound_direction(broken, direction) && direction[2] == -1 && direction[0] == 0);
}

static void test_level(void) {
    // the game's quad panning (front left/right, rear left/right; centre and LFE unused), then the send entries
    const float volumes[12] = {0.6f, 0.8f, 0, 0, 0, 0, 0.3f, 0.4f, 0, 0, 0, 0};
    CHECK(near(pc_vr_sound_direct_level(volumes, 12), 1.0));  // (the send entries do not count)
    const float quiet[12] = {0};
    CHECK(pc_vr_sound_direct_level(quiet, 12) == 0);
    CHECK(near(pc_vr_sound_direct_level(volumes, 1), 0.6));  // (fewer entries than six)
    const float broken[6] = {NAN, 0.5f, 0, 0, 0, 0};
    CHECK(near(pc_vr_sound_direct_level(broken, 6), 0.5));
}

static void test_ramp(void) {
    const int32_t samples[4] = {32768, 32768, -16384, 0};
    float out[4];
    pc_vr_sound_ramp_samples(samples, 4, 1.0f, 0.0f, out);
    CHECK(near(out[0], 1.0) && near(out[1], 0.75) && near(out[2], -0.25) && out[3] == 0);
    pc_vr_sound_ramp_samples(samples, 4, 0.5f, 0.5f, out);
    CHECK(near(out[0], 0.5) && near(out[2], -0.25));
}

int main(void) {
    test_emitters();
    test_directions();
    test_field_gains();
    test_field();
    test_direction();
    test_level();
    test_ramp();
    printf("PC VR sound HRTF math fixture: %u checks passed\n", checks);
    return 0;
}
