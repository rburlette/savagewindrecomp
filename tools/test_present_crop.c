#include "../runtime/nvn/present_crop.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void expect_uv(const NvnPresent *present, uint32_t width, uint32_t height,
                      float x, float y, float w, float h) {
    float uv[4];
    nvn_present_crop_uv(present, width, height, uv);
    const float expected[4] = {x, y, w, h};
    for (int i = 0; i < 4; i++) assert(fabsf(uv[i] - expected[i]) < 0.000001f);
}

int main(void) {
    const int full[4] = {0, 0, 0, 0};
    NvnPresent p = nvn_present_snapshot(NULL, full);
    expect_uv(&p, 1920, 1080, 0, 0, 1, 1);

    // The profile's handheld rectangle must sample the logical 1920x1080
    // image's upper 1280x720 region even when the host image is scaled.
    int window_crop[4] = {0, 0, 1280, 720};
    NvnPresent first = nvn_present_snapshot(NULL, window_crop);
    expect_uv(&first, 1920, 1080, 0, 0, 2.0f / 3, 2.0f / 3);
    window_crop[0] = 320;
    window_crop[1] = 180;
    window_crop[2] = 960;
    window_crop[3] = 540;
    NvnPresent second = nvn_present_snapshot(NULL, window_crop);
    expect_uv(&first, 1920, 1080, 0, 0, 2.0f / 3, 2.0f / 3);
    expect_uv(&second, 1920, 1080, 1.0f / 6, 1.0f / 6, 0.5f, 0.5f);
    // A re-present copies the first snapshot, independent of later changes.
    NvnPresent replay = first;
    expect_uv(&replay, 1920, 1080, 0, 0, 2.0f / 3, 2.0f / 3);

    const int edge[4] = {-100, 900, 500, 500};
    p = nvn_present_snapshot(NULL, edge);
    expect_uv(&p, 1920, 1080, 0, 5.0f / 6, 400.0f / 1920, 1.0f / 6);
    const int invalid[4] = {3000, 0, 100, 720};
    p = nvn_present_snapshot(NULL, invalid);
    expect_uv(&p, 1920, 1080, 0, 0, 1, 1);
    expect_uv(&p, 0, 1080, 0, 0, 1, 1);
    puts("present crop: 8 cases passed");
    return 0;
}
