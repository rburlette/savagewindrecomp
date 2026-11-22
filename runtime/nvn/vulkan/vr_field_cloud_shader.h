// PC VR field haze. The game's FieldHaze_Lv01..07 vertex programs take the yaw for every
// particle from the eye camera's backward axis. Give each particle its own horizontal
// direction and fade distance to the midpoint of the rendered eyes. The game still
// animates, expands, projects and shades its vertices.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../maxwell/shader.h"
#include "../shader_archive.h"

typedef enum {
    VR_FIELD_CLOUD_NONE,
    VR_FIELD_CLOUD_LEVEL_1,
    VR_FIELD_CLOUD_MOVING_4_6,
    VR_FIELD_CLOUD_MOVING_7,
    VR_FIELD_CLOUD_LEVEL_2_3
} VrFieldCloudShader;

// binary_size is the already computed maxwell_binary_size, as in the translation cache.
// Full binary identities come from the seven named emitters' BNSH program entries in
// the 1.9.0 GameResident effect archive. Levels 4, 5 and 6 share their program.
static inline VrFieldCloudShader vr_field_cloud_identify(const uint8_t *code, size_t binary_size, int stage) {
    if (stage != STAGE_VERTEX) return VR_FIELD_CLOUD_NONE;
    static const struct {
        size_t size;
        uint64_t hash;
        VrFieldCloudShader shader;
    } programs[] = {
        {13048, UINT64_C(0x48d599500769c911), VR_FIELD_CLOUD_LEVEL_1},
        {10608, UINT64_C(0x6d0cb63b83719972), VR_FIELD_CLOUD_LEVEL_2_3},
        {10904, UINT64_C(0x6c72dcf4cb61e9a2), VR_FIELD_CLOUD_LEVEL_2_3},
        {11640, UINT64_C(0xf6391beec590ce96), VR_FIELD_CLOUD_MOVING_4_6},
        {11544, UINT64_C(0xb409fde883ab909d), VR_FIELD_CLOUD_MOVING_7}
    };
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; ++i) {
        if (programs[i].size == binary_size && shader_code_hash(code, binary_size, stage, NULL) == programs[i].hash)
            return programs[i].shader;
    }
    return VR_FIELD_CLOUD_NONE;
}

#define VR_FIELD_CLOUD_VERSION "#version 460\n"
#define VR_FIELD_CLOUD_MARK "// PC VR field cloud yaw and range\n"
#define VR_FIELD_CLOUD_FADE "#define cloudFade(depth, scale, bias) (cloudRange >= 0.0 ? cloudRange + (bias) : fma(depth, scale, bias))\n"

static inline bool vr_field_cloud_patched(const ShaderInfo *info) {
    const char prefix[] = VR_FIELD_CLOUD_VERSION VR_FIELD_CLOUD_MARK;
    return info && info->msl && !strncmp(info->msl, prefix, sizeof prefix - 1);
}

// Preserve the translator's interpretation of an attribute, including its integer and
// integer-to-float ShaderKey variants. The usual particle inputs are floating point.
static inline void vr_field_cloud_attribute(char out[48], const char *source, unsigned index) {
    char declaration[32], numeric_cast[32];
    snprintf(declaration, sizeof declaration, "in vec4 a%u;", index);
    snprintf(numeric_cast, sizeof numeric_cast, "as_u(float(a%u[", index);
    const bool numeric = strstr(source, declaration) || strstr(source, numeric_cast);
    snprintf(out, 48, numeric ? "vec4(a%u)" : "uintBitsToFloat(uvec4(a%u))", index);
}

// The native tier fade subtracts its thresholds (material row 93) from the linear
// view depth of the particle centre. Select the radial range only at those calls.
// Other uses of view depth, including the fragment's soft intersection, stay native.
static inline bool vr_field_cloud_fades(char **output, const char **source, bool history, VrFieldCloudShader shader) {
    const char *depth = shader == VR_FIELD_CLOUD_LEVEL_1 ? "r29" : shader == VR_FIELD_CLOUD_LEVEL_2_3 ? "r21" :
                        shader == VR_FIELD_CLOUD_MOVING_4_6 ? "r23" : "r12";
    const char *scale = history ? "ph[pb[5] + 18][2]" : "cb8[18][2]";
    const char *threshold = "r";
    if (shader == VR_FIELD_CLOUD_LEVEL_2_3) {
        scale = "r15";
        threshold = history ? "ph[pb[6] + 93][" : "cb9[93][";
    }
    char match[120];
    snprintf(match, sizeof match, "fma(uintBitsToFloat(%s), (-uintBitsToFloat(%s)), (-uintBitsToFloat(%s", depth, scale, threshold);
    const unsigned expected = shader == VR_FIELD_CLOUD_MOVING_7 ? 1 : 2;
    for (unsigned i = 0; i < expected; ++i) {
        const char *call = strstr(*source, match);
        if (!call) return false;
        size_t count = (size_t)(call - *source);
        memcpy(*output, *source, count);
        *output += count;
        memcpy(*output, "cloudFade", sizeof "cloudFade" - 1);
        *output += sizeof "cloudFade" - 1;
        *source = call + sizeof "fma" - 1;
    }
    return !strstr(*source, match);
}

