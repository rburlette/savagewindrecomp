// Read-only UI/world observations for opt-in host automation; never permission to save by themselves.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AUTOMATION_STATE_PAUSED = 1u << 0,
    // Existing world-manager count is nonzero. This is a loaded-world proxy, not save eligibility.
    AUTOMATION_STATE_WORLD_LOADED = 1u << 1,
    AUTOMATION_STATE_TITLE = 1u << 2,
    AUTOMATION_STATE_EVENT = 1u << 3,
    AUTOMATION_STATE_MOVIE = 1u << 4,
    // Active screens outside screen_mask's 0..63 range: callers must not assume an exhaustive mask.
    AUTOMATION_STATE_HIGH_SCREENS = 1u << 5
};

typedef struct AutomationState {
    uint64_t sampled_ns;
    uint64_t screen_mask;  // Screen objects present, not a claim that each one is visible or interactive.
    uint32_t screen_count;
    uint32_t pause_state;
    uint32_t flags;
} AutomationState;

// Strict SAVAGEWIND_QUICKSAVE=1; sampled on the existing guest VFR hook. Returns a coherent publication
// less than 500 ms old, or false with *out zeroed (disabled, invalid, reset, stale, or concurrent publication).
// Currently Windows only: other platforms fail closed until equivalent readable-memory checks exist.
// UI identities remain context-dependent: screen 0x38 is used by both SavePrompt and the title Load list.
bool automation_state_read(AutomationState *out);

#ifdef __cplusplus
}
#endif
