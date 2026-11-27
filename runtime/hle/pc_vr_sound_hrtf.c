// PC VR: each positional sound rendered binaurally (HRTF) from its own position, with Steam Audio (Valve, Apache 2.0;
// phonon.dll beside the executable, loaded at run time). Behind vr_sound_hrtf; it works with audio_output =
// headphones_3d, where the game mixes 5.1 and the device gets stereo.
//
// The game pans its sounds itself and only keeps their horizontal angle; the renderer the runtime runs is the game's
// own (runtime/services/audio.c), so a sound's samples exist only as a renderer voice. The path between the two, all
// 1.9.0 (docs/evidence/pc-vr-per-sound-hrtf-2026-11-27.md in the project's records):
//
//  1. On the audio library's thread, a sound's 3D component hands each emitter's position, in the listener's distance
//     and panning frames, to the listener's block. Patches around the two calls (tools/patches.py _sound_call:
//     rt_pc_vr_sound_3d_begin and _end, rt_pc_vr_sound_emitter) keep, per component, its nearest emitters in range.
//  2. After the sound's panner (rt_pc_vr_sound_panned, x0 the sound): its 3D component at +0xf8, its voice at
//     [+0x100]+8, whose +0x40 holds a cell with the client sound object (its vtable checked). Published: client -> a
//     point (its emitters within 20 degrees of each other: one direction, pc_vr_sound_directions) or a spread sound's
//     field (third-order Ambisonics, its nearest emitters' points weighted by nearness, pc_vr_sound_field), with
//     height, which the game's own panning drops; or nothing when none of its emitters is in range.
//  3. On the render thread, the renderer's command processor runs the frame's command list (pc_vr_sound_hrtf_process,
//     for audio.c's hook on CommandListProcessor::Process). The commands are looked through first: a voice's grouped
//     mix (id 0x0d) carries its node id (+0x0c), found among the engine's voices (pcvr_audio_system +0x610), whose
//     +0xe0 is its voice set, whose +0x1c0 record leads (+8, along the sound's chain of records) to the client +
//     0x2c8. The processor resumes where it stopped, so it is run in segments, each ending just before the mix of a
//     voice whose client is heard: the voice's samples are still in its buffer then. At the level of the mix's six
//     direct entries, a point goes through a binaural effect of the voice's own into a stereo bus, a spread sound
//     into the shared field (its gains ramped from last frame's, so it glides), and a sound turning from one to the
//     other fades over one frame. Those entries are then mixed at volume 0, so the voice's effect send (the reverb)
//     stays the game's.
//  4. At the device sink, the field goes through Steam Audio's Ambisonics binaural decoder, raised by SPREAD_GAIN,
//     into the bus, and the bus is added to the stereo output (pc_vr_sound_hrtf_mix).
//
// Anything that does not fit (no direction yet, no emitter in range, a voice not found, another frame size) leaves
// the voice to the game's mix. The voices no sound is found for are the game's flat streams (music, ambience beds).
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../game_sites.h"
#include "../hle.h"
#include "../pc_vr.h"
#include "../settings.h"
#include "labo_guest_memory.h"
#include "pc_vr_sound_hrtf.h"
#include "pc_vr_sound_hrtf_math.h"

