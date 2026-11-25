// Authored Maxwell instructions only: verify shift/half controls survive the
// actual translator and that the resulting Vulkan compute GLSL compiles.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <shaderc/shaderc.h>
#include "../runtime/nvn/maxwell/translate.c"

int main(void) {
    const uint64_t instruction = UINT64_C(0x5cc0000000000000) | UINT64_C(4) << 39 |
        UINT64_C(3) << 20 | UINT64_C(2) << 8 | UINT64_C(7) << 16 | 1;
    const struct { uint64_t control; const char *expression; } cases[] = {
        {0, "(r2 + r3 + r4)"},
        {UINT64_C(1) << 37, "(((r2) + (r3)) >> 16u)"},
        {UINT64_C(2) << 37, "(((r2) + (r3)) << 16u)"},
        {UINT64_C(1) << 35, "(r2 & 0xFFFFu)"},
        {UINT64_C(2) << 33, "(r3 >> 16)"},
        {UINT64_C(2) << 31 | UINT64_C(1) << 49, "-(r4 >> 16)"},
        {UINT64_C(1) << 35 | UINT64_C(1) << 51 | UINT64_C(1) << 37, "-(r2 & 0xFFFFu)"},
        {UINT64_C(2) << 33 | UINT64_C(1) << 50 | UINT64_C(2) << 37, "-(r3 >> 16)"},
    };
    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    shaderc_compile_options_t options = shaderc_compile_options_initialize();
    assert(compiler && options);
    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; ++i) {
        uint64_t code[16] = {UINT64_C(0x001f8000fc0007e0), instruction | cases[i].control,
            UINT64_C(0xe30000000007000f), UINT64_C(0xe2400fffff87000f)};
        ShaderKey key = {.glsl = 1};
        ShaderInfo *shader = maxwell_translate((const uint8_t *)code, sizeof code, STAGE_COMPUTE, &key);
        assert(shader && shader->msl && !shader->error[0]);
        if (!strstr(shader->msl, cases[i].expression)) {
            fprintf(stderr, "IADD3 case %u lost expected expression: %s\n", i, cases[i].expression);
            return 1;
        }
        shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler, shader->msl, strlen(shader->msl),
            shaderc_compute_shader, "authored-iadd3", "main", options);
        if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
            fprintf(stderr, "IADD3 case %u: %s\n", i, shaderc_result_get_error_message(result));
            return 1;
        }
        shaderc_result_release(result);
        maxwell_free(shader);
    }
    shaderc_compile_options_release(options);
    shaderc_compiler_release(compiler);
    puts("IADD3: 8 authored shift/half/negation translations and Vulkan GLSL compiles passed");
    return 0;
}
