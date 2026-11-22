// PC VR's configuration (runtime/pc_vr_config.c): read from the environment once, at the first use, and constant after.
//
// SAVAGEWIND_PC_VR=1 alone is the play configuration: head look, the projection trace in its continuous mode, the
// native FOV and view (at 14.53 game units per metre unless SAVAGEWIND_PC_VR_NATIVE_VIEW_UNITS_PER_METER gives another
// finite positive value), the render certificates and the eye images all run, and none of them can be switched off
// on its own. The research-era names that chose them one by one (SAVAGEWIND_PC_VR_HEAD_LOOK, _PROJECTION_TRACE,
// _NATIVE_FOV_TRACE, _NATIVE_FOV_EXPERIMENT, _NATIVE_VIEW_EXPERIMENT, _NATIVE_RENDER_TRACE, _EYE_IMAGES, _FIRST_LIGHT)
// are aliases: 1 agrees with the play configuration, another value is ignored and logged as such.
//
// Two sets of settings, each under its environment name: the play set (SAVAGEWIND_PC_VR_<name>), whose defaults are
// what the play runs use, and the diagnostics set, all off by default. The process's configuration is logged once, as
// "[pc-vr] configuration: ...", when PC VR is on.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_VR_NATIVE_VIEW_UNITS_PER_METER_DEFAULT 14.53
#define PC_VR_MENU_DISTANCE_DEFAULT 0.77  // metres (D056: the user's choice in the headset)
#define PC_VR_MENU_DISTANCE_MIN 0.1
#define PC_VR_MENU_DISTANCE_MAX 20.0
#define PC_VR_HUD_DEPTH_DEFAULT 0.9       // times the camera target's distance (D057: where the user set it by hand)
#define PC_VR_HUD_DEPTH_MIN 0.1
#define PC_VR_HUD_DEPTH_MAX 4.0
// (the user's session on Link of 2026-11-18 and two places on the simulator: at 0.8 as many models are detailed as by
// the game's own measure, the depth along the view, and none is coarser within 30 degrees of the view's centre; the
// user's look in the headset decides)
#define PC_VR_MODEL_DISTANCE_DEFAULT 0.8  // times a model's distance, for its level of detail
#define PC_VR_MODEL_DISTANCE_MIN 0.25
#define PC_VR_MODEL_DISTANCE_MAX 2.0
#define PC_VR_SHOT_PRESENTS 8
// (measured on Link at 90 Hz, 2026-11-15, eye scale 2.7, one or two runs each: starting 1.5 ms before the display
// frame, the frame still reads the pose published in it, the pose being published right after it is located, and
// about one image in 200 comes a display frame late while standing; at 0 about one in 55; -3 ms is no better)
#define PC_VR_HEADSET_PACE_OFFSET_MS_DEFAULT (-1.5)

typedef enum { PC_VR_RUNE_AIM_OFF, PC_VR_RUNE_AIM_ON, PC_VR_RUNE_AIM_FLIP } PcVrRuneAim;

// The aliases, a bit each in PcVrConfig's aliases_seen and aliases_ignored.
enum {
    PC_VR_ALIAS_HEAD_LOOK, PC_VR_ALIAS_PROJECTION_TRACE, PC_VR_ALIAS_NATIVE_FOV_TRACE,
    PC_VR_ALIAS_NATIVE_FOV_EXPERIMENT, PC_VR_ALIAS_NATIVE_VIEW_EXPERIMENT, PC_VR_ALIAS_NATIVE_RENDER_TRACE,
    PC_VR_ALIAS_EYE_IMAGES, PC_VR_ALIAS_FIRST_LIGHT, PC_VR_ALIAS_COUNT
};

// The play settings that check their value, a bit each in PcVrConfig's settings_ignored when a value was given and not
// taken (the default stays).
enum {
    PC_VR_SETTING_NATIVE_VIEW_UNITS_PER_METER, PC_VR_SETTING_MENU_DISTANCE, PC_VR_SETTING_HUD_DEPTH,
    PC_VR_SETTING_EYE_IMAGES_FLIP, PC_VR_SETTING_EYE_SCALE, PC_VR_SETTING_MODEL_DISTANCE, PC_VR_SETTING_COUNT
};