// Guest layouts (1.9.0).
#define SOUND_3D_COMPONENT 0xf8     // a sound's 3D component (0: not positional)
#define SOUND_VOICE_HOLDER 0x100    // the holder of a sound's voice
#define HOLDER_VOICE 0x8
#define VOICE_CLIENT_CELL 0x40      // a cell holding the voice's client sound object
#define COMPONENT_BLOCKS 0x78       // a 3D component's per-listener blocks, listener 0's first
#define BLOCK_RANGE_SQUARED 0x208   // a block's squared range (float)
#define SYSTEM_VOICES 0x610         // the audio system's engine voices (pointers) and their count (u32)
#define SYSTEM_VOICE_COUNT 0x618
#define ENGINE_VOICE_RENDERER 0x20  // an engine voice's nn::audio::VoiceType (its info's node id at +0x14)
#define VOICE_INFO_NODE 0x14
#define ENGINE_VOICE_SET 0xe0       // the voice set it belongs to
#define SET_RECORD 0x1c0            // the voice set's record, whose +8 leads to its client + CLIENT_RECORD_LINK
#define RECORD_CLIENT_LINK 0x8      // (the next record's +8, or the client's, the last record's)
#define CLIENT_RECORD_LINK 0x2c8
#define RECORD_HOPS 16              // records followed to a client, at most
#define CLIENT_VTABLE (g_site.pcvr_sound_client_vtable + 0x10)
// The renderer's command processor and its commands (the sdk's own layouts).
#define PROCESSOR_COMMAND 0x08      // the next command to run
#define PROCESSOR_COUNT 0x1c        // u32: the commands it runs to
#define PROCESSOR_SAMPLES 0x20      // s32: samples a frame
#define PROCESSOR_BUFFERS 0x30      // the mix buffers (s32 samples)
#define PROCESSOR_INDEX 0x3c        // u32: the next command's index
#define COMMAND_FLAGS 0x04          // bit 0: enabled
#define COMMAND_ID 0x05
#define COMMAND_SIZE 0x06           // s16
#define COMMAND_NODE 0x0c
#define COMMAND_MIX_RAMP_GROUPED 0x0d
#define GROUPED_COUNT 0x20
#define GROUPED_INPUTS 0x24         // s16 x 24
#define GROUPED_PREVIOUS 0x84       // f32 x 24
#define GROUPED_CURRENT 0xe4        // f32 x 24
#define GROUPED_ENTRIES 24

#define SAMPLE_RATE 48000
#define FRAME_SAMPLES 240           // the game's renderer: 240 samples at 48 kHz
#define COMPONENT_SLOTS 512
#define CLIENT_SLOTS 512
#define NODE_SLOTS 128
#define EFFECT_SLOTS 256             // a voice channel's effects, one per direction it is heard from
#define STOP_SLOTS 64               // voices rendered binaurally in one frame, at most
#define COMMAND_LIMIT 4096
#define DIRECTION_FRAMES 40         // a published direction is used for 40 renderer frames (200 ms)
#define EFFECT_IDLE_FRAMES 400      // an effect unused for 2 s is given to another voice
#define FIELD_TAIL_FRAMES 8         // the field's decoder run on for 40 ms after its last input (its filters' tail)
// The spread sounds' level, +8 dB. A field already carries a point's energy, at or above what the game's own
// headphone mix gives the same sound, but heard around the head rather than inside it, it sounds farther and
// quieter: the user, stepping it live by 2 dB on Link, found +8 dB "great" for the water and waterfalls.
#define SPREAD_GAIN 2.51188643f

// ------------------------------------------------------------------ Steam Audio 4.8.1 (the subset used; its
// phonon.h, Copyright Valve Corporation, Apache License 2.0). Calls are __stdcall on 32-bit only.
typedef struct SteamAudioOpaque *IplHandle;
typedef struct { uint32_t version; void *log, *allocate, *release; int simd_level; int flags; } IplContextSettings;
typedef struct { int32_t sampling_rate, frame_size; } IplAudioSettings;
typedef struct { int type; const char *sofa_name; const uint8_t *sofa_data; int sofa_size; float volume; int norm; }
    IplHrtfSettings;
typedef struct { IplHandle hrtf; } IplBinauralSettings;
typedef struct { float x, y, z; } IplVector;
typedef struct { IplVector direction; int interpolation; float spatial_blend; IplHandle hrtf; float *peak_delays; }
    IplBinauralParams;
typedef struct { int32_t channels, samples; float **data; } IplAudioBuffer;
#define IPL_VERSION ((4u << 16) | (8u << 8) | 1u)
#define IPL_SIMD_AVX2 3
#define IPL_HRTF_DEFAULT 0
#define IPL_INTERPOLATION_BILINEAR 1
typedef int (*IplContextCreate)(IplContextSettings *, IplHandle *);
typedef int (*IplHrtfCreate)(IplHandle, IplAudioSettings *, IplHrtfSettings *, IplHandle *);
typedef int (*IplBinauralCreate)(IplHandle, IplAudioSettings *, IplBinauralSettings *, IplHandle *);
typedef void (*IplBinauralReset)(IplHandle);
typedef int (*IplBinauralApply)(IplHandle, IplBinauralParams *, IplAudioBuffer *, IplAudioBuffer *);
typedef struct { IplHandle hrtf; int32_t max_order; } IplFieldSettings;
typedef struct { IplHandle hrtf; int32_t order; } IplFieldParams;
typedef int (*IplFieldCreate)(IplHandle, IplAudioSettings *, IplFieldSettings *, IplHandle *);
typedef int (*IplFieldApply)(IplHandle, IplFieldParams *, IplAudioBuffer *, IplAudioBuffer *);

