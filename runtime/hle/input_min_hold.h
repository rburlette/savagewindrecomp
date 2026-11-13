#ifndef SAVAGEWIND_INPUT_MIN_HOLD_H
#define SAVAGEWIND_INPUT_MIN_HOLD_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline int input_min_hold_parse_ms(const char *value) {
    if (!value || !strcmp(value, "0") || !strcmp(value, "off")) return 0;
    if (*value < '1' || *value > '9') return 0;
    unsigned parsed = 0;
    const char *cursor = value;
    while (*cursor >= '0' && *cursor <= '9' && parsed <= 100) parsed = parsed * 10 + (unsigned)(*cursor++ - '0');
    return !*cursor && parsed <= 100 ? (int)parsed : 0;
}

// UI-thread state for one physical key. The effective value alone goes through normal bindings.
typedef struct {
    bool physical;
    bool effective;
    uint64_t until_ns;
} InputMinHoldKey;

static inline void input_min_hold_down(InputMinHoldKey *key, uint64_t now_ns, uint64_t hold_ns, bool repeat) {
    if (!key->physical && !repeat) {
        key->physical = true;
        key->effective = true;
        key->until_ns = now_ns + hold_ns;
    }
}

static inline void input_min_hold_up(InputMinHoldKey *key, uint64_t now_ns) {
    if (!key->physical) return;  // stray release cannot extend or cancel a pending short press
    key->physical = false;
    if (now_ns >= key->until_ns) key->effective = false;
}

static inline bool input_min_hold_expire(InputMinHoldKey *key, uint64_t now_ns) {
    if (key->physical || !key->effective || now_ns < key->until_ns) return false;
    key->effective = false;
    return true;
}

static inline void input_min_hold_cancel(InputMinHoldKey *key) {
    key->physical = false;
    key->effective = false;
    key->until_ns = 0;
}

#endif
