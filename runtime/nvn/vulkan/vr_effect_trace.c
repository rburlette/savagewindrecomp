// PC VR renderer (development): the passes of one eye's scene (SAVAGEWIND_PC_VR_EFFECT_TRACE=1). What it logs and
// when: vr_effect_trace.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "vr_effect_trace.h"
#include "vr_shots.h"
#include "../../rt.h"
#include "../../pc_vr.h"
#include "../../hle/pc_vr_render_trace.h"

uint64_t renderer_present_count(void);  // (device.c) the presents so far

#define TRACE_LIMIT 8      // traces a run
#define TRACE_ITEMS 192    // passes and events a trace keeps
#define TRACE_TEXTURES 128  // targets a trace names
#define PASS_PROGRAMS 12   // distinct effect programs a pass names
#define PASS_SAMPLED 8     // sampled targets a pass names
#define PASS_RECTS 6       // distinct viewport and scissor rectangles a pass names

bool vr_effect_trace_running;

typedef enum { STAGE_IDLE, STAGE_FRAME, STAGE_FOLLOWING, STAGE_DONE } TraceStage;

typedef struct {
    const NvnTexture *texture;
    uint32_t width, height, format, samples;   // the guest's
    uint32_t image_width, image_height, image_layers;
    VkFormat image_format;                     // the renderer's image (0 sizes: none yet)
} TraceTexture;

typedef struct {
    bool is_event;
    // An event: its kind, its textures and how many came in a row.
    VrEffectTraceEvent event;
    uint8_t source, destination;
    uint32_t repeats;
    // A pass: its targets (texture numbers, 0 none) and its draws.
    uint8_t colors[8], color_count, depth;
    uint32_t first_draw, draws, eye_draws, other_eye_draws, uncertified_draws, blended, depth_tested, depth_written;
    uint32_t effect_draws, effect_haze, effect_blended, effect_depth_written, haze_untagged;
    uint64_t programs[PASS_PROGRAMS];
    uint32_t program_draws[PASS_PROGRAMS];
    uint8_t program_count;
    bool programs_overflow, holds_final;
    uint8_t sampled[PASS_SAMPLED], sampled_count;
    // Its draws' viewport and scissor rectangles (the game's units), and the size they are measured against: the first
    // colour target's guest size, or the depth target's.
    struct { float viewport[4]; int32_t scissor[4]; uint32_t draws, effect_draws; } rects[PASS_RECTS];
    uint8_t rect_count;
    bool rects_overflow;
    uint32_t target_width, target_height;
} TraceItem;

static struct {
    int on;  // -1 not read yet
    unsigned requests_seen, traces;
    TraceStage stage;
    uint64_t frame;  // renderer_present_count() at the traced frame's start
    // The whole frame: effect-tagged draws by eye (0 uncertified, 1 left, 2 right), the field haze's draws.
    uint32_t effect_by_eye[3], haze_draws, haze_tagged, draws;
    // The left eye's passes and events.
    TraceItem items[TRACE_ITEMS];
    unsigned item_count;
    bool items_overflow;
    uint32_t eye_sequence_draws;
    TraceTexture textures[TRACE_TEXTURES];
    unsigned texture_count;
    // The last certificate looked up and its eye (-1: none).
    uint64_t certificate;
    int certificate_eye;
    // The depth targets the followed eye's effects tested against and the render targets they sampled, each saved once
    // a trace (what they cover).
    const NvnTexture *saved[8];
    unsigned saved_count;
    // The colour targets the followed eye's effects drew into: each is saved when a draw first reads it (the game's
    // composite of an effects buffer, complete by then).
    const NvnTexture *effect_targets[8];
    unsigned effect_target_count;
} trace = {.on = -1, .certificate_eye = -1};

static uint8_t texture_number(const NvnTexture *texture);

