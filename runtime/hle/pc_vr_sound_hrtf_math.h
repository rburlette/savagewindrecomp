#ifndef SAVAGEWIND_PC_VR_SOUND_HRTF_MATH_H
#define SAVAGEWIND_PC_VR_SOUND_HRTF_MATH_H

// Pure parts of the per-sound binaural rendering (pc_vr_sound_hrtf.c): which of a sound's emitters it is heard from,
// the directions to them and their shares, and the level the game gave the sound. No guest access, no Steam Audio.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

// A voice's mix into its destination (the renderer's grouped mix command) has one entry per destination buffer; the
// game puts the direct sound in the first six (its speakers) and the effect send in the six after.
#define PC_VR_SOUND_DIRECT_ENTRIES 6
// The nearest emitters of a sound kept, and the directions they are merged into at most (to tell a sound heard from
// one direction from one spread over several).
#define PC_VR_SOUND_NEAREST 16
#define PC_VR_SOUND_DIRECTIONS 4
// A spread sound's field: third-order Ambisonics, 16 channels.
#define PC_VR_SOUND_FIELD_ORDER 3
#define PC_VR_SOUND_FIELD_CHANNELS 16
// Emitters this close in angle are heard as one direction (cosine of 20 degrees).
#define PC_VR_SOUND_MERGE_COSINE 0.9397f
// The distance (game units) below which a nearer emitter gets no larger share.
#define PC_VR_SOUND_NEAR_LIMIT 1.0f

// The emitters of one sound's 3D component seen in one update: how many are within the sound's range, and the
// nearest of them, nearest first, by their position in the listener's panning frame (the head's view: x right, y up,
// z back) and their distance.
typedef struct {
    unsigned in_range, kept;
    float distance[PC_VR_SOUND_NEAREST];
    float pan[PC_VR_SOUND_NEAREST][3];
} PcVrSoundEmitters;

static inline void pc_vr_sound_emitters_reset(PcVrSoundEmitters *emitters) {
    emitters->in_range = emitters->kept = 0;
}

// One emitter: its position in the distance frame (whose length the game measures its range by) and in the panning
// frame. Beyond range_squared, or not finite, it is not heard and not counted, as the game drops it.
static inline void pc_vr_sound_emitters_add(PcVrSoundEmitters *emitters, const float distance[3], const float pan[3],
                                            float range_squared) {
    const float distance_squared = distance[0] * distance[0] + distance[1] * distance[1] + distance[2] * distance[2];
    if (!isfinite(distance_squared) || !isfinite(pan[0]) || !isfinite(pan[1]) || !isfinite(pan[2]) ||
        !(distance_squared < range_squared)) return;
    ++emitters->in_range;
    const float length = sqrtf(distance_squared);
    unsigned place = emitters->kept;
    while (place > 0 && emitters->distance[place - 1] > length) --place;
    if (place >= PC_VR_SOUND_NEAREST) return;
    const unsigned last = emitters->kept < PC_VR_SOUND_NEAREST ? emitters->kept : PC_VR_SOUND_NEAREST - 1;
    for (unsigned index = last; index > place; --index) {
        emitters->distance[index] = emitters->distance[index - 1];
        for (unsigned axis = 0; axis < 3; ++axis) emitters->pan[index][axis] = emitters->pan[index - 1][axis];
    }
    emitters->distance[place] = length;
    for (unsigned axis = 0; axis < 3; ++axis) emitters->pan[place][axis] = pan[axis];
    if (emitters->kept < PC_VR_SOUND_NEAREST) ++emitters->kept;
}

// The unit direction from the listener to a panning-frame position, in Steam Audio's listener space (the same axes:
// x right, y up, -z ahead). False: not finite, or too close to say (then straight ahead).
static inline bool pc_vr_sound_direction(const float pan[3], float direction[3]) {
    const double length = sqrt((double)pan[0] * pan[0] + (double)pan[1] * pan[1] + (double)pan[2] * pan[2]);
    if (!isfinite(length) || length < 1e-4) {
        direction[0] = direction[1] = 0;
        direction[2] = -1;
        return false;
    }
    for (unsigned axis = 0; axis < 3; ++axis) direction[axis] = (float)(pan[axis] / length);
    return true;
}

