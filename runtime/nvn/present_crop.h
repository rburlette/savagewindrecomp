// Immutable state for one NVN window presentation. Crop coordinates are in the
// logical texture, not the host image (which may have been output-scaled).
#pragma once
#include <stdint.h>

struct NvnTexture;
typedef struct NvnPresent {
    struct NvnTexture *texture;
    int crop[4];  // x, y, width, height; zero size means the full texture
    uint64_t game_present_id;  // host-only identity; zero for UI re-presentation
} NvnPresent;

static inline NvnPresent nvn_present_snapshot(struct NvnTexture *texture, const int crop[4]) {
    NvnPresent present;
    present.texture = texture;
    for (int i = 0; i < 4; ++i) present.crop[i] = crop[i];
    present.game_present_id = 0;
    return present;
}

// The present shaders expect UV origin and extent. Clamp the guest rectangle so
// an invalid crop never samples outside the texture. Use wide arithmetic for
// x + width near INT_MAX.
static inline void nvn_present_crop_uv(const NvnPresent *present, uint32_t width, uint32_t height, float uv[4]) {
    uv[0] = uv[1] = 0.0f;
    uv[2] = uv[3] = 1.0f;
    if (!width || !height || present->crop[2] <= 0 || present->crop[3] <= 0) return;
    int64_t x0 = present->crop[0], y0 = present->crop[1];
    int64_t x1 = x0 + present->crop[2], y1 = y0 + present->crop[3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    if (x0 >= x1 || y0 >= y1) return;
    uv[0] = (float)x0 / (float)width;
    uv[1] = (float)y0 / (float)height;
    uv[2] = (float)(x1 - x0) / (float)width;
    uv[3] = (float)(y1 - y0) / (float)height;
}
