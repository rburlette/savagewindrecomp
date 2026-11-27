// PC VR's configuration (runtime/pc_vr_config.c): its settings and its diagnostics, read once, at the first use, and
// constant after.
//
// The settings are savagewind.ini keys, as upstream's own are (runtime/settings.c hands PC VR the vr* lines it reads
// and asks it to write them back): `vr = 1` is PC VR, in its play configuration, and each other key's default is the
// play value (D130, D132), so a settings file with `vr = 1` alone plays as the play runs do. `savagewind settings set
// vr_eye_scale 2.7` changes one. A value outside its range is not taken: the default stays and the configuration line
// names it under "ignored".
//
// The diagnostics are launch options (runtime/launch_flags.h), environment flags all off by default: traces, shots,
// timing lines and A/B switches (SAVAGEWIND_PC_VR_<name>). PC VR's configuration is logged once, as
// "[pc-vr] configuration: ...", when PC VR is on.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_VR_GAME_UNITS_PER_METER_DEFAULT 14.53
#define PC_VR_MENU_DISTANCE_DEFAULT 0.77  // metres (D056: the user's choice in the headset)
#define PC_VR_MENU_DISTANCE_MIN 0.1
#define PC_VR_MENU_DISTANCE_MAX 20.0
#define PC_VR_HUD_DEPTH_DEFAULT 0.9       // times the camera target's distance (D057: where the user set it by hand)
#define PC_VR_HUD_DEPTH_MIN 0.1
#define PC_VR_HUD_DEPTH_MAX 4.0
// (D130: 2042x2203 at 72 Hz in the headset's view shape; 1: not scaled)
#define PC_VR_EYE_SCALE_DEFAULT 2.5
#define PC_VR_EYE_SCALE_MIN 1.0
#define PC_VR_EYE_SCALE_MAX 5.0
// (the user's session on Link of 2026-11-18 and two places on the simulator: at 0.8 as many models are detailed as by
// the game's own measure, the depth along the view, and none is coarser within 30 degrees of the view's centre; the
// user's look in the headset decides)
#define PC_VR_MODEL_DISTANCE_DEFAULT 0.8  // times a model's distance, for its level of detail
#define PC_VR_MODEL_DISTANCE_MIN 0.25
#define PC_VR_MODEL_DISTANCE_MAX 2.0
// The headset pace (runtime/pc_vr_pace.h, D130): a game frame every 2 display frames (36 a second at 72 Hz), starting
// 1.5 ms after a display frame's start (a session at 90 Hz plays at -1.5), its first eye call waiting up to 8.5 ms for
// the next display frame's head pose.
#define PC_VR_HEADSET_PACE_DEFAULT 2
#define PC_VR_HEADSET_PACE_MAX 6
#define PC_VR_PACE_OFFSET_MS_DEFAULT 1.5
#define PC_VR_PACE_OFFSET_MS_LIMIT 50.0  // either way
#define PC_VR_POSE_WAIT_MS_DEFAULT 8.5
#define PC_VR_POSE_WAIT_MS_MAX 20.0
// DLSS on each eye (D132): rendered 1.5 times smaller each way and brought up to eye scale 3.0
#define PC_VR_DLSS_RATIO_DEFAULT 1.5
#define PC_VR_DLSS_RATIO_MAX 3.0
#define PC_VR_DLSS_EYE_SCALE_DEFAULT 3.0
#define PC_VR_DLSS_EYE_SCALE_MAX 5.0
#define PC_VR_SHOT_PRESENTS 8

// vr_antialiasing: off (the eye images as the game makes them, its FXAA), or NVIDIA's DLAA or DLSS on each eye as the
// mode a run starts in (runtime/nvn/vulkan/vr_dlaa.h; the '9' key cycles none, DLAA and DLSS)
typedef enum {
    PC_VR_ANTIALIASING_OFF, PC_VR_ANTIALIASING_DLAA, PC_VR_ANTIALIASING_DLSS, PC_VR_ANTIALIASING_COUNT
} PcVrAntialiasing;
#define PC_VR_ANTIALIASING_DEFAULT PC_VR_ANTIALIASING_DLSS

// The savagewind.ini keys, in the order they are written; a bit each in PcVrConfig's settings_ignored when a value
// was given and not taken.
enum {
    PC_VR_SETTING_VR, PC_VR_SETTING_EYE_SCALE, PC_VR_SETTING_GAME_UNITS_PER_METER, PC_VR_SETTING_MENU_DISTANCE,
    PC_VR_SETTING_HUD_DEPTH, PC_VR_SETTING_MODEL_DISTANCE, PC_VR_SETTING_HEADSET_PACE, PC_VR_SETTING_PACE_OFFSET_MS,
    PC_VR_SETTING_POSE_WAIT_MS, PC_VR_SETTING_DESKTOP_PICTURE, PC_VR_SETTING_ANTIALIASING, PC_VR_SETTING_DLSS_RATIO,
    PC_VR_SETTING_DLSS_EYE_SCALE, PC_VR_SETTING_SOUND_HRTF, PC_VR_SETTING_COUNT
};
// (runtime/pc_vr_settings.inc) The keys ("vr", "vr_eye_scale", ...) and vr_antialiasing's words, in PcVrAntialiasing
// order ("off", "dlaa", "dlss")
extern const char *const pc_vr_setting_keys[PC_VR_SETTING_COUNT];
extern const char *const pc_vr_antialiasing_names[PC_VR_ANTIALIASING_COUNT];