// Saves a texture the effects use, the first time in this trace (vr_shots.h vr_shot_save_now), ending the render pass.
static void save_once(const NvnTexture *texture, const char *role) {
    if (!texture) return;
    for (unsigned i = 0; i < trace.saved_count; ++i)
        if (trace.saved[i] == texture) return;
    if (trace.saved_count >= 8) return;
    trace.saved[trace.saved_count++] = texture;
    char name[64];
    const unsigned number = texture_number(texture);
    snprintf(name, sizeof name, "vr-effect-trace%u-T%u-%s", trace.traces, number, role);
    vk_end_pass();
    const bool saved = vr_shot_save_now(vk_tex((NvnTexture *)texture, true), name);
    LOG("[pc-vr-effect-trace] trace %u: T%u (the effects' %s) at draw %u %s build/%s.png\n", trace.traces, number, role,
        trace.eye_sequence_draws, saved ? "saved as" : "not savable, not", name);
}

// (development) SAVAGEWIND_PC_VR_EFFECT_TRACE=2: also every reduced target (200 guest pixels wide or less) a draw of the
// traced eye samples, saved the first time it is read, complete by then: where the eighth-size chain goes wrong.
static bool trace_small_targets;
bool vr_effect_trace_small_targets(void) { return trace_small_targets; }
static void save_small(const NvnTexture *texture, unsigned draw_number) {
    static const NvnTexture *saved[24];
    static unsigned saved_count, saved_trace;
    if (!texture || texture->width > 200) return;
    if (saved_trace != trace.traces) saved_trace = trace.traces, saved_count = 0;
    for (unsigned i = 0; i < saved_count; ++i)
        if (saved[i] == texture) return;
    if (saved_count >= 24) return;
    saved[saved_count++] = texture;
    char name[80];
    snprintf(name, sizeof name, "vr-effect-trace%u-small%02u-%ux%u-f%x-draw%u", trace.traces, saved_count, texture->width,
             texture->height, texture->format, draw_number);
    vk_end_pass();
    const bool saved_now = vr_shot_save_now(vk_tex((NvnTexture *)texture, true), name);
    LOG("[pc-vr-effect-trace] trace %u: small target %ux%u format %#x flags %#x storage %llx read at draw %u %s build/%s.png\n",
        trace.traces, texture->width, texture->height, texture->format, texture->flags, (unsigned long long)texture->addr,
        draw_number, saved_now ? "saved as" : "not savable, not", name);
}

static bool trace_on(void) {
    if (trace.on < 0) {
        const char *value = getenv("SAVAGEWIND_PC_VR_EFFECT_TRACE");
        trace.on = value && (atoi(value) == 1 || atoi(value) == 2);
        trace_small_targets = value && atoi(value) == 2;
        if (trace.on)
            LOG("[pc-vr-effect-trace] on: each eye shot request (Right Ctrl, capture button, eyeshot) traces the next "
                "frame's left eye, at most %d a run\n", TRACE_LIMIT);
    }
    return trace.on;
}

// A texture's number in this trace (1 up; 0: none or past the table), its description kept at first sight.
static uint8_t texture_number(const NvnTexture *texture) {
    if (!texture) return 0;
    for (unsigned i = 0; i < trace.texture_count; ++i)
        if (trace.textures[i].texture == texture) return (uint8_t)(i + 1);
    if (trace.texture_count >= TRACE_TEXTURES) return 0;
    TraceTexture *entry = &trace.textures[trace.texture_count++];
    memset(entry, 0, sizeof *entry);
    entry->texture = texture;
    entry->width = texture->width, entry->height = texture->height;
    entry->format = texture->format, entry->samples = texture->samples;
    const TexBackend *image = texture->backend;
    if (image) {
        entry->image_width = image->width, entry->image_height = image->height, entry->image_layers = image->layers;
        entry->image_format = image->format;
    }
    return (uint8_t)trace.texture_count;
}

