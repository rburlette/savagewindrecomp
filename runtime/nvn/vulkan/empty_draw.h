#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    uint64_t vertex, fragment;
    uint32_t stage_mask, storage_flags, uncertain_textures, query_counting, query_active, colors;
    VkFormat color[8], depth, stencil;
    uint32_t target[8][6]; // guest format/type/width/height/depth/samples
} VkEmptyDrawFacts;

// Measured unrenderable BC4 volume draw: no attachments, shader writes, queries
// or unknown stages. Omit only GPU execution; retain command/host bookkeeping.
static bool vk_empty_draw_no_effect(const VkEmptyDrawFacts *facts) {
    return facts && facts->vertex == UINT64_C(0x0c24d0d42e1f913b) && facts->fragment == UINT64_C(0x18ba4281dd29fb3e) &&
        facts->stage_mask == 3 && !facts->storage_flags && !facts->uncertain_textures && !facts->query_counting &&
        !facts->query_active && facts->colors == 1 && facts->color[0] == VK_FORMAT_UNDEFINED &&
        facts->depth == VK_FORMAT_UNDEFINED && facts->stencil == VK_FORMAT_UNDEFINED && facts->target[0][0] == 73 &&
        facts->target[0][1] == 2 && facts->target[0][2] == 32 && facts->target[0][3] == 32 &&
        facts->target[0][4] == 32 && facts->target[0][5] == 0;
}