// One body for an ordinary vertex shader. A motion-vector variant also runs that body
// against the previous frame's uniforms. Edit each copy with its own camera and motion.
static inline bool vr_field_cloud_body(char **output, const char **source, const char *uniform,
                                       const char attributes[6][48], VrFieldCloudShader shader) {
    const char *first = strstr(*source, uniform);
    if (!first) return false;
    const size_t name_length = strlen(uniform);
    if (strncmp(first + name_length, "[2];\n", 5)) return false;
    const char *line = first;
    while (line > *source && line[-1] != '\n') --line;
    size_t count = (size_t)(line - *source);
    memcpy(*output, *source, count);
    *output += count;

    // For Lv01..03 a3 is the simulated local centre. The moving variants integrate
    // velocity and acceleration before calculating their yaw. At the first heading
    // read r0 contains the velocity multiplier and the three live registers below
    // contain the integrated acceleration. This includes the native drag==1 branch.
    char motion[256] = "";
    if (shader == VR_FIELD_CLOUD_MOVING_4_6 || shader == VR_FIELD_CLOUD_MOVING_7) {
        const char *acceleration = shader == VR_FIELD_CLOUD_MOVING_4_6 ? "r10, r11, r13" : "r13, r14, r15";
        snprintf(motion, sizeof motion,
                 "      localCentre += %s.w * fma(%s.xyz, vec3(uintBitsToFloat(r0)), "
                 "uintBitsToFloat(uvec3(%s)));\n", attributes[2], attributes[1], acceleration);
    }
    int added = snprintf(*output, 1024,
        "    uvec2 cloudYaw = %s.xz;\n"
        "    float cloudRange = -1.0;\n"
        "    if (%s.w == floatBitsToUint(1.0)) {\n"
        "      vec3 localCentre = %s.xyz;\n%s"
        "      vec4 point = vec4(localCentre, 1.0);\n"
        "      vec3 toEyes = uintBitsToFloat(%s.xyz) - vec3(dot(%s, point), dot(%s, point), dot(%s, point));\n"
        "      cloudYaw = floatBitsToUint(dot(toEyes.xz, toEyes.xz) > 1e-12 ? toEyes.xz : vec2(0.0, 1.0));\n"
        "      cloudRange = length(toEyes);\n"
        "    }\n", uniform, uniform, attributes[0], motion, uniform, attributes[3], attributes[4], attributes[5]);
    if (added < 0 || added >= 1024) return false;
    *output += added;

    const char *cursor = line;
    unsigned x_reads = 0, z_reads = 0;
    const char *read = first;
    while (read) {
        const char *component = read + name_length;
        if (strncmp(component, "[0]", 3) && strncmp(component, "[2]", 3)) return false;
        count = (size_t)(read - cursor);
        memcpy(*output, cursor, count);
        *output += count;
        const bool horizontal_x = component[1] == '0';
        memcpy(*output, horizontal_x ? "cloudYaw.x" : "cloudYaw.y", 10);
        *output += 10;
        x_reads += horizontal_x;
        z_reads += !horizontal_x;
        cursor = component + 3;
        read = strstr(cursor, uniform);
    }
    *source = cursor;
    return x_reads == 6 && z_reads == 2 && vr_field_cloud_fades(output, source, uniform[0] == 'p', shader);
}

// On a contract mismatch the caller can keep the native uniforms and report the
// failure. The marker travels with a successfully patched translation in the cache.
static inline bool vr_field_cloud_patch(ShaderInfo *info, VrFieldCloudShader shader) {
    if (!info || !info->msl || info->stage != STAGE_VERTEX || shader == VR_FIELD_CLOUD_NONE) return false;
    if (vr_field_cloud_patched(info)) return true;
    const char *source = info->msl;
    if (strncmp(source, VR_FIELD_CLOUD_VERSION, sizeof VR_FIELD_CLOUD_VERSION - 1)) return false;
    static const unsigned inputs[] = {3, 4, 5, 7, 8, 9};
    char attributes[6][48];
    for (unsigned i = 0; i < sizeof inputs / sizeof inputs[0]; ++i)
        vr_field_cloud_attribute(attributes[i], source, inputs[i]);
    // Reserve a centre calculation and two renamed calls per body. Keep the original
    // string intact until both bodies pass.
    const char prefix[] = VR_FIELD_CLOUD_VERSION VR_FIELD_CLOUD_MARK VR_FIELD_CLOUD_FADE;
    char *patched = malloc(strlen(source) + 2 * (1024 + 16) + sizeof prefix);
    if (!patched) return false;
    char *output = patched;
    memcpy(output, prefix, sizeof prefix - 1);
    output += sizeof prefix - 1;
    source += sizeof VR_FIELD_CLOUD_VERSION - 1;
    if ((info->mvec && !vr_field_cloud_body(&output, &source, "ph[pb[5] + 16]", attributes, shader)) ||
        !vr_field_cloud_body(&output, &source, "cb8[16]", attributes, shader)) {
        free(patched);
        return false;
    }
    strcpy(output, source);
    free(info->msl);
    info->msl = patched;
    return true;
}