static const char *vulkan_format_name(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "R11G11B10F";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "RGBA32F";
    case VK_FORMAT_R16G16_SFLOAT: return "RG16F";
    case VK_FORMAT_R16_SFLOAT: return "R16F";
    case VK_FORMAT_R32_SFLOAT: return "R32F";
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8 sRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "RGB10A2";
    case VK_FORMAT_R8G8_UNORM: return "RG8";
    case VK_FORMAT_R8_UNORM: return "R8";
    case VK_FORMAT_R16G16_UNORM: return "RG16";
    case VK_FORMAT_D32_SFLOAT: return "D32F";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32F S8";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24 S8";
    case VK_FORMAT_D16_UNORM: return "D16";
    default: return NULL;
    }
}

static TraceItem *new_item(bool is_event) {
    if (trace.item_count >= TRACE_ITEMS) { trace.items_overflow = true; return NULL; }
    TraceItem *item = &trace.items[trace.item_count++];
    memset(item, 0, sizeof *item);
    item->is_event = is_event;
    return item;
}

static int certificate_eye(uint64_t certificate) {
    if (!certificate) return -1;
    if (certificate != trace.certificate) {
        PcVrRenderCertificate resolved;
        trace.certificate = certificate;
        trace.certificate_eye = pc_vr_render_trace_resolve(certificate, &resolved) && resolved.native_index < 2
            ? (int)resolved.native_index : -1;
    }
    return trace.certificate_eye;
}

// The pass a draw into these targets belongs to: the last item when it is a pass into the same targets, else a new one.
static TraceItem *pass_for(const VrEffectTraceDraw *draw) {
    uint8_t colors[8] = {0};
    const int color_count = draw->color_count < 8 ? draw->color_count : 8;
    for (int i = 0; i < color_count; ++i) colors[i] = texture_number(draw->colors[i]);
    const uint8_t depth = texture_number(draw->depth);
    TraceItem *last = trace.item_count ? &trace.items[trace.item_count - 1] : NULL;
    if (last && !last->is_event && last->color_count == color_count && last->depth == depth &&
        !memcmp(last->colors, colors, sizeof colors))
        return last;
    TraceItem *pass = new_item(false);
    if (!pass) return NULL;
    memcpy(pass->colors, colors, sizeof colors);
    pass->color_count = (uint8_t)color_count;
    pass->depth = depth;
    pass->first_draw = trace.eye_sequence_draws;
    const NvnTexture *measure = color_count && draw->colors[0] ? draw->colors[0] : draw->depth;
    if (measure) pass->target_width = measure->width, pass->target_height = measure->height;
    return pass;
}

// Whether a rectangle of the pass leaves part of its target uncovered (the viewport or the scissor).
static bool rect_leaves_part(const TraceItem *pass, unsigned index) {
    if (!pass->target_width || !pass->target_height) return false;
    const float *viewport = pass->rects[index].viewport;
    const int32_t *scissor = pass->rects[index].scissor;
    const float width = (float)pass->target_width, height = (float)pass->target_height;
    const bool viewport_part = viewport[0] > 0.5f || viewport[1] > 0.5f || viewport[0] + viewport[2] < width - 0.5f ||
                               viewport[1] + viewport[3] < height - 0.5f;
    const bool scissor_part = scissor[0] > 0 || scissor[1] > 0 || (int64_t)scissor[0] + scissor[2] < (int64_t)width ||
                              (int64_t)scissor[1] + scissor[3] < (int64_t)height;
    return viewport_part || scissor_part;
}

