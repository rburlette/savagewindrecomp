// Asset-free contract for the field-cloud GLSL edit and its persisted cache marker, and for the per-draw copy of the
// game's view block that carries the eye midpoint (vr_field_cloud_view.h): the view is looked up in the game's memory,
// only an eye's draw uploads a copy, and only row 16 of that copy differs from the game's block.
#include <assert.h>
#include "../runtime/nvn/vulkan/vk.h"
#include "../runtime/nvn/vulkan/vr_field_cloud_shader.h"

// What vr_field_cloud_view.h calls: the upload ring, the descriptor of a span, the recorded eye views.
enum { VIEW_BLOCK_FLOATS = 20 * 4, DIRECTION_ROW = 16 * 4 };
static float game_block[VIEW_BLOCK_FLOATS], uploaded_block[VIEW_BLOCK_FLOATS];
static unsigned uploads, lookups;
static bool view_is_an_eyes;

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    assert(data == game_block && size <= sizeof uploaded_block && align == 256);
    memcpy(uploaded_block, data, size);
    ++uploads;
    return (VkSpan){.buf = (VkBuffer)(uintptr_t)0xb0, .off = 512, .ptr = uploaded_block};
}
static VkDescriptorBufferInfo ubo_span(VkSpan span, uint64_t size) {
    return (VkDescriptorBufferInfo){.buffer = span.buf, .offset = span.off, .range = size};
}
bool pc_vr_projection_view_midpoint(const float view[12], float midpoint[3]) {
    assert(view == game_block);  // (never the upload ring)
    ++lookups;
    if (!view_is_an_eyes) return false;
    memcpy(midpoint, (const float[3]){11, 12, 13}, 3 * sizeof *midpoint);
    return true;
}
#include "../runtime/nvn/vulkan/vr_field_cloud_view.h"

static void test_view_block(void) {
    const uint64_t address = (uint64_t)(uintptr_t)game_block;
    const VkDescriptorBufferInfo unset = {.buffer = (VkBuffer)(uintptr_t)0xee, .offset = 7, .range = 9};
    VkDescriptorBufferInfo descriptor = unset;
    for (unsigned i = 0; i < VIEW_BLOCK_FLOATS; ++i) game_block[i] = 100.0f + i;
    float game_block_before[VIEW_BLOCK_FLOATS];
    memcpy(game_block_before, game_block, sizeof game_block);

    // Neither eye's view, and a block too short for the row: the draw's ordinary path, nothing uploaded.
    assert(!vr_field_cloud_view(address, sizeof game_block, &descriptor));
    assert(lookups == 1 && uploads == 0 && !memcmp(&descriptor, &unset, sizeof descriptor));
    view_is_an_eyes = true;
    assert(!vr_field_cloud_view(address, (DIRECTION_ROW + 4) * sizeof(float) - 4, &descriptor));
    assert(lookups == 1 && uploads == 0 && !memcmp(&descriptor, &unset, sizeof descriptor));

    // An eye's view: the block ends exactly at the row, and then goes past it.
    const uint64_t sizes[] = {(DIRECTION_ROW + 4) * sizeof(float), sizeof game_block};
    for (unsigned i = 0; i < 2; ++i) {
        assert(vr_field_cloud_view(address, sizes[i], &descriptor));
        assert(uploads == i + 1 && descriptor.buffer == (VkBuffer)(uintptr_t)0xb0);
        assert(descriptor.offset == 512 && descriptor.range == sizes[i]);
        for (unsigned value = 0; value < sizes[i] / sizeof(float); ++value) {
            const unsigned column = value - DIRECTION_ROW;
            const float expected = value < DIRECTION_ROW || column > 3 ? game_block[value]
                                   : column == 3 ? 1.0f : 11.0f + column;
            assert(uploaded_block[value] == expected);
        }
    }
    assert(!memcmp(game_block, game_block_before, sizeof game_block));  // (the game's own block is never written)
}

static void append_body(char **output, bool history, unsigned x_reads, unsigned fade_calls) {
    const char *uniform = history ? "ph[pb[5] + 16]" : "cb8[16]";
    const char *scale = history ? "ph[pb[5] + 18][2]" : "cb8[18][2]";
    for (unsigned read = 0; read < x_reads + 2; ++read)
        *output += sprintf(*output, "  value = %s[%u];\n", uniform, read < 2 ? 2u : 0u);
    for (unsigned i = 0; i < fade_calls; ++i)
        *output += sprintf(*output, "  value = fma(uintBitsToFloat(r29), (-uintBitsToFloat(%s)), (-uintBitsToFloat(r%u)));\n", scale, i);
    *output += sprintf(*output, "  softDepth = fma(particleDepth, depthScale, sceneDepth);\n");
}

static ShaderInfo synthetic_shader(bool history, unsigned current_x_reads, unsigned current_fade_calls) {
    ShaderInfo info = {.stage = STAGE_VERTEX, .mvec = history};
    info.msl = malloc(2048);
    assert(info.msl);
    char *output = info.msl;
    output += sprintf(output, "#version 460\nin vec4 a3;\nin vec4 a4;\nin vec4 a5;\nin vec4 a7;\nin vec4 a8;\nin vec4 a9;\n");
    if (history) append_body(&output, true, 6, 2);
    append_body(&output, false, current_x_reads, current_fade_calls);
    return info;
}

int main(void) {
    test_view_block();
    for (unsigned history = 0; history <= 1; ++history) {
        ShaderInfo valid = synthetic_shader(history, 6, 2);
        assert(!vr_field_cloud_patched(&valid));
        assert(vr_field_cloud_patch(&valid, VR_FIELD_CLOUD_LEVEL_1));
        assert(vr_field_cloud_patched(&valid));
        // The edit starts from the native heading and substitutes the centre-relative
        // direction only when that particular uniform carries the midpoint tag.
        assert(strstr(valid.msl, "uvec2 cloudYaw = cb8[16].xz;"));
        assert(strstr(valid.msl, "if (cb8[16].w == floatBitsToUint(1.0))"));
        assert((strstr(valid.msl, "if (ph[pb[5] + 16].w == floatBitsToUint(1.0))") != NULL) == history);
        assert(!strstr(valid.msl, "cb8[16]["));
        assert(!strstr(valid.msl, "ph[pb[5] + 16]["));
        assert(strstr(valid.msl, "cloudRange = length(toEyes)"));
        assert(!strstr(valid.msl, "value = fma("));
        assert(strstr(valid.msl, "value = cloudFade("));
        assert(strstr(valid.msl, "softDepth = fma(particleDepth, depthScale, sceneDepth);"));
        char *patched = valid.msl;
        assert(vr_field_cloud_patch(&valid, VR_FIELD_CLOUD_LEVEL_1) && valid.msl == patched);
        free(valid.msl);

        // Failure in the current body must also roll back any edit of the history body.
        for (unsigned which = 0; which < 3; ++which) {
            ShaderInfo changed = synthetic_shader(history, which == 0 ? 5 : 6, which == 1 ? 1 : which == 2 ? 3 : 2);
            char before[2048];
            strcpy(before, changed.msl);
            char *original = changed.msl;
            assert(!vr_field_cloud_patch(&changed, VR_FIELD_CLOUD_LEVEL_1));
            assert(changed.msl == original && !strcmp(before, changed.msl));
            assert(!vr_field_cloud_patched(&changed));
            free(changed.msl);
        }
    }
    puts("vr_field_cloud passed");
    return 0;
}
