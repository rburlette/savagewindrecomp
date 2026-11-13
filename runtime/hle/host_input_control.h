#ifndef SAVAGEWIND_HOST_INPUT_CONTROL_H
#define SAVAGEWIND_HOST_INPUT_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

// Strict presented-frame controls for a finite startup script. No signs, spaces or leading zeroes.
static inline bool host_input_parse_frame(const char *text, uint64_t *frame, const char **end) {
    if (!text || *text < '1' || *text > '9') return false;
    uint64_t number = 0;
    do {
        number = number * 10 + (uint64_t)(*text++ - '0');
        if (number > 100000) return false;
    } while (*text >= '0' && *text <= '9');
    *frame = number;
    if (end) *end = text;
    return true;
}

static inline bool host_input_parse_until(const char *text, uint64_t *frame) {
    const char *end;
    return host_input_parse_frame(text, frame, &end) && !*end;
}


static inline bool host_input_control_compatible(const char *host, const char *guest,
                                                  const char *bind, const char *clock) {
    return host && *host && !guest && !bind && !clock;
}

#endif
