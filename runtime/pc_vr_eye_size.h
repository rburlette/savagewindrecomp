// Bounded exact per-eye raster size, independent of poses/FOV and the desktop window.
#pragma once
#include <math.h>
#include <stdbool.h>

// The eye render size for an eye scale, in the OpenXR runtime's recommended view shape (view_width x view_height):
// the pixel count of Labo's 800x900 eye targets at that scale, each axis rounded to whole pixels within 128..4096
// (EyeScale 3.0 on Link's 1824x1968: 2451x2644 for 2400x2700). The headset shows each eye image over its view's FOV,
// and the runtime recommends a size of that FOV's shape, so in this shape the pixels are square there (Labo's 8:9
// against the Quest 3's 0.927 left them 4% narrower than tall). False when the view size or the scale is unusable.
static inline bool pc_vr_eye_size_for_view(double scale, unsigned view_width, unsigned view_height,
                                           unsigned *width, unsigned *height) {
    *width = *height = 0;
    if (!(scale > 1.0 && scale <= 5.0) || !view_width || !view_height) return false;
    const double aspect = (double)view_width / (double)view_height;
    const double pixels = 800.0 * scale * 900.0 * scale;
    const double exact_height = sqrt(pixels / aspect), exact_width = exact_height * aspect;
    const double axes[2] = {exact_width, exact_height};
    unsigned rounded[2];
    for (unsigned axis = 0; axis < 2; ++axis) {
        const double whole = floor(axes[axis] + 0.5);
        rounded[axis] = whole < 128.0 ? 128u : whole > 4096.0 ? 4096u : (unsigned)whole;
    }
    *width = rounded[0];
    *height = rounded[1];
    return true;
}

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
        if (size[axis] < 128) return false;
        if (axis == 0 && *text++ != 'x') return false;
    }
    if (*text) return false;
    *width = size[0];
    *height = size[1];
    return true;
}