// The directions a sound is heard from, each with its share of the sound's level (the shares' squares sum to 1, so
// the sound keeps its power).
typedef struct {
    unsigned count;
    float direction[PC_VR_SOUND_DIRECTIONS][3];
    float weight[PC_VR_SOUND_DIRECTIONS];
} PcVrSoundDirections;

// From a sound's nearest emitters: each nearer emitter weighs more (one over its distance, no more than at
// PC_VR_SOUND_NEAR_LIMIT); emitters within 20 degrees of a direction already taken join it (the direction moves to
// their weighted mean), up to four directions, and an emitter beyond those joins the closest one in angle. One
// emitter, or several on one object, give one direction; a sound spread around the listener stays spread. False:
// no emitter in range (out untouched).
static inline bool pc_vr_sound_directions(const PcVrSoundEmitters *emitters, PcVrSoundDirections *out) {
    if (!emitters->kept) return false;
    PcVrSoundDirections result = {0};
    double sums[PC_VR_SOUND_DIRECTIONS][3] = {{0}};
    for (unsigned index = 0; index < emitters->kept; ++index) {
        float direction[3];
        pc_vr_sound_direction(emitters->pan[index], direction);
        const float nearness = 1.0f / fmaxf(emitters->distance[index], PC_VR_SOUND_NEAR_LIMIT);
        unsigned target = result.count, closest = 0;
        float best = -2.0f;
        for (unsigned taken = 0; taken < result.count; ++taken) {
            const float cosine = direction[0] * result.direction[taken][0] + direction[1] * result.direction[taken][1] +
                                 direction[2] * result.direction[taken][2];
            if (cosine > best) best = cosine, closest = taken;
        }
        if (result.count && (best >= PC_VR_SOUND_MERGE_COSINE || result.count == PC_VR_SOUND_DIRECTIONS))
            target = closest;
        else ++result.count;
        result.weight[target] += nearness;
        for (unsigned axis = 0; axis < 3; ++axis) sums[target][axis] += (double)direction[axis] * nearness;
        const float mean[3] = {(float)sums[target][0], (float)sums[target][1], (float)sums[target][2]};
        pc_vr_sound_direction(mean, result.direction[target]);
    }
    double power = 0;
    for (unsigned taken = 0; taken < result.count; ++taken) power += (double)result.weight[taken] * result.weight[taken];
    const float scale = power > 0 ? (float)(1.0 / sqrt(power)) : 0;
    for (unsigned taken = 0; taken < result.count; ++taken) result.weight[taken] *= scale;
    *out = result;
    return true;
}

// A point's gains in a third-order Ambisonic field, as Steam Audio's encoder makes them (its native format: ACN
// channel order, orthonormal real spherical harmonics; Ambisonics' forward, left and up are Steam Audio's -z, -x and
// +y). `direction` a unit vector in Steam Audio's listener space. (Checked against its encoder: the fixture.)
static inline void pc_vr_sound_field_gains(const float direction[3], float gains[PC_VR_SOUND_FIELD_CHANNELS]) {
    const double x = -direction[2], y = -direction[0], z = direction[1];
    gains[0] = 0.28209479f;
    gains[1] = (float)(0.48860251 * y);
    gains[2] = (float)(0.48860251 * z);
    gains[3] = (float)(0.48860251 * x);
    gains[4] = (float)(1.09254843 * x * y);
    gains[5] = (float)(1.09254843 * y * z);
    gains[6] = (float)(0.31539157 * (3 * z * z - 1));
    gains[7] = (float)(1.09254843 * x * z);
    gains[8] = (float)(0.54627422 * (x * x - y * y));
    gains[9] = (float)(0.59004359 * y * (3 * x * x - y * y));
    gains[10] = (float)(2.89061144 * x * y * z);
    gains[11] = (float)(0.45704580 * y * (5 * z * z - 1));
    gains[12] = (float)(0.37317633 * z * (5 * z * z - 3));
    gains[13] = (float)(0.45704580 * x * (5 * z * z - 1));
    gains[14] = (float)(1.44530572 * z * (x * x - y * y));
    gains[15] = (float)(0.59004359 * x * (x * x - 3 * y * y));
}

