// Addresses in the game's main module that the runtime reads or writes. They differ between game versions:
// tools/game_sites.py finds them for the version the game code was generated from, and the game library carries them
// (runtime/gamelib.h, the generated layout.c). g_site holds them once the library is loaded (gamelib_load).
#pragma once
#include <stdint.h>

// name (as in tools/game_sites.py) and what is there
#define GAME_SITES(X)                                                                                                  \
    X(opt_table)               /* the Options screen's option table: u32 count, +8 entries, +0x10 rows, +0x18 order */ \
    X(opt_two_redraw)          /* optional all-or-nothing dual-row spike gate */                                      \
    X(opt_two_text)            /* native Options message-to-layout text setter */                                    \
    X(opt_two_help)            /* native row description update */                                                   \
    X(opt_two_pane_calc)       /* verified local transform/dirty-bit contract */                                      \
    X(screen_mgr)              /* ScreenMgr *: +0x28 screen count, +0x30 screen pointers by id */                      \
    X(beast_focus_axes)                                                                                                \
    X(beast_focus_linear)                                                                                              \
    X(beast_confirm_axes)                                                                                              \
    X(beast_confirm_linear)                                                                                            \
    X(beast_clamp_pitch)                                                                                               \
    X(beast_wrap_yaw)                                                                                                  \
    X(beast_world)                                                                                                     \
    X(opt_screen_rows)         /* field: the Options screen's row count, +8 its rows (1.8.2 +0x3698, 1.9.0 +0x36b0) */ \
    X(title_options_state)     /* field: the title menu's active button screen (0x6a: Options; 1.8.2 +0x363c) */       \
    X(dialog_answer)           /* the two-button dialog's answer (u32) */                                              \
    X(pause_state)             /* u32: 0 playing, 2 in the pause menu or the map */                                    \
    X(event_mgr)               /* ksys::evt::Manager *: its active event at +0x1d2b8 */                                \
    X(controller_step)         /* float: the controllers' hold counter step (1) */                                     \
    X(frame_loop_interval_ret) /* the frame loop's return address from setting the present interval */                \
    X(title_version_format)    /* "Ver.%s": the title screen's version line */                                         \
    X(player_info)             /* uking::act::PlayerInfo * (test tools) */                                              \
    X(world_mgr)               /* ksys::world::Manager * (test tools, weather.c)*/                                                \
    X(main_data)               /* main's .data (then .bss, up to main_end) */                                           \
    X(main_end)