static void pass_add(TraceItem *pass, const VrEffectTraceDraw *draw, int eye, bool effect, bool final) {
    pass->draws++;
    if (eye == 0) pass->eye_draws++;
    else if (eye == 1) pass->other_eye_draws++;
    else pass->uncertified_draws++;
    pass->blended += draw->blended;
    pass->depth_tested += draw->depth_test;
    pass->depth_written += draw->depth_test && draw->depth_write;
    pass->holds_final |= final;
    if (draw->field_haze && !effect) pass->haze_untagged++;
    if (effect) {
        pass->effect_draws++;
        pass->effect_haze += draw->field_haze;
        pass->effect_blended += draw->blended;
        pass->effect_depth_written += draw->depth_test && draw->depth_write;
        unsigned i = 0;
        while (i < pass->program_count && pass->programs[i] != draw->fragment_hash) ++i;
        if (i < pass->program_count) pass->program_draws[i]++;
        else if (pass->program_count < PASS_PROGRAMS) {
            pass->programs[pass->program_count] = draw->fragment_hash;
            pass->program_draws[pass->program_count++] = 1;
        } else pass->programs_overflow = true;
    }
    if (draw->viewport && draw->scissor) {
        unsigned index = 0;
        while (index < pass->rect_count &&
               (memcmp(pass->rects[index].viewport, draw->viewport, sizeof pass->rects[index].viewport) ||
                memcmp(pass->rects[index].scissor, draw->scissor, sizeof pass->rects[index].scissor)))
            ++index;
        if (index == pass->rect_count && index < PASS_RECTS) {
            memcpy(pass->rects[index].viewport, draw->viewport, sizeof pass->rects[index].viewport);
            memcpy(pass->rects[index].scissor, draw->scissor, sizeof pass->rects[index].scissor);
            pass->rects[index].draws = pass->rects[index].effect_draws = 0;
            pass->rect_count++;
        }
        if (index < pass->rect_count) {
            pass->rects[index].draws++;
            pass->rects[index].effect_draws += effect;
        } else {
            pass->rects_overflow = true;
        }
    }
    for (int s = 0; s < draw->sampled_count; ++s) {
        const uint8_t number = texture_number(draw->sampled[s]);
        if (!number) continue;
        unsigned i = 0;
        while (i < pass->sampled_count && pass->sampled[i] != number) ++i;
        if (i == pass->sampled_count && pass->sampled_count < PASS_SAMPLED) pass->sampled[pass->sampled_count++] = number;
    }
}

static void print_numbers(char *line, size_t size, const char *label, const uint8_t *numbers, unsigned count) {
    size_t used = strlen(line);
    if (used < size) used += (size_t)snprintf(line + used, size - used, "%s", label);
    for (unsigned i = 0; i < count && used < size; ++i)
        used += numbers[i] ? (size_t)snprintf(line + used, size - used, " T%u", numbers[i])
                           : (size_t)snprintf(line + used, size - used, " -");
}