static struct {
    IplHandle context, hrtf;
    IplBinauralCreate binaural_create;
    IplBinauralReset binaural_reset;
    IplBinauralApply binaural_apply;
    IplHandle field_decoder;  // the Ambisonics binaural effect all spread sounds' field goes through
    IplFieldApply field_apply;
} steam;

// ------------------------------------------------------------------ the state
static _Atomic bool sound_active;  // Steam Audio loaded, its HRTF made and the sites present

// What the audio library's thread found, and what it published for the render thread (under sound_lock).
static pthread_mutex_t sound_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint64_t component; PcVrSoundEmitters emitters; } ComponentRecord;
static ComponentRecord component_records[COMPONENT_SLOTS];
// A sound heard from one direction (a point: through a binaural effect of its own), or spread over several (its field,
// into the shared Ambisonics field).
typedef struct {
    bool heard, spread;
    float direction[3], field[PC_VR_SOUND_FIELD_CHANNELS];
} SoundPlace;
typedef struct { uint64_t client; SoundPlace place; uint64_t frame; } ClientDirection;
static ClientDirection client_directions[CLIENT_SLOTS];
// The 3D update in progress on this thread: the listener block its emitters go to (0: none).
static _Thread_local struct { uint64_t block; PcVrSoundEmitters emitters; } current_update;

// The render thread's own state (only it touches these).
static _Atomic uint64_t render_frame;  // frames mixed (pc_vr_sound_hrtf_mix)
typedef struct { uint32_t node; uint64_t client; } NodeClient;
static NodeClient node_clients[NODE_SLOTS];
static unsigned node_client_count;
// A voice channel's state: its point effect, how it was rendered last frame and, spread, its field gains then (each
// frame ramps from those, so a field glides and a change between point and spread fades over one frame).
enum { VOICE_LAST_NONE, VOICE_LAST_POINT, VOICE_LAST_SPREAD };
typedef struct {
    uint32_t node;
    int16_t input;
    uint8_t last;
    uint64_t client, last_frame;
    IplHandle effect;
    float direction[3];  // as a point last frame: its direction
    float field[PC_VR_SOUND_FIELD_CHANNELS];  // spread last frame: its field
} VoiceEffect;
static VoiceEffect voice_effects[EFFECT_SLOTS];
typedef struct { uint32_t index; uint64_t command; uint32_t node; uint64_t client; SoundPlace place; } Stop;
static Stop stops[STOP_SLOTS];
static float bus[2][FRAME_SAMPLES];
static bool bus_used;
static float field_bus[PC_VR_SOUND_FIELD_CHANNELS][FRAME_SAMPLES];
static uint64_t field_frame;  // the last frame anything went into the field (its decoder's tail is let run out)

static bool read_u64(uint64_t address, uint64_t *out) { return labo_guest_copy(address, out, sizeof *out, false); }
static bool read_u32(uint64_t address, uint32_t *out) { return labo_guest_copy(address, out, sizeof *out, false); }
static unsigned slot_of(uint64_t key, unsigned slots) {
    return (unsigned)((key >> 4) * 0x9E3779B97F4A7C15ull >> 40) % slots;
}

// ------------------------------------------------------------------ the audio library's thread (the patches)
void rt_pc_vr_sound_3d_begin(uint64_t component, uint32_t listener) {
    current_update.block = 0;
    if (!atomic_load_explicit(&sound_active, memory_order_relaxed) || listener != 0) return;
    uint64_t blocks = 0;
    if (!read_u64(component + COMPONENT_BLOCKS, &blocks) || !blocks) return;
    current_update.block = blocks;
    pc_vr_sound_emitters_reset(&current_update.emitters);
}

