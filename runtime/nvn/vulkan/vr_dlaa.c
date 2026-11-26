// PC VR renderer: NVIDIA DLAA on each eye image (an experiment). What it is for, its inputs, what it does not own and
// its conventions: vr_dlaa.h.
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "dlss.h"
#include "record.h"
#include "vr_dlaa.h"
#include "vr_math.h"
#include "../../rt.h"
#include "../../pc_vr.h"
#include "../../pc_vr_config.h"

uint64_t renderer_present_count(void);  // (device.c) the presents so far
uint32_t nvn_window_origin(void);       // (objects.c) the game's window origin: 0 lower left
// (exec.c) the camera's motion vectors into vectors (RG16F at depth's size), from a camera candidate's floats (the
// view-projection, row-major, at 12 to 27) and the previous frame's view-projection; link / previous_link: Link's
// position now and then (link NULL: none). false: nothing made.
bool vk_camera_motion_vectors(VkCommandBuffer cb, const float *camera, const double *previous_view_projection,
                              const float *link, const float *previous_link, TexBackend *depth, TexBackend *vectors);

enum { VR_DLAA_CAMERA_FLOATS = 58 };  // (a camera candidate's floats, exec.c FG_CAMERA_FLOATS)

// Why an eye image was not anti-aliased, counted.
typedef enum { VR_DLAA_SKIP_DEPTH, VR_DLAA_SKIP_CAMERA, VR_DLAA_SKIP_DLSS, VR_DLAA_SKIP_COUNT } VrDlaaSkip;
static const char *const vr_dlaa_skip_names[VR_DLAA_SKIP_COUNT] = {"depth", "camera", "dlss"};

// One eye's history: its camera and Link's position at the frame DLAA last ran for it.
typedef struct {
    VrDlaaCamera camera;
    float link_position[3];
    bool has_link;
    uint64_t present;  // renderer_present_count() then
    bool valid;        // false: none, the next frame starts over
} VrDlaaHistory;

static struct {
    VrDlaaHistory eyes[2];
    // Its own images, shared by the eyes in turn (each eye's placement draws the output before the next eye's DLAA
    // writes it again, with a full barrier between): the camera vectors and DLSS's output.
    TexBackend vectors, output;
    uint64_t runs, resets, first_logged[2];
    uint64_t skipped[VR_DLAA_SKIP_COUNT];
} vr_dlaa;

// The modes the '9' key cycles, in this order (vr_dlaa.h). Decided at the first present (DLSS must be up), changed
// only at a present: the key asks (any thread), vr_dlaa_present applies, so a frame is drawn in one mode throughout.
static const char *const vr_dlaa_mode_names[VR_DLAA_MODES] = {"none", "DLAA", "DLSS"};
static int vr_dlaa_mode = VR_DLAA_MODE_NONE;                  // the mode now (the thread that replays the commands)
static bool vr_dlaa_decided;                                  // the first present has decided it
static _Atomic int vr_dlaa_switchable;                        // the switch is on and DLSS runs: the key cycles
static _Atomic int vr_dlaa_requested = -1, vr_dlaa_shown = -1;  // the key's request; the mode last applied

// The game's own textures in the eye's draws, biased by log2 of the render size over the output size while DLSS brings
// the eye up (0 at DLAA: as the user judged it). Set by each run; read per texture by exec.c.
static float vr_dlaa_bias;

bool vr_dlaa_enabled(void) { return vr_dlaa_mode != VR_DLAA_MODE_NONE; }

// The DLSS mode's render ratio and eye scale (vr_dlss_ratio, vr_dlss_eye_scale: 1.5, NVIDIA's Quality, and 3.0)
static double vr_dlss_ratio(void) { return pc_vr_config()->settings.dlss_ratio; }
static double vr_dlss_eye_scale(void) { return pc_vr_config()->settings.dlss_eye_scale; }

double vr_dlaa_render_ratio(void) { return vr_dlaa_mode == VR_DLAA_MODE_DLSS ? vr_dlss_ratio() : 1; }

float vr_dlaa_mip_bias(void) { return vr_dlaa_bias; }

void vr_dlaa_next_mode(void) {
    if (!atomic_load(&vr_dlaa_switchable)) {
        static int told;
        if (!told++) LOG("[pc-vr-dlaa] the 9 key: DLAA is not switched on for this run, or DLSS does not run here\n");
        return;
    }
    const int requested = atomic_load(&vr_dlaa_requested), shown = atomic_load(&vr_dlaa_shown);
    atomic_store(&vr_dlaa_requested, ((requested >= 0 ? requested : shown) + 1) % VR_DLAA_MODES);
}

