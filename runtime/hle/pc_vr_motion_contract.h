#ifndef SAVAGEWIND_PC_VR_MOTION_CONTRACT_H
#define SAVAGEWIND_PC_VR_MOTION_CONTRACT_H

#include "../rt.h"

// Called at import resolution (runtime/dispatch.c). Under PC VR, returns the contract's wrapper for each of the ten
// SixAxis/SevenSix motion imports; NULL for every other import and for every import outside PC VR.
HostFn pc_vr_motion_contract_lookup(const char *name);

// Keep the delivered samples' numbering and neutral calibration in step after the pinned PC VR mode-2 hook redirects
// EnableRecording to the original SDK timestamp reset (runtime/pc_vr.c).
void pc_vr_motion_contract_timestamp_reset(void);

#endif