// A point's field energy (the summed squares of its gains, the same in every direction: 16 / 4 pi), and the most a
// spread field is raised toward it (4 times, 12 dB).
#define PC_VR_SOUND_POINT_ENERGY 1.27323954f
#define PC_VR_SOUND_FIELD_MAX_GAIN 4.0f

// A spread sound's field: the sum of its nearest emitters' points, each weighing one over its distance (no more than
// at PC_VR_SOUND_NEAR_LIMIT). One signal plays from all of them, so they add as amplitudes: emitters together sound as
// one, emitters apart spread it. Nearer and farther emitters coming and going change the field a little at a time,
// without the jumps of a few fixed directions. Points apart partly cancel in the sum, so the field is then brought to
// a point's energy (up to PC_VR_SOUND_FIELD_MAX_GAIN): a spread sound is as loud as the game made it, as a point is
// (the user found the water quieter as a field). False: no emitter in range (out untouched).
static inline bool pc_vr_sound_field(const PcVrSoundEmitters *emitters, float field[PC_VR_SOUND_FIELD_CHANNELS]) {
    if (!emitters->kept) return false;
    double sums[PC_VR_SOUND_FIELD_CHANNELS] = {0};
    for (unsigned index = 0; index < emitters->kept; ++index) {
        float direction[3], gains[PC_VR_SOUND_FIELD_CHANNELS];
        pc_vr_sound_direction(emitters->pan[index], direction);
        pc_vr_sound_field_gains(direction, gains);
        const double weight = 1.0 / fmax(emitters->distance[index], PC_VR_SOUND_NEAR_LIMIT);
        for (unsigned channel = 0; channel < PC_VR_SOUND_FIELD_CHANNELS; ++channel)
            sums[channel] += weight * gains[channel];
    }
    // The field with the weights summing to 1, then raised to a point's energy. (Its omnidirectional gain is always a
    // point's, the weights all positive, so a sound spread evenly all round, its other gains cancelled, is raised by
    // at most PC_VR_SOUND_FIELD_MAX_GAIN: the cap only guards rounding.)
    double total = 0, energy = 0;
    for (unsigned index = 0; index < emitters->kept; ++index)
        total += 1.0 / fmax(emitters->distance[index], PC_VR_SOUND_NEAR_LIMIT);
    for (unsigned channel = 0; channel < PC_VR_SOUND_FIELD_CHANNELS; ++channel) {
        sums[channel] /= total;
        energy += sums[channel] * sums[channel];
    }
    const double raise = fmin(sqrt(PC_VR_SOUND_POINT_ENERGY / energy), PC_VR_SOUND_FIELD_MAX_GAIN);
    for (unsigned channel = 0; channel < PC_VR_SOUND_FIELD_CHANNELS; ++channel)
        field[channel] = (float)(sums[channel] * raise);
    return true;
}

// The level of a voice's direct sound: the root of the summed squares of its direct entries' volumes, the power the
// game's panner shared out between the speakers.
static inline float pc_vr_sound_direct_level(const float *volumes, unsigned count) {
    double power = 0;
    for (unsigned entry = 0; entry < count && entry < PC_VR_SOUND_DIRECT_ENTRIES; ++entry)
        if (isfinite(volumes[entry])) power += (double)volumes[entry] * volumes[entry];
    return (float)sqrt(power);
}

// A frame of the renderer's mix samples (32-bit, on the 16-bit scale) as floats in [-1, 1], times a level ramped from
// `from` to `to` over the frame, as the renderer ramps a mix's volume.
static inline void pc_vr_sound_ramp_samples(const int32_t *samples, unsigned count, float from, float to,
                                            float *out) {
    const float step = count ? (to - from) / (float)count : 0;
    for (unsigned index = 0; index < count; ++index)
        out[index] = (float)samples[index] * (1.0f / 32768.0f) * (from + step * (float)index);
}

#endif