// (textures.c) The eye targets' scale again from the eye size and the render ratio now.
void vk_eye_scale_refresh(void);
void vr_dlaa_present(void) {
    int mode = atomic_exchange(&vr_dlaa_requested, -1);
    if (!vr_dlaa_decided) {  // (the first present: DLSS is up by now, or will not be)
        vr_dlaa_decided = true;
        const PcVrConfig *config = pc_vr_config();
        if (!config->enabled || config->settings.antialiasing == PC_VR_ANTIALIASING_OFF) return;  // (every present)
        if (!vk_dlss_available()) {
            LOG("[pc-vr-dlaa] switched on, but DLSS does not run here: the eye images as they are\n");
            return;
        }
        atomic_store(&vr_dlaa_switchable, 1);
        if (mode < 0)
            mode = config->settings.antialiasing == PC_VR_ANTIALIASING_DLSS ? VR_DLAA_MODE_DLSS : VR_DLAA_MODE_DLAA;
        LOG("[pc-vr-dlaa] the 9 key cycles none, DLAA at eye scale %.3g, DLSS from %.3g times smaller up to eye "
            "scale %.3g\n", config->settings.eye_scale, vr_dlss_ratio(), vr_dlss_eye_scale());
    }
    if (mode < 0 || (mode == vr_dlaa_mode && atomic_load(&vr_dlaa_shown) >= 0)) return;
    vr_dlaa_mode = mode;
    atomic_store(&vr_dlaa_shown, mode);
    // (the sizes: the headset's images follow the eye size from this frame, the eye targets the render ratio)
    pc_vr_eye_scale_override(mode == VR_DLAA_MODE_DLSS ? vr_dlss_eye_scale() : 0);
    vk_eye_scale_refresh();
    vr_dlaa_reset();
    vr_dlaa.first_logged[0] = vr_dlaa.first_logged[1] = 0;  // (each mode's first run logged)
    if (mode != VR_DLAA_MODE_DLSS) vr_dlaa_bias = 0;
    if (mode == VR_DLAA_MODE_NONE) pc_vr_render_trace_jitter(0, 0, 0);
    unsigned width = 0, height = 0;
    pc_vr_eye_size(&width, &height);
    const double ratio = vr_dlaa_render_ratio();
    LOG("[pc-vr-dlaa] mode %s from present %llu: eye images %ux%u, rendered %ldx%ld\n", vr_dlaa_mode_names[mode],
        (unsigned long long)renderer_present_count(), width, height, lround(width / ratio), lround(height / ratio));
}

// NGX's mode for a render size this many times smaller per axis than the output (dlss.h, settings.h UPQ_*: 0 Ultra
// Quality 1.3, 1 Quality 1.5, 2 Balanced 1.72, 3 Performance 2, 4 Ultra Performance 3). At the output's size dlss.c
// makes DLAA, NGX's 1x mode, whatever the mode asked for.
static unsigned vr_dlaa_quality(double ratio) {
    return ratio < 1.4 ? 0 : ratio < 1.6 ? 1 : ratio < 1.85 ? 2 : ratio < 2.5 ? 3 : 4;
}

void vr_dlaa_reset(void) { vr_dlaa.eyes[0].valid = vr_dlaa.eyes[1].valid = false; }

// One of the module's images at this size and format, made again when either changes (exec.c post_image_ex's way):
// sampled, drawn into and copied from, with extra usages.
static TexBackend *vr_dlaa_image(TexBackend *image, uint32_t width, uint32_t height, VkFormat format,
                                 VkImageUsageFlags extra) {
    if (image->image && (image->width != width || image->height != height || image->format != format)) {
        vk_views_forget(image);
        vk_defer_image(image->image, image->mem);
        image->image = VK_NULL_HANDLE;
        image->mem = VK_NULL_HANDLE;
    }
    if (!image->image) {
        VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = format, .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     extra};
        VmaAllocationCreateInfo allocation = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(g_vk.vma, &info, &allocation, &image->image, &image->mem, NULL) != VK_SUCCESS) {
            allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;  // (no room in VRAM: slower, no abort)
            VK_CHECK(vmaCreateImage(g_vk.vma, &info, &allocation, &image->image, &image->mem, NULL));
        }
        image->format = format; image->width = width; image->height = height; image->depth = 1; image->layers = 1;
        image->levels = 1; image->samples = 1; image->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        image->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), image->image, image->aspect);
    }
    return image;
}

// An eye image not anti-aliased: counted, the eye's history ended.
static TexBackend *vr_dlaa_skip(unsigned eye, VrDlaaSkip why) {
    if (eye < 2) vr_dlaa.eyes[eye].valid = false;
    if (vr_dlaa.skipped[why]++ < 8)
        LOG("[pc-vr-dlaa] eye %u not anti-aliased (%s) present=%llu (line %llu of 8 for that reason)\n", eye,
            vr_dlaa_skip_names[why], (unsigned long long)renderer_present_count(),
            (unsigned long long)vr_dlaa.skipped[why]);
    return NULL;
}