static void log_eye_sequence(bool reached_final) {
    unsigned passes = 0;
    for (unsigned i = 0; i < trace.item_count; ++i) passes += !trace.items[i].is_event;
    LOG("[pc-vr-effect-trace] trace %u, frame %llu: the left eye from its first certified draw to %s: %u draws in %u "
        "passes%s\n", trace.traces, (unsigned long long)trace.frame,
        reached_final ? "its scene-final draw" : "the present (no scene-final draw seen)", trace.eye_sequence_draws,
        passes, trace.items_overflow ? " (more than the trace keeps: the rest left out)" : "");
    unsigned pass_number = 0;
    for (unsigned i = 0; i < trace.item_count; ++i) {
        const TraceItem *item = &trace.items[i];
        char line[512] = "";
        if (item->is_event) {
            static const char *const names[] = {"dispatch", "copy", "clear colour", "clear depth"};
            snprintf(line, sizeof line, "%s", names[item->event]);
            if (item->event == VR_EFFECT_TRACE_COPY) {
                const size_t used = strlen(line);
                snprintf(line + used, sizeof line - used, " T%u -> T%u", item->source, item->destination);
            } else if (item->event != VR_EFFECT_TRACE_DISPATCH) {
                const size_t used = strlen(line);
                snprintf(line + used, sizeof line - used, " T%u", item->destination);
            }
            LOG("[pc-vr-effect-trace]   %s x%u\n", line, item->repeats);
            continue;
        }
        ++pass_number;
        print_numbers(line, sizeof line, "colour", item->colors, item->color_count);
        if (!item->color_count) strncat(line, " none", sizeof line - strlen(line) - 1);
        print_numbers(line, sizeof line, ", depth", &item->depth, 1);
        LOG("[pc-vr-effect-trace] pass %u (draw %u on): %s: %u draws (eye %u, other eye %u, uncertified %u), blended %u, "
            "depth test %u, depth write %u; effect %u (field haze %u, blended %u, depth write %u)%s\n", pass_number,
            item->first_draw, line, item->draws, item->eye_draws, item->other_eye_draws, item->uncertified_draws,
            item->blended, item->depth_tested, item->depth_written, item->effect_draws, item->effect_haze,
            item->effect_blended, item->effect_depth_written, item->holds_final ? "; the scene-final draw" : "");
        if (item->haze_untagged) LOG("[pc-vr-effect-trace]   field haze draws without the effect tag: %u\n", item->haze_untagged);
        if (item->program_count) {
            char programs[512] = "";
            size_t used = 0;
            for (unsigned p = 0; p < item->program_count && used < sizeof programs; ++p)
                used += (size_t)snprintf(programs + used, sizeof programs - used, " %016llx x%u",
                                         (unsigned long long)item->programs[p], item->program_draws[p]);
            LOG("[pc-vr-effect-trace]   effect programs (fragment hash):%s%s\n", programs,
                item->programs_overflow ? " and more" : "");
        }
        if (item->sampled_count) {
            char sampled[256] = "";
            print_numbers(sampled, sizeof sampled, "samples", item->sampled, item->sampled_count);
            LOG("[pc-vr-effect-trace]   %s\n", sampled);
        }
        bool any_part = false;
        for (unsigned r = 0; r < item->rect_count; ++r) any_part |= rect_leaves_part(item, r);
        if (item->effect_draws || any_part) {
            for (unsigned r = 0; r < item->rect_count; ++r) {
                const float *viewport = item->rects[r].viewport;
                const int32_t *scissor = item->rects[r].scissor;
                LOG("[pc-vr-effect-trace]   viewport %.1f,%.1f %.1fx%.1f, scissor %d,%d %dx%d of %ux%u: %u draws, %u "
                    "effect%s\n", viewport[0], viewport[1], viewport[2], viewport[3], scissor[0], scissor[1],
                    scissor[2], scissor[3], item->target_width, item->target_height, item->rects[r].draws,
                    item->rects[r].effect_draws, rect_leaves_part(item, r) ? " (leaves part uncovered)" : "");
            }
            if (item->rects_overflow) LOG("[pc-vr-effect-trace]   (more rectangles than the trace keeps)\n");
        }
    }
    for (unsigned i = 0; i < trace.texture_count; ++i) {
        const TraceTexture *entry = &trace.textures[i];
        const char *name = vulkan_format_name(entry->image_format);
        char image_format[32];
        if (name) snprintf(image_format, sizeof image_format, "%s", name);
        else snprintf(image_format, sizeof image_format, "VkFormat %d", (int)entry->image_format);
        LOG("[pc-vr-effect-trace] T%u: guest %ux%u format %#x%s, image %ux%u%s %s\n", i + 1, entry->width,
            entry->height, entry->format, entry->samples > 1 ? " multisampled" : "", entry->image_width,
            entry->image_height, entry->image_layers > 1 ? " (layers)" : "", entry->image_width ? image_format : "none");
    }
}

