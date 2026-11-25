// The SDL input fixtures exercise the headset focus policy without starting the renderer.
#pragma once
#include "../runtime/pc_vr.h"

static bool test_vr_enabled;
bool pc_vr_enabled(void) { return test_vr_enabled; }
bool pc_vr_controller_without_focus(void) { return test_vr_enabled; }
bool pc_vr_world_scale_active(void) { return false; }
double pc_vr_world_scale(double factor) { (void)factor; abort(); }
double pc_vr_panel_depth(PcVrPanel which) { (void)which; return 0; }
double pc_vr_panel_depth_adjust(PcVrPanel which, double factor) { (void)which; (void)factor; abort(); }
void pc_vr_eye_shot_key_poll(void) {}
void pc_vr_eye_shot_request(void) {}
void color_grade_toggle_live(void) { abort(); }
void color_grade_cycle_live(void) { abort(); }
void vr_dlaa_next_mode(void) { abort(); }