void rt_pc_vr_sound_emitter(uint64_t block, uint64_t distance_position, uint64_t pan_position) {
    if (!current_update.block || block != current_update.block) return;
    float distance[3], pan[3], range_squared;
    if (labo_guest_copy(distance_position, distance, sizeof distance, false) &&
        labo_guest_copy(pan_position, pan, sizeof pan, false) &&
        labo_guest_copy(block + BLOCK_RANGE_SQUARED, &range_squared, sizeof range_squared, false))
        pc_vr_sound_emitters_add(&current_update.emitters, distance, pan, range_squared);
}

void rt_pc_vr_sound_3d_end(uint64_t component) {
    if (!current_update.block) return;
    pthread_mutex_lock(&sound_lock);
    ComponentRecord *record = &component_records[slot_of(component, COMPONENT_SLOTS)];
    record->component = component;
    record->emitters = current_update.emitters;
    pthread_mutex_unlock(&sound_lock);
    current_update.block = 0;
}

// The client sound object of a sound (0: not found or not one).
static uint64_t sound_client(uint64_t sound) {
    uint64_t holder = 0, voice = 0, cell = 0, client = 0, vtable = 0;
    if (!read_u64(sound + SOUND_VOICE_HOLDER, &holder) || !holder || !read_u64(holder + HOLDER_VOICE, &voice) || !voice ||
        !read_u64(voice + VOICE_CLIENT_CELL, &cell) || !cell || !read_u64(cell, &client) || !client ||
        !read_u64(client, &vtable) || vtable != CLIENT_VTABLE)
        return 0;
    return client;
}

void rt_pc_vr_sound_panned(uint64_t sound) {
    if (!atomic_load_explicit(&sound_active, memory_order_relaxed)) return;
    const uint64_t client = sound_client(sound);
    if (!client) return;
    uint64_t component = 0;
    read_u64(sound + SOUND_3D_COMPONENT, &component);
    pthread_mutex_lock(&sound_lock);
    const ComponentRecord *record = component ? &component_records[slot_of(component, COMPONENT_SLOTS)] : NULL;
    ClientDirection *published = &client_directions[slot_of(client, CLIENT_SLOTS)];
    published->client = client;
    // One direction (its emitters within 20 degrees of each other): a point. Several: the field of its emitters (or,
    // without the field's decoder, a point toward the nearest).
    PcVrSoundDirections directions = {0};
    SoundPlace *place = &published->place;
    place->heard = record && record->component == component && pc_vr_sound_directions(&record->emitters, &directions);
    place->spread = place->heard && directions.count > 1 && steam.field_decoder;
    if (place->spread) pc_vr_sound_field(&record->emitters, place->field);
    else if (place->heard) memcpy(place->direction, directions.direction[0], sizeof place->direction);
    published->frame = atomic_load_explicit(&render_frame, memory_order_relaxed);
    pthread_mutex_unlock(&sound_lock);
}

// ------------------------------------------------------------------ the render thread
// The engine voices' node ids and their clients, read once a frame.
static void node_map_refresh(void) {
    node_client_count = 0;
    const uint64_t system = g_site.pcvr_audio_system;
    uint64_t voices = 0;
    uint32_t count = 0;
    if (!read_u64(system + SYSTEM_VOICES, &voices) || !voices || !read_u32(system + SYSTEM_VOICE_COUNT, &count)) return;
    for (uint32_t index = 0; index < count && node_client_count < NODE_SLOTS; ++index) {
        uint64_t voice = 0, info = 0, set = 0, record = 0, link = 0;
        uint32_t node = 0;
        if (!read_u64(voices + 8ull * index, &voice) || !voice || !read_u64(voice + ENGINE_VOICE_RENDERER, &info) ||
            !info || !read_u32(info + VOICE_INFO_NODE, &node) || !node || !read_u64(voice + ENGINE_VOICE_SET, &set) ||
            !set || !read_u64(set + SET_RECORD, &record) || !record || !read_u64(record + RECORD_CLIENT_LINK, &link))
            continue;
        // A sound's voice sets' records are chained: a record's link is the next record's link field, and the last
        // one's is the client's. (A sound of several voices, one per channel: each voice reaches the same client.)
        for (unsigned hop = 0; hop < RECORD_HOPS && link > CLIENT_RECORD_LINK; ++hop) {
            uint64_t vtable = 0;
            read_u64(link - CLIENT_RECORD_LINK, &vtable);
            if (vtable == CLIENT_VTABLE) {
                node_clients[node_client_count++] = (NodeClient){.node = node, .client = link - CLIENT_RECORD_LINK};
                break;
            }
            if (!read_u64(link, &link)) break;
        }
    }
}

