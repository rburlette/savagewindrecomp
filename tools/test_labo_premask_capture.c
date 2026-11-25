#include "../runtime/nvn/labo_premask_capture.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    LaboPremaskCapture s = {0};
    assert(labo_premask_slot(&s, 42, 0, 2) == -1);
    assert(labo_premask_slot(&s, 42, 1, 1) == -1);
    assert(labo_premask_slot(&s, 42, 1, 2) == 0);
    assert(labo_premask_slot(&s, 42, 1, 2) == -1);
    assert(labo_premask_slot(&s, 42, 3, 4) == 1);
    assert(labo_premask_slot(&s, 42, 5, 6) == -1);
    assert(s.closed);
    s = (LaboPremaskCapture){0};
    assert(labo_premask_slot(&s, 42, 1, 2) == 0);
    assert(labo_premask_slot(&s, 43, 3, 4) == -1);
    assert(s.closed);

    LaboMaskHistoryBounds ring = {0};
    assert(labo_mask_history_first(&ring, 3) == 0);
    assert(labo_mask_history_reserve(&ring, 3) == 0);
    assert(labo_mask_history_reserve(&ring, 3) == 1);
    assert(labo_mask_history_reserve(&ring, 3) == 2);
    assert(ring.overwritten == 0);
    assert(labo_mask_history_reserve(&ring, 3) == 0);
    assert(ring.overwritten == 1 && labo_mask_history_first(&ring, 3) == 1);

    LaboMaskDepthEpoch epoch = {0};
    assert(labo_mask_depth_attachment_base(false, 3, 4, 5, 6, 7));
    assert(labo_mask_depth_attachment_base(true, 3, 0, 0, 0, 1));
    assert(labo_mask_depth_attachment_base(true, 3, 0, 1, 0, 0));
    assert(!labo_mask_depth_attachment_base(true, 3, 1, 0, 0, 1));
    assert(!labo_mask_depth_attachment_base(true, 3, 0, 0, 1, 1));
    assert(!labo_mask_depth_attachment_base(true, 7, 0, 0, 0, 1));
    assert(labo_mask_depth_write(&epoch) == 0 && !epoch.full_clear);
    labo_mask_depth_clear(&epoch, false);
    assert(epoch.writes == 1 && epoch.partial_clears == 1 && !epoch.full_clear);
    labo_mask_depth_clear(&epoch, true);
    assert(epoch.epoch == 1 && epoch.full_clear && !epoch.writes && !epoch.partial_clears);
    for (int i = 0; i < LABO_MASK_DEPTH_FIRST; ++i) assert(labo_mask_depth_write(&epoch) == i);
    assert(labo_mask_depth_write(&epoch) == -1 && epoch.writes == 9);
    labo_mask_depth_clear(&epoch, false);
    assert(epoch.writes == 9 && epoch.partial_clears == 1 && epoch.epoch == 1);
    labo_mask_depth_clear(&epoch, true);
    assert(epoch.epoch == 2 && !epoch.writes && !epoch.partial_clears);
    LaboMaskDepthEpoch frontier = epoch;
    assert(labo_mask_depth_same_frontier(&epoch, &frontier));
    labo_mask_depth_write(&epoch);
    assert(!labo_mask_depth_same_frontier(&epoch, &frontier));
    frontier = epoch;
    labo_mask_depth_clear(&epoch, false);
    assert(!labo_mask_depth_same_frontier(&epoch, &frontier));
    frontier = epoch;
    labo_mask_depth_clear(&epoch, true);
    assert(!labo_mask_depth_same_frontier(&epoch, &frontier));

    uint8_t rgba[4];
    labo_mask_depth_rgb8(0, rgba);
    assert(rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);
    labo_mask_depth_rgb8(1, rgba);
    assert(rgba[0] == 255 && rgba[1] == 255 && rgba[2] == 255);
    labo_mask_depth_rgb8(0.5f, rgba);
    assert(rgba[0] == 128 && rgba[1] == 128 && rgba[2] == 128);
    labo_mask_depth_rgb8(NAN, rgba);
    assert(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 255);
    labo_mask_depth_rgb8(-0.01f, rgba);
    assert(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 255);
    labo_mask_depth_rgb8(1.01f, rgba);
    assert(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 255);
    labo_premask_rgb8(0, rgba);
    assert(rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);
    const uint32_t one = (15u << 6) | (15u << 17) | (15u << 27);
    labo_premask_rgb8(one, rgba);
    assert(rgba[0] == 128 && rgba[1] == 128 && rgba[2] == 128 && rgba[3] == 255);
    const uint32_t three = ((16u << 6) | 32u) |
        (((16u << 6) | 32u) << 11) | (((16u << 5) | 16u) << 22);
    labo_premask_rgb8(three, rgba);
    assert(rgba[0] == 191 && rgba[1] == 191 && rgba[2] == 191);
    labo_premask_rgb8(1u, rgba); // smallest positive subnormal cannot become exact black
    assert(rgba[0] == 1 && rgba[1] == 0 && rgba[2] == 0);
    labo_premask_rgb8(31u << 6, rgba); // positive infinity signals invalid HDR
    assert(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 255 && rgba[3] == 255);
    labo_premask_rgb8((31u << 6) | 1u, rgba); // NaN signals invalid HDR
    assert(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 255);
    puts("labo premask grouping, writer ring, depth epochs, and HDR/depth visualization: 53 checks passed");
}
