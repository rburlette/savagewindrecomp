#ifndef SAVAGEWIND_PC_VR_LISTENER_H
#define SAVAGEWIND_PC_VR_LISTENER_H

#include <stdint.h>
#include "pc_vr_head_look_math.h"

// Called from head look's init (before guest threads): with the game version's listener poser site present, the
// poser's calc is overridden so that the sound listener follows the head (pc_vr_listener.c).
void pc_vr_listener_init(void);

// At the frame record's root call (pc_vr_projection_trace.c), with the base camera it copied and the two eye cameras
// it built from it: the head's offset from that camera, for the listener until the next one.
void pc_vr_listener_publish(uint64_t base_camera, const PcVrHeadLookCamera *base, const PcVrHeadLookCamera eyes[2]);

#endif