static uint64_t client_of_node(uint32_t node) {
    for (unsigned index = 0; index < node_client_count; ++index)
        if (node_clients[index].node == node) return node_clients[index].client;
    return 0;
}

// The frame's voices to render binaurally: each enabled grouped mix from the processor's next command on whose voice's
// client has a fresh direction. Returns how many.
static unsigned find_stops(uint64_t processor, uint64_t frame) {
    uint64_t command = 0;
    uint32_t index = 0, total = 0;
    if (!read_u64(processor + PROCESSOR_COMMAND, &command) || !read_u32(processor + PROCESSOR_INDEX, &index) ||
        !read_u32(processor + PROCESSOR_COUNT, &total) || total > COMMAND_LIMIT)
        return 0;
    node_map_refresh();
    unsigned found = 0;
    for (; index < total && found < STOP_SLOTS; ++index) {
        uint8_t header[0x10];
        if (!labo_guest_copy(command, header, sizeof header, false)) break;
        int16_t size;
        uint32_t node;
        memcpy(&size, header + COMMAND_SIZE, sizeof size);
        memcpy(&node, header + COMMAND_NODE, sizeof node);
        if (header[COMMAND_ID] == COMMAND_MIX_RAMP_GROUPED && (header[COMMAND_FLAGS] & 1)) {
            const uint64_t client = client_of_node(node);
            ClientDirection published = {0};
            if (client) {
                pthread_mutex_lock(&sound_lock);
                const ClientDirection *slot = &client_directions[slot_of(client, CLIENT_SLOTS)];
                if (slot->client == client) published = *slot;
                pthread_mutex_unlock(&sound_lock);
            }
            if (published.client && published.place.heard && frame - published.frame <= DIRECTION_FRAMES) {
                Stop *stop = &stops[found++];
                *stop = (Stop){.index = index, .command = command, .node = node, .client = client};
                stop->place = published.place;
            }
        }
        if (size <= 0) break;
        command += (uint64_t)size;
    }
    return found;
}

// A voice channel's state (node and input buffer), made or taken over when needed: its point effect, and how it was
// rendered last frame (nothing, when it was not rendered last frame or now plays another sound). NULL: none to be had.
static VoiceEffect *voice_state(uint32_t node, int16_t input, uint64_t client, uint64_t frame) {
    VoiceEffect *free_slot = NULL, *oldest = NULL;
    for (unsigned index = 0; index < EFFECT_SLOTS; ++index) {
        VoiceEffect *slot = &voice_effects[index];
        if (slot->effect && slot->node == node && slot->input == input) {
            if (slot->client != client || slot->last_frame + 1 != frame) slot->last = VOICE_LAST_NONE;
            slot->client = client;
            return slot;
        }
        if (!slot->effect && !free_slot) free_slot = slot;
        if (slot->effect && frame - slot->last_frame > EFFECT_IDLE_FRAMES &&
            (!oldest || slot->last_frame < oldest->last_frame))
            oldest = slot;
    }
    VoiceEffect *slot = free_slot ? free_slot : oldest;
    if (!slot) return NULL;
    if (!slot->effect) {
        IplAudioSettings audio = {SAMPLE_RATE, FRAME_SAMPLES};
        IplBinauralSettings settings = {steam.hrtf};
        if (steam.binaural_create(steam.context, &audio, &settings, &slot->effect) != 0) return NULL;
    }
    slot->node = node, slot->input = input, slot->client = client, slot->last = VOICE_LAST_NONE;
    return slot;
}

// A point: `level` through the voice's binaural effect toward `direction` into the bus, times a fade from `from` to
// `to` over the frame.
static void render_point(VoiceEffect *state, const float *level, const float direction[3], float from, float to) {
    float mono[FRAME_SAMPLES], left[FRAME_SAMPLES], right[FRAME_SAMPLES];
    for (unsigned index = 0; index < FRAME_SAMPLES; ++index)
        mono[index] = level[index] * (from + (to - from) * (float)index / FRAME_SAMPLES);
    float *in_channels[1] = {mono}, *out_channels[2] = {left, right};
    IplAudioBuffer in = {1, FRAME_SAMPLES, in_channels}, out = {2, FRAME_SAMPLES, out_channels};
    IplBinauralParams params = {{direction[0], direction[1], direction[2]}, IPL_INTERPOLATION_BILINEAR, 1.0f,
                                steam.hrtf, NULL};
    steam.binaural_apply(state->effect, &params, &in, &out);
    for (unsigned index = 0; index < FRAME_SAMPLES; ++index) {
        bus[0][index] += left[index];
        bus[1][index] += right[index];
    }
    bus_used = true;
}

