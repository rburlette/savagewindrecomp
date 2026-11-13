// Original AM VR lifecycle state, shared with the SevenSix sample adapter.
#pragma once
#include <stdbool.h>

// True for the standalone SAVAGEWIND_LABO_MOTION=1 diagnostic. PC VR returns false.
bool labo_motion_enabled(void);
// True only between ICommonStateGetter BeginVrMode and EndVrMode when the
// diagnostic profile or PC VR launch mode owns the lifecycle.
bool labo_motion_active(void);
// Acquire the AM state lock only while VR is active. A true return keeps the
// lock held until labo_motion_sample_release(), so EndVrMode waits for a copy.
// Do not call guest SDK code or reenter another AM state function while held.
bool labo_motion_sample_acquire(void);
void labo_motion_sample_release(void);
