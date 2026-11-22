// Authored facts exercise the exact production omission guard. Any resource,
// storage/query effect, unsupported texture binding or different shader retains GPU execution.
#include <assert.h>
#include <stdio.h>
#include "../runtime/nvn/vulkan/empty_draw.h"

int main(void) {
    const VkEmptyDrawFacts known = {.vertex = UINT64_C(0x0c24d0d42e1f913b), .fragment = UINT64_C(0x18ba4281dd29fb3e),
        .stage_mask = 3, .colors = 1, .target = {{73, 2, 32, 32, 32, 0}}};
    unsigned checks = 1;
    assert(vk_empty_draw_no_effect(&known));
#define KEEP(change) do { VkEmptyDrawFacts f = known; change; assert(!vk_empty_draw_no_effect(&f)); ++checks; } while (0)
    assert(!vk_empty_draw_no_effect(NULL)); ++checks;
    KEEP(f.vertex ^= 1); KEEP(f.fragment ^= 1);
    for (unsigned bit = 0; bit < 32; ++bit) {
        KEEP(f.storage_flags = UINT32_C(1) << bit);
        KEEP(f.stage_mask ^= UINT32_C(1) << bit);
    }
    KEEP(f.uncertain_textures = 1); KEEP(f.query_counting = 1); KEEP(f.query_active = 1);
    KEEP(f.colors = 0); KEEP(f.colors = 2);
    KEEP(f.color[0] = VK_FORMAT_R8G8B8A8_UNORM);
    KEEP(f.depth = VK_FORMAT_D32_SFLOAT); KEEP(f.stencil = VK_FORMAT_S8_UINT);
    for (unsigned i = 0; i < 6; ++i) KEEP(++f.target[0][i]);
#undef KEEP
    printf("empty draw: %u side-effect/attachment/shader/resource guard checks passed\n", checks);
    return 0;
}