// A field: `level` into the shared field, its gains ramped from `from` to `to` over the frame.
static void render_field(const float *level, const float *from, const float *to, uint64_t frame) {
    for (unsigned channel = 0; channel < PC_VR_SOUND_FIELD_CHANNELS; ++channel) {
        const float start = from[channel], step = (to[channel] - from[channel]) / FRAME_SAMPLES;
        if (start == 0 && step == 0) continue;
        for (unsigned index = 0; index < FRAME_SAMPLES; ++index)
            field_bus[channel][index] += level[index] * (start + step * (float)index);
    }
    field_frame = frame;
}

// A stop's voice, its mix's command next: its samples as a point into the bus or as a spread sound into the field,
// its direct entries then silenced.
static void render_stop(uint64_t processor, const Stop *stop, uint64_t frame) {
    uint32_t count = 0;
    int32_t samples_per_frame = 0;
    uint64_t buffers = 0;
    int16_t inputs[GROUPED_ENTRIES];
    float previous[GROUPED_ENTRIES], current[GROUPED_ENTRIES];
    const uint64_t command = stop->command;
    if (!read_u32(command + GROUPED_COUNT, &count) || count < PC_VR_SOUND_DIRECT_ENTRIES || count > GROUPED_ENTRIES ||
        !labo_guest_copy(command + GROUPED_INPUTS, inputs, sizeof inputs, false) ||
        !labo_guest_copy(command + GROUPED_PREVIOUS, previous, sizeof previous, false) ||
        !labo_guest_copy(command + GROUPED_CURRENT, current, sizeof current, false) ||
        !labo_guest_copy(processor + PROCESSOR_SAMPLES, &samples_per_frame, sizeof samples_per_frame, false) ||
        samples_per_frame != FRAME_SAMPLES || !read_u64(processor + PROCESSOR_BUFFERS, &buffers) || !buffers)
        return;
    for (unsigned entry = 1; entry < PC_VR_SOUND_DIRECT_ENTRIES; ++entry)
        if (inputs[entry] != inputs[0]) return;  // (one source buffer for the direct sound)
    const float from = pc_vr_sound_direct_level(previous, count), to = pc_vr_sound_direct_level(current, count);
    if (from == 0 && to == 0) return;
    int32_t samples[FRAME_SAMPLES];
    if (inputs[0] < 0 ||
        !labo_guest_copy(buffers + (uint64_t)inputs[0] * FRAME_SAMPLES * 4, samples, sizeof samples, false))
        return;
    VoiceEffect *state = voice_state(stop->node, inputs[0], stop->client, frame);
    if (!state) return;
    float level[FRAME_SAMPLES];
    pc_vr_sound_ramp_samples(samples, FRAME_SAMPLES, from, to, level);
    // A point through its binaural effect, a spread sound into the field. A sound turning from one to the other fades
    // over this frame: the way out from full to nothing, the way in from nothing to full (a field from its last gains
    // when it was one already, so it glides).
    const SoundPlace *place = &stop->place;
    static const float no_field[PC_VR_SOUND_FIELD_CHANNELS];
    if (!place->spread) {
        if (state->last == VOICE_LAST_SPREAD) {
            render_field(level, state->field, no_field, frame);
            steam.binaural_reset(state->effect);
            render_point(state, level, place->direction, 0, 1);
        } else {
            if (state->last == VOICE_LAST_NONE) steam.binaural_reset(state->effect);
            render_point(state, level, place->direction, 1, 1);
        }
        memcpy(state->direction, place->direction, sizeof state->direction);
        state->last = VOICE_LAST_POINT;
    } else {
        if (state->last == VOICE_LAST_POINT) {
            render_point(state, level, state->direction, 1, 0);
            render_field(level, no_field, place->field, frame);
        } else render_field(level, state->last == VOICE_LAST_SPREAD ? state->field : place->field, place->field, frame);
        memcpy(state->field, place->field, sizeof state->field);
        state->last = VOICE_LAST_SPREAD;
    }
    state->last_frame = frame;
    // The direct entries mixed at volume 0: the voice's speakers are the bus now; its send stays the game's.
    const float silent[PC_VR_SOUND_DIRECT_ENTRIES] = {0};
    labo_guest_copy(command + GROUPED_PREVIOUS, (void *)silent, sizeof silent, true);
    labo_guest_copy(command + GROUPED_CURRENT, (void *)silent, sizeof silent, true);
}

