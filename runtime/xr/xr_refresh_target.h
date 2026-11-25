// Experimental two-refresh scene cadence. One XR owner calls begin once per xrWaitFrame, hold_ready only
// for a ready world pair, and presented only after a fresh world image's successful xrEndFrame.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

typedef struct {
    int64_t display_time, period;
    uint64_t last_present;
    int elapsed;        // predicted refreshes since last_present; saturated outside the decision horizon
    int next_interval;  // 0 while learning, 2 for the usual target, 1 to recover after a late image
    bool held_previous, held_current;
} XrRefreshTarget;

static inline void xr_refresh_target_begin(XrRefreshTarget *state, int64_t display_time, int64_t period, bool usable) {
    if (!usable || display_time <= 0 || period <= 0) {
        *state = (XrRefreshTarget){0};
        return;
    }
    // Accept a single rounded display period. Duplicate, skipped or reversed predictions and rate changes
    // invalidate the old phase. The current prediction becomes the origin for the next call.
    uint64_t half = ((uint64_t)period + 1) / 2;
    uint64_t delta = display_time > state->display_time ? (uint64_t)display_time - (uint64_t)state->display_time : 0;
    if (period != state->period || delta < half || delta >= (uint64_t)period + half)
        *state = (XrRefreshTarget){0};
    else if (state->elapsed < 4)
        ++state->elapsed;
    state->display_time = display_time;
    state->period = period;
}

// Positive is early. Outside one refresh either side of the target, let the ordinary owner decide.
static inline int xr_refresh_target_distance(const XrRefreshTarget *state, uint64_t present) {
    if (!state->next_interval || present <= state->last_present || present - state->last_present != 1)
        return INT_MAX;
    int distance = state->next_interval - state->elapsed;
    return distance >= -1 && distance <= 1 ? distance : INT_MAX;
}

static inline bool xr_refresh_target_hold_ready(XrRefreshTarget *state, uint64_t present) {
    if (xr_refresh_target_distance(state, present) != 1 || state->held_previous) return false;
    state->held_current = true;
    return true;
}

static inline void xr_refresh_target_presented(XrRefreshTarget *state, uint64_t present) {
    if (!state->period) return;
    int distance = xr_refresh_target_distance(state, present);
    if (distance == INT_MAX) {
        // A first or discontinuous image seeds a new phase. Its consecutive successor establishes that
        // phase only if it was actually shown two refreshes later, without any intervention.
        bool consecutive = state->last_present && present > state->last_present && present - state->last_present == 1;
        state->next_interval = !state->next_interval && consecutive && state->elapsed == 2 ? 2 : 0;
    } else {
        // Keep the planned phase through one late display. Two consecutive late displays rephase later;
        // two consecutive early-ready images let the second through and rephase earlier at its actual display.
        state->next_interval = distance == -1 && state->next_interval != 1 ? 1 : 2;
    }
    state->held_previous = distance != INT_MAX && state->held_current;
    state->held_current = false;
    state->elapsed = 0;
    state->last_present = present;
}
