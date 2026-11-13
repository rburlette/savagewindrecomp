// Fixtures: the game sites tools/game_sites.py lists for BOTW 1.8.2 (KNOWN '1.8.2'), as the loaded game library gives
// them (runtime/gamelib.c take_sites), for the PC VR modules and the upstream sites they read. One per fixture.
#pragma once
#include "../runtime/game_sites.h"

#define TEST_MAIN_SIZE_182 UINT64_C(0x1d37000)

GameSites g_site = {
    .pause_state = UINT64_C(0x7001cb48a4), .event_mgr = UINT64_C(0x7001cd8db8),
    .world_mgr = UINT64_C(0x7001cf5d00), .main_end = UINT64_C(0x7001d37000),
    .pcvr_matrix_builder = UINT64_C(0x7000d9f70c), .pcvr_base_caller = UINT64_C(0x7000fd8788),
    .pcvr_layer_base_caller = UINT64_C(0x7000ddbb14), .pcvr_eye_caller = UINT64_C(0x7000ddc1b0),
    .pcvr_proj_builder = UINT64_C(0x7000da13f4), .pcvr_proj_device = UINT64_C(0x7000da1034),
    .pcvr_proj_build_caller = UINT64_C(0x7000da0e44), .pcvr_proj_device_caller = UINT64_C(0x7000da0e68),
    .pcvr_proj_root_return = UINT64_C(0x7000dde6f0), .pcvr_proj_sibling_return = UINT64_C(0x7000ddbdd8),
    .pcvr_render_begin = UINT64_C(0x7000e60ac4), .pcvr_render_begin_caller = UINT64_C(0x7000ddccc0),
    .labo_set_vr = UINT64_C(0x700114d090), .labo_end_vr_call = UINT64_C(0x700114d158),
    .labo_enable_rec_call = UINT64_C(0x700114d15c), .labo_vr_notify_1 = UINT64_C(0x7000d76ce0),
    .labo_vr_notify_2 = UINT64_C(0x7000d76d90), .labo_sensor_ret = UINT64_C(0x700114d1d8),
    .labo_track_start_ret = UINT64_C(0x70011524c4),
    .pcvr_cam_update_0 = UINT64_C(0x7000ae17c4), .pcvr_cam_update_1 = UINT64_C(0x7000b097d4),
    .pcvr_cam_update_2 = UINT64_C(0x7000ae51a8), .pcvr_cam_update_3 = UINT64_C(0x7000b06c5c),
    .pcvr_cam_update_4 = UINT64_C(0x7000b0e838), .pcvr_cam_update_5 = UINT64_C(0x7000b10f08),
    .pcvr_cam_update_6 = UINT64_C(0x7000afabe0), .pcvr_cam_update_7 = UINT64_C(0x7000b00b68),
    .pcvr_actor_vtable = UINT64_C(0x7001b65568), .pcvr_eye_vtable = UINT64_C(0x7001ba4210),
    .pcvr_proj_vtable = UINT64_C(0x7001ba4438), .pcvr_base_vtable = UINT64_C(0x7001bbdf60),
    .labo_manager_slot = UINT64_C(0x7001c05cc8), .automation_screen_root = UINT64_C(0x7001c05d88),
};
const char *g_game_version = "1.8.2";
