// Pure bounds and visualization rules for the opt-in Labo pre-mask diagnostic.
#ifndef LABO_PREMASK_CAPTURE_H
#define LABO_PREMASK_CAPTURE_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t present, target[2];
    unsigned count;
    bool closed;
} LaboPremaskCapture;

typedef struct {
    uint64_t next, overwritten;
} LaboMaskHistoryBounds;

// Sparse depth provenance: a full depth clear starts a new epoch. Partial
// clears cannot erase earlier setup. Retain the first eight depth writes and
// count everything omitted; the renderer separately retains the last write.
enum { LABO_MASK_DEPTH_FIRST = 8 };
// vk_view(render=true) uses one mip/layer regardless of sampling counts.
// Keep this diagnostic to ordinary base views, including count=0 defaults.
static inline bool labo_mask_depth_attachment_base(bool has_view, unsigned setmask,
    unsigned level, unsigned levels, unsigned layer, unsigned layers) {
    return !has_view || (!(setmask & ~3u) &&
        (!(setmask & 1u) || (!level && levels <= 1)) &&
        (!(setmask & 2u) || (!layer && layers <= 1)));
}

typedef struct {
    uint64_t epoch, writes, partial_clears;
    bool full_clear;
} LaboMaskDepthEpoch;

static inline void labo_mask_depth_clear(LaboMaskDepthEpoch *state, bool full) {
    if (!full) {
        state->partial_clears++;
        return;
    }
    *state = (LaboMaskDepthEpoch){.epoch = state->epoch + 1, .full_clear = true};
}

static inline int labo_mask_depth_write(LaboMaskDepthEpoch *state) {
    const uint64_t index = state->writes++;
    return index < LABO_MASK_DEPTH_FIRST ? (int)index : -1;
}

static inline bool labo_mask_depth_same_frontier(const LaboMaskDepthEpoch *first,
                                                 const LaboMaskDepthEpoch *second) {
    return first->epoch == second->epoch && first->writes == second->writes &&
        first->partial_clears == second->partial_clears && first->full_clear == second->full_clear;
}

// Raw device depth visualization, not linearized scene distance. Preserve
// exact endpoints; invalid/out-of-range depth is explicitly magenta.
static inline void labo_mask_depth_rgb8(float depth, uint8_t out[4]) {
    if (!isfinite(depth) || depth < 0 || depth > 1) {
        out[0] = 255; out[1] = 0; out[2] = 255; out[3] = 255;
        return;
    }
    out[0] = out[1] = out[2] = (uint8_t)(depth * 255.0f + 0.5f);
    out[3] = 255;
}

static inline unsigned labo_mask_history_reserve(LaboMaskHistoryBounds *bounds, unsigned capacity) {
    if (!bounds || !capacity) return 0;
    const unsigned slot = (unsigned)(bounds->next % capacity);
    if (bounds->next >= capacity) bounds->overwritten++;
    bounds->next++;
    return slot;
}

static inline uint64_t labo_mask_history_first(const LaboMaskHistoryBounds *bounds, unsigned capacity) {
    return bounds && bounds->next > capacity ? bounds->next - capacity : 0;
}

static inline int labo_premask_slot(LaboPremaskCapture *capture, uint64_t present,
                                    uint64_t source, uint64_t target) {
    if (!capture || capture->closed || !source || !target || source == target) return -1;
    if (capture->count && capture->present != present) {
        capture->closed = true;
        return -1;
    }
    for (unsigned i = 0; i < capture->count; ++i) if (capture->target[i] == target) return -1;
    if (capture->count == 2) {
        capture->closed = true;
        return -1;
    }
    capture->present = present;
    capture->target[capture->count] = target;
    return (int)capture->count++;
}

// VK_FORMAT_B10G11R11_UFLOAT_PACK32: unsigned 5-bit exponent (bias 15),
// six fraction bits for R/G and five for B. Inf/NaN remain nonfinite.
static inline float labo_premask_ufloat(uint32_t bits, unsigned fraction_bits) {
    const unsigned fraction_mask = (1u << fraction_bits) - 1u;
    const unsigned fraction = bits & fraction_mask;
    const unsigned exponent = bits >> fraction_bits;
    if (exponent == 31) return fraction ? NAN : INFINITY;
    if (!exponent) return ldexpf((float)fraction, -14 - (int)fraction_bits);
    return ldexpf((float)((1u << fraction_bits) | fraction), (int)exponent - 15 - (int)fraction_bits);
}

// Diagnostic display transform, not an inverse game transfer function:
// finite HDR x maps monotonically to x/(1+x), with exact zero still black.
// Any nonfinite channel marks the whole pixel magenta, never black.
static inline void labo_premask_rgb8(uint32_t packed, uint8_t out[4]) {
    float rgb[3] = {
        labo_premask_ufloat(packed & 0x7ffu, 6),
        labo_premask_ufloat((packed >> 11) & 0x7ffu, 6),
        labo_premask_ufloat((packed >> 22) & 0x3ffu, 5),
    };
    if (!isfinite(rgb[0]) || !isfinite(rgb[1]) || !isfinite(rgb[2])) {
        out[0] = 255; out[1] = 0; out[2] = 255; out[3] = 255;
        return;
    }
    for (int i = 0; i < 3; ++i) {
        uint8_t shown = (uint8_t)(rgb[i] / (1.0f + rgb[i]) * 255.0f + 0.5f);
        out[i] = rgb[i] > 0.0f && !shown ? 1 : shown;
    }
    out[3] = 255;
}
#endif