typedef struct {
    bool enabled;  // vr = 1: PC VR, in its play configuration
    bool shader_variants;  // PC VR's own shader translations (the field haze): with PC VR, or without its session by
                           // SAVAGEWIND_PC_VR_SHADERS=1 (the shader precompilation and the pipeline seed's export)
    // The settings, savagewind.ini's vr_<name>.
    struct {
        double eye_scale;         // vr_eye_scale: the Labo eye targets' scale, within [1, 5]; 1: not scaled
        double native_view_units_per_meter;  // vr_game_units_per_meter: the world scale (live: pc_vr_world_scale)
        double menu_distance;     // vr_menu_distance: the menus' quad, metres ahead (live: pc_vr_panel_depth_adjust)
        double hud_depth;         // vr_hud_depth: the HUD's quad, times the camera target's distance (live as well)
        double model_distance;    // vr_model_distance: a model's level of detail by its distance from the point between
                                  // the eyes times this (runtime/hle/pc_vr_cull_view.c), within [0.25, 2]; 0: by the
                                  // game's own measure, the depth along the view, which changes as the head turns
        unsigned headset_pace;    // vr_headset_pace, 0 to 6: the game's frames on the headset's grid, each at least
                                  // this many display frames (runtime/pc_vr_pace.h); 0: off, the game's own pacing
        int64_t headset_pace_offset_ns;  // vr_pace_offset_ms, -50 to 50: with the pace, a frame starts this long after
                                         // a display frame does, before it when negative
        uint64_t pose_wait_budget_ns;    // vr_pose_wait_ms, 0 to 20: with the pace, the frame's first eye call waits
                                         // for the latest display frame's pose that leaves the game this long before
                                         // its next present (runtime/pc_vr_pace.h); 0: no wait
        bool desktop_picture;     // vr_desktop_picture: the left eye drawn in the desktop window; 0: nothing is drawn
                                  // there, as while it is minimised, and the left eye is not copied for it
        PcVrAntialiasing antialiasing;  // vr_antialiasing: off, dlaa or dlss
        double dlss_ratio;        // vr_dlss_ratio, above 1 to 3: the DLSS mode's eyes rendered this many times smaller
                                  // per axis
        double dlss_eye_scale;    // vr_dlss_eye_scale, above 1 to 5: the DLSS mode's eye scale (the size the headset
                                  // is given)
        bool sound_hrtf;          // vr_sound_hrtf, on by default: each positional sound rendered binaurally at its
                                  // own position (runtime/hle/pc_vr_sound_hrtf.c; with audio_output = headphones_3d);
                                  // 0: the game's panning only
    } settings;
    // Not settings of their own: the head pose reaches the game every display frame with the pace (or, without it, by
    // SAVAGEWIND_PC_VR_DISPLAY_RATE_TRACKING=1), and pace 2 with it targets alternate refreshes (D128).
    bool display_rate_tracking;
    bool target_refresh;
    // The diagnostics, launch options SAVAGEWIND_PC_VR_<name>, all off by default.
    struct {
        bool perf;                // PERF, any value: the XR owner's timing windows
        uint64_t shot_presents[PC_VR_SHOT_PRESENTS];  // SHOT=<present>[,...]
        unsigned shot_count;
        bool quick_menu_shots;    // QUICK_MENU_SHOTS=1
        bool bench_head_requested;  // BENCH_HEAD given (benchmark only)
        uint64_t bench_head_start;  // its game present, 1..100000; 0: off, or not a present in that range
        bool log_panes;           // SAVAGEWIND_LOG_PANES: a number above 0
        bool log_screens;         // SAVAGEWIND_LOG_SCREENS: a number above 0
    } diagnostics;
    unsigned settings_ignored;  // a bit per setting whose value was not taken
} PcVrConfig;

// The configuration from a name lookup: lookup(context, name) is the value of a savagewind.ini key ("vr_eye_scale")
// or of a launch option ("SAVAGEWIND_PC_VR_SHOT"), or NULL when it is not set. A pure function of what the lookup
// answers; the process's lookup is savagewind.ini's lines and the environment (pc_vr_config).
typedef const char *(*PcVrConfigLookup)(const void *context, const char *name);
void pc_vr_config_parse(PcVrConfigLookup lookup, const void *context, PcVrConfig *out);

// The configuration as the "[pc-vr] configuration:" line shows it (without that prefix), into text (size bytes, cut
// off when longer). Returns the length it needed.
size_t pc_vr_config_describe(const PcVrConfig *config, char *text, size_t size);

// (runtime/pc_vr_settings.inc, a part of runtime/settings.c) A line of savagewind.ini that is PC VR's: kept for the
// process's configuration. False for any other key.
bool pc_vr_settings_take(const char *key, const char *value);
// The value savagewind.ini gave a key, or NULL when the file does not have it
const char *pc_vr_settings_value(const char *key);
// PC VR's lines, written into savagewind.ini: every key, as read, or with its default when the file did not have it
void pc_vr_settings_write(FILE *file);

// The process's configuration, read at the first call (any thread; savagewind.ini is loaded first) and constant after.
const PcVrConfig *pc_vr_config(void);

#ifdef __cplusplus
}
#endif