void vr_effect_trace_draw(const VrEffectTraceDraw *draw) {
    if (!vr_effect_trace_running || !draw || !draw->command) return;
    const uint32_t layer_draw = nvn_layer_draw(draw->command);
    const bool effect = layer_draw & PC_VR_LAYER_DRAW_EFFECT;
    const int eye = certificate_eye(nvn_render_certificate(draw->command));
    trace.draws++;
    if (effect) trace.effect_by_eye[eye + 1]++;
    trace.haze_draws += draw->field_haze;
    trace.haze_tagged += draw->field_haze && effect;
    if (trace.stage == STAGE_FRAME && eye == 0) trace.stage = STAGE_FOLLOWING;
    if (trace.stage != STAGE_FOLLOWING) return;
    const bool final = (layer_draw & PC_VR_LAYER_DRAW_KIND) == PC_VR_LAYER_DRAW_FINAL && eye == 0;
    TraceItem *pass = pass_for(draw);
    if (pass) pass_add(pass, draw, eye, effect, final);
    trace.eye_sequence_draws++;
    if (trace_small_targets)
        for (int i = 0; i < draw->sampled_count; ++i) save_small(draw->sampled[i], trace.eye_sequence_draws);
    for (int i = 0; i < draw->sampled_count; ++i)  // (an effects buffer read, by any draw: saved complete)
        for (unsigned target = 0; target < trace.effect_target_count; ++target)
            if (draw->sampled[i] == trace.effect_targets[target]) save_once(draw->sampled[i], "effects-buffer");
    if (effect) {  // (what the effects test against and read, each saved the first time: what it covers)
        save_once(draw->depth, "depth");
        for (int i = 0; i < draw->sampled_count; ++i) save_once(draw->sampled[i], "sampled");
        for (int i = 0; draw->blended && i < draw->color_count && i < 8; ++i) {  // (not the opaque ones' G-buffer)
            unsigned target = 0;
            while (target < trace.effect_target_count && trace.effect_targets[target] != draw->colors[i]) ++target;
            if (draw->colors[i] && target == trace.effect_target_count && target < 8)
                trace.effect_targets[trace.effect_target_count++] = draw->colors[i];
        }
    }
    if (final) {
        log_eye_sequence(true);
        trace.stage = STAGE_DONE;
    }
}

void vr_effect_trace_event(VrEffectTraceEvent kind, const NvnTexture *source, const NvnTexture *destination) {
    if (!vr_effect_trace_running || trace.stage != STAGE_FOLLOWING) return;
    const uint8_t source_number = texture_number(source), destination_number = texture_number(destination);
    TraceItem *last = trace.item_count ? &trace.items[trace.item_count - 1] : NULL;
    if (last && last->is_event && last->event == kind && last->source == source_number &&
        last->destination == destination_number) {
        last->repeats++;
        return;
    }
    TraceItem *item = new_item(true);
    if (!item) return;
    item->event = kind;
    item->source = source_number, item->destination = destination_number;
    item->repeats = 1;
}

void vr_effect_trace_present(void) {
    if (!trace_on()) return;
    if (vr_effect_trace_running) {  // (the traced frame ends)
        if (trace.stage == STAGE_FOLLOWING) log_eye_sequence(false);
        else if (trace.stage == STAGE_FRAME)
            LOG("[pc-vr-effect-trace] trace %u, frame %llu: no draw of the left eye's\n", trace.traces,
                (unsigned long long)trace.frame);
        LOG("[pc-vr-effect-trace] trace %u, frame %llu, all of it: %u draws; effect-tagged draws: left eye %u, right eye "
            "%u, uncertified %u; field haze draws %u, of them tagged %u\n", trace.traces, (unsigned long long)trace.frame,
            trace.draws, trace.effect_by_eye[1], trace.effect_by_eye[2], trace.effect_by_eye[0], trace.haze_draws,
            trace.haze_tagged);
        vr_effect_trace_running = false;
        trace.stage = STAGE_IDLE;
    }
    const unsigned requests = pc_vr_eye_shot_requests();
    if (requests == trace.requests_seen) return;
    trace.requests_seen = requests;
    if (trace.traces >= TRACE_LIMIT) return;
    trace.traces++;
    trace.frame = renderer_present_count();
    memset(trace.effect_by_eye, 0, sizeof trace.effect_by_eye);
    trace.haze_draws = trace.haze_tagged = trace.draws = 0;
    trace.item_count = 0;
    trace.items_overflow = false;
    trace.eye_sequence_draws = 0;
    trace.texture_count = 0;
    trace.certificate = 0;
    trace.certificate_eye = -1;
    trace.saved_count = 0;
    trace.effect_target_count = 0;
    trace.stage = STAGE_FRAME;
    vr_effect_trace_running = true;
}