typedef struct {
    bool enabled;  // SAVAGEWIND_PC_VR=1 exactly: PC VR, in its play configuration
    bool shader_variants;  // PC VR's own shader translations (the field haze): with PC VR, or without its session by
                           // SAVAGEWIND_PC_VR_SHADERS=1 (the shader precompilation and the pipeline seed's export)
    // The play set, SAVAGEWIND_PC_VR_<name>.
    struct {
        double native_view_units_per_meter;  // NATIVE_VIEW_UNITS_PER_METER: the world scale (live: pc_vr_world_scale)
        double menu_distance;     // MENU_DISTANCE: the menus' quad, metres ahead (live: pc_vr_panel_depth_adjust)
        double hud_depth;         // HUD_DEPTH: the HUD's quad, times the camera target's distance (live as well)
        int eye_images_flip;      // EYE_IMAGES_FLIP=0 or 1: the eye images' row order; -1: the window present's
        double eye_scale;         // EYE_SCALE: the Labo eye targets' scale, a decimal in (1, 5]; 1: not scaled
        unsigned eye_width, eye_height;  // EYE_SIZE=WxH (PC VR only): the exact eye render and submission size; 0: none
        bool eye_size_malformed;  // EYE_SIZE given but not as pc_vr_parse_eye_size takes it: startup stops
        double model_distance;    // MODEL_DISTANCE: a model's level of detail by its distance from the point between
                                  // the eyes times this (runtime/hle/pc_vr_cull_view.c), within [0.25, 2]; 0: by the
                                  // game's own measure, the depth along the view, which changes as the head turns
        bool display_rate_tracking;  // DISPLAY_RATE_TRACKING=1
        bool ui_parallax;         // UI_PARALLAX=1: the game's own UI screen parallax kept
        bool hud_backdrop;        // HUD_BACKDROP=1: the HUD's backdrops drawn as the game draws them
        bool world_hud;           // WORLD_HUD: the world-tied HUD, on unless exactly 0
        bool world_hud_upright;   // WORLD_HUD_ROLL=0: markers upright in the image, not turned with the world's up
        double world_hud_test_turn;  // WORLD_HUD_ROLL=<degrees>: that much more clockwise, here in radians; 0: none
        PcVrRuneAim rune_aim;     // RUNE_AIM: 0 off, flip, otherwise on
    } play;
    // The diagnostics set, all off by default.
    struct {
        bool head_look_timing;    // SAVAGEWIND_PC_VR_HEAD_LOOK_TIMING=1: head look's bounded timing lines
        int projection_list;      // SAVAGEWIND_PC_VR_PROJECTION_LIST: 1 every perspective projection, 2 the world's
        bool perf;                // SAVAGEWIND_PC_VR_PERF, any value: the XR owner's timing windows
        bool offer_trace;         // SAVAGEWIND_PC_VR_OFFER_TRACE=1
        bool target_refresh;      // Pace 2 with display-rate tracking targets alternate refreshes; SAVAGEWIND_PC_VR_TARGET_REFRESH=0 disables.
        uint64_t shot_presents[PC_VR_SHOT_PRESENTS];  // SAVAGEWIND_PC_VR_SHOT=<present>[,...]
        unsigned shot_count;
        bool quick_menu_shots;    // SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS=1
        bool bench_head_requested;  // SAVAGEWIND_PC_VR_BENCH_HEAD given (benchmark only)
        uint64_t bench_head_start;  // its game present, 1..100000; 0: off, or not a present in that range
        bool log_panes;           // SAVAGEWIND_LOG_PANES: a number above 0
        bool log_screens;         // SAVAGEWIND_LOG_SCREENS: a number above 0
        bool no_headset_mask;     // SAVAGEWIND_PC_VR_NO_HEADSET_MASK=1: Labo's mask draws cover the whole image, the
                                  // depth mask nothing (D114 A/B runs)
        unsigned copy_wait_ms;    // SAVAGEWIND_PC_VR_COPY_WAIT_MS=1..100: the XR owner's first wait for its copy of an
                                  // eye pair, in milliseconds (100: as before the bound); 0: its own 3 ms
        unsigned headset_pace;    // SAVAGEWIND_PC_VR_HEADSET_PACE=1..6: the game's frames on the headset's grid, each
                                  // at least this many display frames (runtime/pc_vr_pace.h, an experiment); 0: off
        int64_t headset_pace_offset_ns;   // with the pace, SAVAGEWIND_PC_VR_HEADSET_PACE_OFFSET_MS, -50 to 50 (-1.5
                                          // when not given): a frame starts this long after a display frame does,
                                          // before it when negative
    } diagnostics;
    unsigned aliases_seen, aliases_ignored;  // a bit per alias given, and per alias given another value than 1
    unsigned settings_ignored;               // a bit per play setting whose value was not taken
} PcVrConfig;

// The configuration from a name lookup: lookup(context, name) is the setting's value, or NULL when it is not set.
// A pure function of what the lookup answers; the process's lookup is the environment (pc_vr_config).
typedef const char *(*PcVrConfigLookup)(const void *context, const char *name);
void pc_vr_config_parse(PcVrConfigLookup lookup, const void *context, PcVrConfig *out);

// The configuration as the "[pc-vr] configuration:" line shows it (without that prefix), into text (size bytes, cut
// off when longer). Returns the length it needed.
size_t pc_vr_config_describe(const PcVrConfig *config, char *text, size_t size);

// The process's configuration, read from the environment at the first call (any thread) and constant after.
const PcVrConfig *pc_vr_config(void);

#ifdef __cplusplus
}
#endif
