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
    {  // (development) SAVAGEWIND_PC_VR_EFFECT_VIEW_DUMP=1: the effect system's view block as the game wrote it, rows
       // 0 to 18, for the first four distinct views of a run (each eye's: what nn::vfx takes the camera to be)
        static int dump = -1;
        static unsigned dumped;
        static float last_view[12];
        if (dump < 0) dump = getenv("SAVAGEWIND_PC_VR_EFFECT_VIEW_DUMP") && atoi(getenv("SAVAGEWIND_PC_VR_EFFECT_VIEW_DUMP")) == 1;
        const float *block = (const float *)(uintptr_t)address;
        if (dump && dumped < 4 && size >= 19 * 4 * sizeof(float) && memcmp(last_view, block, sizeof last_view)) {
            memcpy(last_view, block, sizeof last_view);
            ++dumped;
            for (unsigned row = 0; row < 19; ++row)
                LOG("[pc-vr-field-cloud] view block %u row %2u: % .6f % .6f % .6f % .6f\n", dumped, row,
                    block[row * 4], block[row * 4 + 1], block[row * 4 + 2], block[row * 4 + 3]);
        }
    }
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