// PC VR's sites (tools/game_sites.py: one optional group): all of them, or all 0 and PC VR stays off (pc_vr_main_
// compatible). Code addresses unless marked; "ret" / "caller": the return address after a call.
#define GAME_SITES_PC_VR(X)                                                                                            \
    X(pcvr_matrix_builder)      /* the camera matrix builder (head look composes the eye views into it) */            \
    X(pcvr_base_caller)         /* its call for the base camera (ret) */                                               \
    X(pcvr_layer_base_caller)   /* its call for a render layer's base camera (ret) */                                  \
    X(pcvr_eye_caller)          /* its call for a Labo eye camera (ret) */                                             \
    X(pcvr_proj_builder)        /* the projection builder */                                                           \
    X(pcvr_proj_device)         /* the projection's device transform */                                                \
    X(pcvr_proj_build_caller)   /* the projection update's call to the builder (ret) */                                \
    X(pcvr_proj_device_caller)  /* its call to the device transform (ret) */                                           \
    X(pcvr_proj_root_return)    /* the render tree's root traversal (ret) */                                           \
    X(pcvr_proj_sibling_return) /* a sibling layer's traversal (ret) */                                                \
    X(pcvr_render_begin)        /* a render layer's begin (its end follows it at +0x58) */            \
    X(pcvr_render_begin_caller) /* the layer loop's call to begin (ret) */                                             \
    X(labo_set_vr)              /* the Labo VR object's switch: (vr, on) */                                            \
    X(labo_end_vr_call)         /* the transition's EndVrMode call (redirected to BeginVrMode) */                      \
    X(labo_enable_rec_call)     /* its EnableRecording call (redirected to the SevenSix timestamp reset) */            \
    X(labo_vr_notify_1)         /* the first manager the Options screen notifies after the switch */                  \
    X(labo_vr_notify_2)         /* the second one */                                                                   \
    X(labo_sensor_ret)          /* the Labo sensor read's return address */                                            \
    X(labo_track_start_ret)     /* tracking start's StopSevenSixAxisSensor call (ret) */                               \
    X(pcvr_cam_update_0)        /* the eight camera types' update entries (runtime/hle/pc_vr_camera_update.c) */       \
    X(pcvr_cam_update_1) X(pcvr_cam_update_2) X(pcvr_cam_update_3) X(pcvr_cam_update_4)                                \
    X(pcvr_cam_update_5) X(pcvr_cam_update_6) X(pcvr_cam_update_7)                                                     \
    X(pcvr_actor_vtable)        /* data: the camera controller actor's vtable */                                       \
    X(pcvr_eye_vtable)          /* data: the eye camera's vtable */                                                    \
    X(pcvr_proj_vtable)         /* data: the projection's vtable */                                                    \
    X(pcvr_base_vtable)         /* data: the base camera's vtable */                                                   \
    X(labo_manager_slot)        /* data: the Labo manager's singleton slot (manager +0x380: the VR object) */          \
    X(automation_screen_root)   /* data: the UI root slot automation reads (runtime/hle/framepace.c) */

// Optional helpers are zero when their complete feature group was not found.
#define GAME_SITES_OPTIONAL(X) \
    X(pouch_lock)             /* native recursive inventory mutex */ \
    X(pouch_unlock)           \
    X(pouch_page_count)       /* entries on an inventory page */ \
    X(pouch_page_item)        /* PouchItem * at a page and slot */ \
    X(pouch_item_type)        /* equipment category from an actor name */ \
    X(pouch_sort)             /* native category sort */ \
    X(quick_sort_event)       /* native event restriction */ \
    X(quick_sort_player_busy) /* native player-state restriction */ \
    X(quick_sort_modal)       /* another modal screen is open */ \
    X(quick_sort_refill)      /* rebuild the quick selector's item list */ \
    X(quick_sort_mapping)     /* rebuild visible slot mapping */ \
    X(quick_sort_cursor)      /* move the native cursor */ \
    X(quick_sort_labels)      /* update selected item labels */ \
    X(player_drop_forbidden) \
    X(actor_info_lookup) \
    X(actor_info_has_tag) \
    X(pouch_discard) \
    X(player_equipment) \
    X(pouch_default_name) \
    X(player_schedule_equip) \
    X(pouch_equipped_snapshot) \
    X(actor_index_assign) \
    X(actor_index_resolve) \
    X(actor_index_release) \
    X(actor_link_release) \
    X(actor_link_alive) \
    X(pouch_cleanup) \
    X(player_actor) \
    X(quick_drop_trash_prohibited) \
    X(quick_drop_actor_method) \
    X(actor_info) \
    X(horse_cam_lr)           /* CameraHorse's return address after calling the camera stick input */

typedef struct {
#define GAME_SITE_FIELD(n) uint64_t n;
    GAME_SITES(GAME_SITE_FIELD)
    GAME_SITES_PC_VR(GAME_SITE_FIELD)
    GAME_SITES_OPTIONAL(GAME_SITE_FIELD)
#undef GAME_SITE_FIELD
} GameSites;

#ifdef __cplusplus
extern "C" {
#endif

extern GameSites g_site;
// The game version the loaded game code is for ("1.8.2"; "" before gamelib_load).
extern const char *g_game_version;

#ifdef __cplusplus
}
#endif
