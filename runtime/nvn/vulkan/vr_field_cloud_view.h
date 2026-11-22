// The field haze vertex shaders face each puff toward the midpoint of the two rendered eyes.
// nn::vfx's per-view block has a direction in row 16. For a patched shader, w=1 selects the
// supplied game-space midpoint. Each draw gets its own copy so other effects can share the game's block.
#ifndef SAVAGEWIND_VR_FIELD_CLOUD_VIEW_H
#define SAVAGEWIND_VR_FIELD_CLOUD_VIEW_H
#include "../../hle/pc_vr_projection_trace.h"

// The block starts with the view the draw is made through. False: it is neither eye's, and the draw binds the game's
// block as any other does.
static bool vr_field_cloud_view(uint64_t address, uint64_t size,
                               VkDescriptorBufferInfo *descriptor) {
    const size_t row_offset = 16 * 4 * sizeof(float);
    float direction_row[4];
    if (size < row_offset + sizeof direction_row) return false;
    // Looked up in the game's own memory: the copy is in the upload ring, which is write-combined (often the graphics
    // card's own memory over the bus), where reads crawl (exec.c vk_readback).
    if (!pc_vr_projection_view_midpoint((const float *)(uintptr_t)address, direction_row)) return false;
    direction_row[3] = 1.0f;
    VkSpan copy = vk_transient_copy((const void *)(uintptr_t)address, size, 256);
    memcpy((uint8_t *)copy.ptr + row_offset, direction_row, sizeof direction_row);
    *descriptor = ubo_span(copy, size);
    static bool reported;
    if (!reported) {
        reported = true;
        LOG("[pc-vr-field-cloud] using the rendered eye midpoint for field cloud facing\n");
    }
    return true;
}

#endif
