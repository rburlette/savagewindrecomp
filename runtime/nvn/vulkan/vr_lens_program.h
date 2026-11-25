// PC VR lens mask (D114): which programs lay the game's lens polygon out about the view axis, and so take the
// headset's visible area in that layout (vr_lens_mask.h vr_lens_vertex_about_axis).
//
// Almost every draw of the polygon uses its position as the place in the eye image (21 other vertex programs at the
// roadside haze save, Link 2026-11-25). The light-shaft slices do not: their vertex program takes the position times
// twice the slice's half size (row 7 of its block: depth x tan x aspect, depth x tan, and the depth) as a point in
// front of the camera, and projects that with the block's projection (rows 0 to 3). The game draws 95 of them into
// each eye's eighth-size target and 127 into its sixteenth-size one, each testing the sun's shadow map; the far fog
// sheet is weighted by the sum, so where the slices do not reach there is no fog.
//
// The program is known by its binary, as the field haze's are (vr_field_cloud_shader.h): the 1.9.0 program's size and
// code hash. A program with another binary that does the same with the polygon is known by its translated source (the
// two products and the depth it reads from row 7), as the distance blur's read is (exec.c blur_edge_source). When
// only one of the two recognises a program, that is logged: a changed translation, or a variant not met before.
#pragma once
#include <stdbool.h>
#include <string.h>
#include "../maxwell/shader.h"
#include "../shader_archive.h"

static inline bool vr_lens_program_about_axis(const ShaderInfo *vertex, const uint8_t *code, size_t max_size) {
    if (!vertex || !vertex->msl || vertex->stage != STAGE_VERTEX || !code) return false;
    uint32_t binary_size = 0;
    const uint64_t hash = shader_code_hash(code, max_size, STAGE_VERTEX, &binary_size);
    const bool known = binary_size == 1208 && hash == UINT64_C(0x0d555e182f218012);
    const char *source = vertex->msl;
    const bool by_source = strstr(source, "in vec4 a0;") && strstr(source, "* uintBitsToFloat(cb3[7][0]) * 2.0f") &&
                           strstr(source, "* uintBitsToFloat(cb3[7][1]) * 2.0f") && strstr(source, "cb3[7][2]");
    if (known != by_source) {
        static unsigned logged;
        if (logged++ < 4)
            LOG("[pc-vr-lens] a light-shaft slice program known by its %s only (binary %u bytes, hash %016llx)\n",
                known ? "binary" : "translated source", binary_size, (unsigned long long)hash);
    }
    return known || by_source;
}
