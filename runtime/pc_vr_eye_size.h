// Bounded exact per-eye raster size, independent of poses/FOV and the desktop window.
#pragma once
#include <stdbool.h>

static inline bool pc_vr_parse_eye_size(const char *text, unsigned *width, unsigned *height) {
    unsigned size[2] = {0, 0};
    *width = *height = 0;
    if (!text) return false;
    for (unsigned axis = 0; axis < 2; ++axis) {
        if (*text < '1' || *text > '9') return false;
        do {
            size[axis] = size[axis] * 10 + (unsigned)(*text++ - '0');
            if (size[axis] > 4096) return false; // also bounds arithmetic before another digit
        } while (*text >= '0' && *text <= '9');
        if (size[axis] < 128 || (size[axis] & 15)) return false;
        if (axis == 0 && *text++ != 'x') return false;
    }
    if (*text) return false;
    *width = size[0];
    *height = size[1];
    return true;
}
