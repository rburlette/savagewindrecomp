// Part of settings.c (D133): PC VR's lines in savagewind.ini, the vr and vr_* keys (runtime/pc_vr_config.h). Kept as
// settings_load reads them, for PC VR's configuration to parse at its one read (pc_vr_config.c), and written back as
// read, a key the file does not have with its default. Included where it stands by settings.c, so the settings' own
// tests, which include settings.c, have it as well; it needs nothing else.
#include "pc_vr_config.h"

const char *const pc_vr_setting_keys[PC_VR_SETTING_COUNT] = {
    "vr", "vr_eye_scale", "vr_game_units_per_meter", "vr_menu_distance", "vr_hud_depth", "vr_model_distance",
    "vr_headset_pace", "vr_pace_offset_ms", "vr_pose_wait_ms", "vr_desktop_picture", "vr_antialiasing",
    "vr_dlss_ratio", "vr_dlss_eye_scale", "vr_sound_hrtf",
};
const char *const pc_vr_antialiasing_names[PC_VR_ANTIALIASING_COUNT] = {"off", "dlaa", "dlss"};

// Each key's default as written (vr_antialiasing's: an index in pc_vr_antialiasing_names)
static const double pc_vr_setting_defaults[PC_VR_SETTING_COUNT] = {
    [PC_VR_SETTING_VR] = 0,
    [PC_VR_SETTING_EYE_SCALE] = PC_VR_EYE_SCALE_DEFAULT,
    [PC_VR_SETTING_GAME_UNITS_PER_METER] = PC_VR_GAME_UNITS_PER_METER_DEFAULT,
    [PC_VR_SETTING_MENU_DISTANCE] = PC_VR_MENU_DISTANCE_DEFAULT,
    [PC_VR_SETTING_HUD_DEPTH] = PC_VR_HUD_DEPTH_DEFAULT,
    [PC_VR_SETTING_MODEL_DISTANCE] = PC_VR_MODEL_DISTANCE_DEFAULT,
    [PC_VR_SETTING_HEADSET_PACE] = PC_VR_HEADSET_PACE_DEFAULT,
    [PC_VR_SETTING_PACE_OFFSET_MS] = PC_VR_PACE_OFFSET_MS_DEFAULT,
    [PC_VR_SETTING_POSE_WAIT_MS] = PC_VR_POSE_WAIT_MS_DEFAULT,
    [PC_VR_SETTING_DESKTOP_PICTURE] = 0,
    [PC_VR_SETTING_ANTIALIASING] = PC_VR_ANTIALIASING_DEFAULT,
    [PC_VR_SETTING_DLSS_RATIO] = PC_VR_DLSS_RATIO_DEFAULT,
    [PC_VR_SETTING_DLSS_EYE_SCALE] = PC_VR_DLSS_EYE_SCALE_DEFAULT,
    [PC_VR_SETTING_SOUND_HRTF] = 1,
};

// The values read, a slot per key; written by settings_load before the process's configuration is read
static struct {
    char value[64];
    bool given;
} pc_vr_setting_lines[PC_VR_SETTING_COUNT];

bool pc_vr_settings_take(const char *key, const char *value) {
    for (unsigned setting = 0; setting < PC_VR_SETTING_COUNT; ++setting) {
        if (strcmp(key, pc_vr_setting_keys[setting])) continue;
        snprintf(pc_vr_setting_lines[setting].value, sizeof pc_vr_setting_lines[setting].value, "%s", value);
        pc_vr_setting_lines[setting].given = true;
        return true;
    }
    return false;
}

const char *pc_vr_settings_value(const char *key) {
    for (unsigned setting = 0; setting < PC_VR_SETTING_COUNT; ++setting)
        if (!strcmp(key, pc_vr_setting_keys[setting]))
            return pc_vr_setting_lines[setting].given ? pc_vr_setting_lines[setting].value : NULL;
    return NULL;
}

void pc_vr_settings_write(FILE *file) {
    for (unsigned setting = 0; setting < PC_VR_SETTING_COUNT; ++setting) {
        const char *key = pc_vr_setting_keys[setting];
        if (pc_vr_setting_lines[setting].given) fprintf(file, "%s = %s\n", key, pc_vr_setting_lines[setting].value);
        else if (setting == PC_VR_SETTING_ANTIALIASING)
            fprintf(file, "%s = %s\n", key, pc_vr_antialiasing_names[(int)pc_vr_setting_defaults[setting]]);
        else fprintf(file, "%s = %.9g\n", key, pc_vr_setting_defaults[setting]);
    }
}