void pc_vr_sound_hrtf_process(Cpu *cpu, GuestFn process) {
    if (!atomic_load_explicit(&sound_active, memory_order_relaxed) ||
        g_settings.audio_output != AUDIO_OUT_HEADPHONES_3D) {
        process(cpu);
        return;
    }
    const uint64_t processor = cpu->x[0], link = cpu->x[30];
    const uint64_t frame = atomic_load_explicit(&render_frame, memory_order_relaxed);
    const unsigned found = find_stops(processor, frame);
    uint32_t total = 0;
    if (!found || !read_u32(processor + PROCESSOR_COUNT, &total)) {
        process(cpu);
        return;
    }
    for (unsigned stop = 0; stop < found; ++stop) {
        // The processor runs up to the voice's mix, and the voice is rendered from its buffer.
        const uint32_t until = stops[stop].index;
        uint32_t reached = 0;
        labo_guest_copy(processor + PROCESSOR_COUNT, (void *)&until, sizeof until, true);
        cpu->x[0] = processor, cpu->x[30] = link;
        process(cpu);
        if (!read_u32(processor + PROCESSOR_INDEX, &reached) || reached != until) {
            // (its time ran out: the rest of the frame is not run, as the processor itself would leave it)
            labo_guest_copy(processor + PROCESSOR_COUNT, &total, sizeof total, true);
            return;
        }
        render_stop(processor, &stops[stop], frame);
    }
    labo_guest_copy(processor + PROCESSOR_COUNT, &total, sizeof total, true);
    cpu->x[0] = processor, cpu->x[30] = link;
    process(cpu);
}

void pc_vr_sound_hrtf_mix(float *out, uint32_t count, uint32_t channels) {
    const uint64_t frame = atomic_fetch_add_explicit(&render_frame, 1, memory_order_relaxed) + 1;
    // The spread sounds' field through its binaural decoder into the bus; for a few frames after it was last used as
    // well, so the decoder's tail runs out.
    if (steam.field_decoder && frame - 1 - field_frame <= FIELD_TAIL_FRAMES) {
        float left[FRAME_SAMPLES], right[FRAME_SAMPLES];
        float *in_channels[PC_VR_SOUND_FIELD_CHANNELS], *out_channels[2] = {left, right};
        for (unsigned channel = 0; channel < PC_VR_SOUND_FIELD_CHANNELS; ++channel) in_channels[channel] = field_bus[channel];
        IplAudioBuffer in = {PC_VR_SOUND_FIELD_CHANNELS, FRAME_SAMPLES, in_channels}, out = {2, FRAME_SAMPLES, out_channels};
        IplFieldParams params = {steam.hrtf, PC_VR_SOUND_FIELD_ORDER};
        steam.field_apply(steam.field_decoder, &params, &in, &out);
        for (unsigned index = 0; index < FRAME_SAMPLES; ++index) {
            bus[0][index] += left[index] * SPREAD_GAIN;
            bus[1][index] += right[index] * SPREAD_GAIN;
        }
        bus_used = true;
        memset(field_bus, 0, sizeof field_bus);
    }
    if (!bus_used) return;
    if (channels == 2 && count == FRAME_SAMPLES) {
        for (uint32_t index = 0; index < count; ++index)
            for (unsigned side = 0; side < 2; ++side) {
                const float sum = out[index * 2 + side] + bus[side][index];
                out[index * 2 + side] = sum > 1.0f ? 1.0f : sum < -1.0f ? -1.0f : sum;
            }
    }
    memset(bus, 0, sizeof bus);
    bus_used = false;
}

