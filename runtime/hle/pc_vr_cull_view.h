// PC VR: the head's view over both eyes for the game's visibility decisions (pc_vr_cull_view.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../cpu.h"
#include "pc_vr_head_look_math.h"

// The frame's two eye cameras (by runtime eye: 0 left, 1 right) and their fields of view, as the frame record wrote
// them for the eye layers that copy `base_camera` (pc_vr_projection_trace.c, at the record's root call).
void pc_vr_cull_view_publish(uint64_t base_camera, const PcVrHeadLookCamera eyes[2],
                             const float fov_left_right_up_down[2][4]);

// The game patches (tools/patches.py _cull_matrix, _cull_fov). `camera` is the camera the game builds its visibility
// view from. The matrix to build it from: the head view's when the feature is on, a view has been published and the
// camera is the eyes' base camera; otherwise the camera's own (camera + 8).
uint64_t rt_pc_vr_cull_matrix(uint64_t camera);
// After Labo's change of the field of view: under the same conditions the head view's fovy and aspect replace the
// pair at the two guest addresses.
void rt_pc_vr_cull_fov(uint64_t camera, uint64_t fovy_address, uint64_t aspect_address);

// The grass patches (tools/patches.py _grass_volume, _grass_matrix), in the grass view object's update: `eye_volume`
// is the culling volume the layer record holds. When the feature is on, a view has been published and the record's
// layer has the eyes' base camera for its camera, a volume of the head view built by the game's own code (with `cpu`,
// whose registers are kept); otherwise `eye_volume`.
uint64_t rt_pc_vr_cull_grass_volume(Cpu *cpu, uint64_t eye_volume, uint64_t layer_record, uint64_t grass_view);
// The record's view matrix (`projection` 0) or projection matrix (1) for the grass: the head view's when the volume
// just given to the same record on this thread was; otherwise `record_matrix`.
uint64_t rt_pc_vr_cull_grass_matrix(uint64_t layer_record, unsigned projection, uint64_t record_matrix);
