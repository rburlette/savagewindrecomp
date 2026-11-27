#ifndef SAVAGEWIND_PC_VR_SOUND_HRTF_H
#define SAVAGEWIND_PC_VR_SOUND_HRTF_H

#include <stdint.h>
#include "../cpu.h"

// Called from head look's init (before guest threads): with vr_sound_hrtf on and the game version's sites present,
// Steam Audio is loaded and the rendering armed (pc_vr_sound_hrtf.c).
void pc_vr_sound_hrtf_init(void);

// The patches around the audio library's calls (tools/patches.py _sound_call), on its thread: a sound's 3D update for
// a listener, each of its emitters' positions in the listener's frames, and the sound after its panner ran.
void rt_pc_vr_sound_3d_begin(uint64_t component, uint32_t listener);
void rt_pc_vr_sound_emitter(uint64_t block, uint64_t distance_position, uint64_t pan_position);
void rt_pc_vr_sound_3d_end(uint64_t component);
void rt_pc_vr_sound_panned(uint64_t sound);

// For the renderer's command processor (runtime/services/audio.c sdk_process), on the render thread: runs `process`
// (the recompiled CommandListProcessor::Process, cpu->x[0] the processor) over the frame's command list, in segments
// around the voices rendered binaurally; as one call of it otherwise.
void pc_vr_sound_hrtf_process(Cpu *cpu, GuestFn process);

// From the renderer's device sink (sdk_take_sink), on the render thread, after the frame's command list ran: the
// voices rendered binaurally this frame added to its output (count frames of channels samples each, interleaved; only
// a 2-channel output takes them), and the bus emptied for the next frame.
void pc_vr_sound_hrtf_mix(float *out, uint32_t count, uint32_t channels);

#endif
