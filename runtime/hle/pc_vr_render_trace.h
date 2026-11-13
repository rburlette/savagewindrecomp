// Recording metadata only (64 certificate slots, reused). A certificate is an observed selected
// camera generation; it never grants rendered-image provenance or admission.
// D048: the eye-image producer labels pairs of them development-unproven for the headset.
#ifndef SAVAGEWIND_PC_VR_RENDER_TRACE_H
#define SAVAGEWIND_PC_VR_RENDER_TRACE_H
#include <stdbool.h>
#include <stdint.h>
#include "../xr/xr_runtime.h"

typedef struct {
    uint64_t id, traversal, note, sequence, frame, generation, reference_hash;
    uint64_t camera_hash, view_hash, parameters_hash, logical_hash, device_hash;
    double units_per_meter; // Explicit diagnostic input, NOT calibrated scale.
    // The eye projection's device matrix, the z and w rows' z and w terms (row-major [10], [11], [14], [15]): a
    // depth d of a view-space z is (a z + b) / (c z + e). D073's aim depth reads the scene with it.
    float depth_terms[4];
    float near_far[2];
    uint32_t native_index, side_bit;
    // The traversal's single latched runtime snapshot that built both eyes:
    // the exact poses/FOV to submit with the rendered pixels.
    XrPoseSnapshot native_snapshot;
} PcVrRenderCertificate;

// Remains enabled while recorded commands drain after camera observation stops.
bool pc_vr_render_trace_enabled(void);
// Call before any guest command-memory callback. Zero means unqualified.
uint64_t pc_vr_render_trace_record(uint64_t guest_command_buffer);
// Historical immutable lookup; never consults the latest camera or pose.
// Failure leaves *out untouched. No guest addresses are returned. Under D048
// eye images (continuous mode), slots are recycled after 64 newer certificates; a recycled ID
// fails to resolve rather than returning another certificate.
bool pc_vr_render_trace_resolve(uint64_t id, PcVrRenderCertificate *out);
// Called before every observed original view-builder invocation, including
// unsupported callers. It only invalidates diagnostic eligibility.
void pc_vr_render_trace_invalidate_camera(uint64_t camera);

// What a draw is to the eye-image producer, known by the game code that issues it and not by its sizes or formats.
// The game finishes every render layer through the same two calls (tools/game_sites.py pcvr_layer_final_draw and
// pcvr_layer_placement_draw; tools/patches.py _layer_draw), and shows the frame through one of two more
// (pcvr_display_lens_draw, pcvr_display_copy_draw). A draw recorded inside one carries its kind (runtime/nvn/nvn.h
// nvn_layer_draw).
typedef enum {
    PC_VR_LAYER_DRAW_NONE,
    PC_VR_LAYER_DRAW_FINAL,      // the layer's HDR scene drawn into its finished image
    PC_VR_LAYER_DRAW_PLACEMENT,  // the layer's finished image drawn into its place in the final image
    PC_VR_LAYER_DRAW_DISPLAY,    // the final image drawn to the display: Labo's lens pass, or the plain copy
} PcVrLayerDraw;
#define PC_VR_LAYER_DRAW_KIND 3u
// With the kind: the layer is one of the two eye layers (the game also finishes the pause menu's layer this way).
// It says which layer the draw belongs to, whether or not that eye has a certificate this frame. The eye layers are
// known from the eye notes once the head look has bound the game's camera, and before that (after the first load)
// from the Labo display's own root and sibling calls for its pair of layers, which need no binding but would also
// name the pause menu's own pair (pc_vr_projection_trace.c).
#define PC_VR_LAYER_DRAW_EYE 4u
// With the kind: the layer may be an eye's. Only while that cannot be told: no eye layer is known yet (before the
// display's first pair of eye layers has been seen, and before any eye note), the trace is not armed, or the thread's
// render scopes are not to be trusted (poisoned, or the innermost one not a layer's own). Such a frame is taken for an uncertified scene, as
// if the layer were an eye's, so it is never shown as a flat menu panel.
#define PC_VR_LAYER_DRAW_MAYBE_EYE 8u
// Called by patched game code around the call that issues the draw, on the thread recording it. begin returns what
// was set before, which end restores.
uint32_t rt_layer_draw_begin(uint32_t kind);
void rt_layer_draw_end(uint32_t previous);
// What a draw recorded on this thread now is: a PcVrLayerDraw, with its layer's flag. 0 outside those calls.
uint32_t pc_vr_layer_draw_recording(void);
#endif