// DLSS on an eye image with that eye's depth at its size and certificate, to output_width x output_height: the module's
// output image (RGBA16F, valid until the other eye's run), or NULL: not run, why counted and logged now and then.
static TexBackend *vr_dlaa_run(VkCommandBuffer cb, unsigned eye, TexBackend *color, TexBackend *depth,
                               const PcVrRenderCertificate *certificate, uint32_t output_width,
                               uint32_t output_height) {
    const uint32_t width = color->width, height = color->height;
    const double ratio = (double)output_width / width;
    if (!depth || !depth->image || !(depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || depth->width != width ||
        depth->height != height)
        return vr_dlaa_skip(eye, VR_DLAA_SKIP_DEPTH);
    VrDlaaCamera camera;
    if (!vr_dlaa_camera(certificate->view, certificate->device_projection, certificate->jitter_ndc, &camera))
        return vr_dlaa_skip(eye, VR_DLAA_SKIP_CAMERA);
    VrDlaaHistory *history = &vr_dlaa.eyes[eye];
    const uint64_t present = renderer_present_count();
    bool reset = !history->valid || history->present + 1 != present || vr_dlaa_cut(&camera, &history->camera);
    // The camera vectors: this frame's view-projection against the eye's previous one (this one's at a reset).
    float camera_floats[VR_DLAA_CAMERA_FLOATS] = {0};
    for (unsigned i = 0; i < 16; ++i) camera_floats[12 + i] = (float)camera.view_projection[i];
    const double *previous = reset ? camera.view_projection : history->camera.view_projection;
    const bool link = !reset && certificate->has_link && history->has_link;
    TexBackend *vectors = vr_dlaa_image(&vr_dlaa.vectors, width, height, VK_FORMAT_R16G16_SFLOAT,
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    vk_full_barrier(cb);  // (the other eye's DLSS read the vectors and the output before)
    bool moved = vk_camera_motion_vectors(cb, camera_floats, previous, link ? certificate->link_position : NULL,
                                          history->link_position, depth, vectors);
    if (!moved) {  // (zero vectors and no history)
        VkClearColorValue zero = {{0, 0, 0, 0}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        rec_CmdClearColorImage(cb, vectors->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        reset = true;
    }
    history->camera = camera;
    memcpy(history->link_position, certificate->link_position, sizeof history->link_position);
    history->has_link = certificate->has_link;
    history->present = present;
    history->valid = moved;
    // (NGX writes its output as a storage image, whatever the eye image's format)
    TexBackend *output = vr_dlaa_image(&vr_dlaa.output, output_width, output_height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                       VK_IMAGE_USAGE_STORAGE_BIT);
    vk_full_barrier(cb);
    float jitter[2];
    vr_dlaa_jitter_pixels(certificate->jitter_ndc, width, height, nvn_window_origin(), jitter);
    // (no bias mask: upstream makes its mask of the blended effects for the flat game's scene only)
    const bool ran = vk_dlss_apply(cb, VK_DLSS_SLOT_LEFT_EYE + eye, color, depth, vectors, NULL, output,
                                   vr_dlaa_quality(ratio), jitter[0], jitter[1], (float)vectors->width,
                                   (float)vectors->height, reset);
    vk_dyn_forget();
    if (!ran) {
        if (!vk_dlss_available()) {  // (NGX cannot make DLSS here after all: mode none from the next present)
            atomic_store(&vr_dlaa_switchable, 0);
            atomic_store(&vr_dlaa_requested, VR_DLAA_MODE_NONE);
        }
        return vr_dlaa_skip(eye, VR_DLAA_SKIP_DLSS);
    }
    // (the offset for the frames to come, over NVIDIA's 8 times the upscale's area; and the textures' bias)
    pc_vr_render_trace_jitter(width, height, (uint32_t)ceil(8 * ratio * ratio - 1e-9));
    vr_dlaa_bias = ratio > 1 ? (float)log2(1 / ratio) : 0;
    ++vr_dlaa.runs;
    vr_dlaa.resets += reset;
    if (vr_dlaa.first_logged[eye]++ == 0)
        LOG("[pc-vr-dlaa] eye %u: first %s %ux%u -> %ux%u (format %d), depth format %d, offset %.3f, %.3f pixels, "
            "texture bias %.2f, %s\n", eye, vr_dlaa_mode_names[vr_dlaa_mode], width, height, output_width,
            output_height, (int)color->format, (int)depth->format, jitter[0], jitter[1], vr_dlaa_bias,
            link ? "Link's box moving" : "camera vectors only");
    if (vr_dlaa.runs % 4000 == 0)
        LOG("[pc-vr-dlaa] %llu eye images anti-aliased, %llu with the history started over; not run: %llu depth, "
            "%llu camera, %llu dlss\n", (unsigned long long)vr_dlaa.runs, (unsigned long long)vr_dlaa.resets,
            (unsigned long long)vr_dlaa.skipped[VR_DLAA_SKIP_DEPTH],
            (unsigned long long)vr_dlaa.skipped[VR_DLAA_SKIP_CAMERA],
            (unsigned long long)vr_dlaa.skipped[VR_DLAA_SKIP_DLSS]);
    return output;
}

TexBackend *vr_dlaa_eye(VkCommandBuffer cb, TexBackend *color, TexBackend *depth,
                        const PcVrRenderCertificate *certificate, uint32_t output_width, uint32_t output_height) {
    const unsigned eye = certificate->native_index;
    if (eye >= 2) return NULL;
    // (within a pixel of the eye image's own size: DLAA at that size; the placement's size is rounded)
    if (output_width <= color->width + 1 && output_height <= color->height + 1)
        output_width = color->width, output_height = color->height;
    return vr_dlaa_run(cb, eye, color, depth, certificate, output_width, output_height);
}