// ------------------------------------------------------------------ setup
static bool steam_audio_load(void) {
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *slash = length ? wcsrchr(path, L'\\') : NULL;
    if (!slash || (size_t)(slash - path) + 12 >= MAX_PATH) return false;
    wcscpy(slash + 1, L"phonon.dll");
    HMODULE library = LoadLibraryW(path);
    if (!library) {
        LOG("[pc-vr-sound-hrtf] off: phonon.dll (Steam Audio) is not beside the executable\n");
        return false;
    }
    IplContextCreate context_create = (IplContextCreate)(void *)GetProcAddress(library, "iplContextCreate");
    IplHrtfCreate hrtf_create = (IplHrtfCreate)(void *)GetProcAddress(library, "iplHRTFCreate");
    steam.binaural_create = (IplBinauralCreate)(void *)GetProcAddress(library, "iplBinauralEffectCreate");
    steam.binaural_reset = (IplBinauralReset)(void *)GetProcAddress(library, "iplBinauralEffectReset");
    steam.binaural_apply = (IplBinauralApply)(void *)GetProcAddress(library, "iplBinauralEffectApply");
    if (!context_create || !hrtf_create || !steam.binaural_create || !steam.binaural_reset || !steam.binaural_apply) {
        LOG("[pc-vr-sound-hrtf] off: phonon.dll lacks the functions used\n");
        return false;
    }
    IplContextSettings context_settings = {.version = IPL_VERSION, .simd_level = IPL_SIMD_AVX2};
    IplAudioSettings audio = {SAMPLE_RATE, FRAME_SAMPLES};
    IplHrtfSettings hrtf_settings = {.type = IPL_HRTF_DEFAULT, .volume = 1.0f};
    if (context_create(&context_settings, &steam.context) != 0 ||
        hrtf_create(steam.context, &audio, &hrtf_settings, &steam.hrtf) != 0) {
        LOG("[pc-vr-sound-hrtf] off: Steam Audio's context or HRTF could not be made\n");
        return false;
    }
    // (the spread sounds' field and its decoder: without them a spread sound is heard from its nearest direction)
    IplFieldCreate field_create = (IplFieldCreate)(void *)GetProcAddress(library, "iplAmbisonicsBinauralEffectCreate");
    steam.field_apply = (IplFieldApply)(void *)GetProcAddress(library, "iplAmbisonicsBinauralEffectApply");
    IplFieldSettings field_settings = {steam.hrtf, PC_VR_SOUND_FIELD_ORDER};
    if (!field_create || !steam.field_apply ||
        field_create(steam.context, &audio, &field_settings, &steam.field_decoder) != 0) {
        steam.field_decoder = NULL;
        LOG("[pc-vr-sound-hrtf] the Ambisonics decoder could not be made: spread sounds heard as points\n");
    }
    return true;
#else
    LOG("[pc-vr-sound-hrtf] off: Windows only for now\n");
    return false;
#endif
}

void pc_vr_sound_hrtf_init(void) {
    if (!pc_vr_enabled() || !pc_vr_config()->settings.sound_hrtf || atomic_load(&sound_active)) return;
    // (the group's sites: with them the patches are in the game code, tools/game_sites.py)
    if (!g_site.pcvr_sound_3d_update || !g_site.pcvr_sound_emitter_sector || !g_site.pcvr_sound_panner ||
        !g_site.pcvr_audio_system || !g_site.pcvr_sound_client_vtable) {
        LOG("[pc-vr-sound-hrtf] off: no sound sites for this game version\n");
        return;
    }
    if (!steam_audio_load()) return;
    atomic_store_explicit(&sound_active, true, memory_order_release);
    LOG("[pc-vr-sound-hrtf] armed: positional sounds rendered binaurally%s\n",
        g_settings.audio_output == AUDIO_OUT_HEADPHONES_3D ? "" : " (once audio_output is headphones_3d)");
}

RT_ON_RESET(pc_vr_sound_hrtf_reset) {
    pthread_mutex_lock(&sound_lock);
    memset(component_records, 0, sizeof component_records);
    memset(client_directions, 0, sizeof client_directions);
    pthread_mutex_unlock(&sound_lock);
    node_client_count = 0;
    for (unsigned index = 0; index < EFFECT_SLOTS; ++index) voice_effects[index].node = 0, voice_effects[index].client = 0;
    memset(bus, 0, sizeof bus);
    bus_used = false;
}
