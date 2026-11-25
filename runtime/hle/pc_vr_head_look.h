#ifndef SAVAGEWIND_PC_VR_HEAD_LOOK_H
#define SAVAGEWIND_PC_VR_HEAD_LOOK_H

#include <stdbool.h>
#include <stdint.h>

// Arm before guest threads and before the camera-controller wrappers. The
// feature is exact opt-in and available only for the pinned main build.
void pc_vr_head_look_init(void);

// Lock-free: the SevenSix adapter uses this to select its identity-motion
// delivery without taking the head-look camera mutex.
bool pc_vr_head_look_enabled(void);

// Called with camera-controller x0 before its original virtual update. This
// observes only the verified controller -> weak -> actor -> DefaultCamera chain.
void pc_vr_head_look_observe_controller(uint64_t controller);

#endif
