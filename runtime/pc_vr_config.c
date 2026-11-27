// PC VR's configuration: the parse of its settings and diagnostics, and the process's one read of them. Interface and
// the settings themselves: runtime/pc_vr_config.h; their lines in savagewind.ini: runtime/pc_vr_settings.inc.
#include "pc_vr_config.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "settings.h"

static bool exactly_one(const char *value) { return value && !strcmp(value, "1"); }

// A decimal with nothing else around it, finite.
static bool explicit_number(const char *text, double *out) {
    if (!text || !*text || strspn(text, "0123456789.+-eE") != strlen(text)) return false;
    errno = 0;
    char *end = NULL;
    const double value = strtod(text, &end);
    if (errno == ERANGE || end == text || *end || !isfinite(value)) return false;
    *out = value;
    return true;
}

// A list of game presents, "<present>[,<present>...]": the first capacity of them that are not 0, up to the first part
// that is not a number. Returns how many.
static unsigned present_list(const char *text, uint64_t *presents, unsigned capacity) {
    unsigned count = 0;
    for (const char *cursor = text; cursor && *cursor && count < capacity;) {
        char *end = NULL;
        const unsigned long long present = strtoull(cursor, &end, 10);
        if (end == cursor) break;
        if (present) presents[count++] = present;
        cursor = *end == ',' ? end + 1 : end + strlen(end);
    }
    return count;
}

// A setting's number: the value given when it is a number within [minimum, maximum] (above minimum when exclusive),
// else the default, and the setting marked ignored when a value was given.
static double number_setting(PcVrConfigLookup lookup, const void *context, unsigned setting, double fallback,
                             double minimum, bool exclusive, double maximum, PcVrConfig *out) {
    const char *value = lookup(context, pc_vr_setting_keys[setting]);
    double given;
    if (!value) return fallback;
    if (explicit_number(value, &given) && (exclusive ? given > minimum : given >= minimum) && given <= maximum)
        return given;
    out->settings_ignored |= 1u << setting;
    return fallback;
}

// A switch, as upstream reads its own (runtime/settings.c): on for a number other than 0.
static bool switch_setting(PcVrConfigLookup lookup, const void *context, unsigned setting, bool fallback) {
    const char *value = lookup(context, pc_vr_setting_keys[setting]);
    return value ? atoi(value) != 0 : fallback;
}

void pc_vr_config_parse(PcVrConfigLookup lookup, const void *context, PcVrConfig *out) {
    memset(out, 0, sizeof *out);
    out->enabled = switch_setting(lookup, context, PC_VR_SETTING_VR, false);
    out->shader_variants = out->enabled || exactly_one(lookup(context, "SAVAGEWIND_PC_VR_SHADERS"));

    // The settings.
    out->settings.eye_scale = number_setting(lookup, context, PC_VR_SETTING_EYE_SCALE, PC_VR_EYE_SCALE_DEFAULT,
        PC_VR_EYE_SCALE_MIN, false, PC_VR_EYE_SCALE_MAX, out);
    out->settings.native_view_units_per_meter = number_setting(lookup, context, PC_VR_SETTING_GAME_UNITS_PER_METER,
        PC_VR_GAME_UNITS_PER_METER_DEFAULT, 0, true, INFINITY, out);
    out->settings.menu_distance = number_setting(lookup, context, PC_VR_SETTING_MENU_DISTANCE,
        PC_VR_MENU_DISTANCE_DEFAULT, PC_VR_MENU_DISTANCE_MIN, false, PC_VR_MENU_DISTANCE_MAX, out);
    out->settings.hud_depth = number_setting(lookup, context, PC_VR_SETTING_HUD_DEPTH, PC_VR_HUD_DEPTH_DEFAULT,
        PC_VR_HUD_DEPTH_MIN, false, PC_VR_HUD_DEPTH_MAX, out);
    // (0: the game's own measure; otherwise a factor within its range)
    const char *value = lookup(context, pc_vr_setting_keys[PC_VR_SETTING_MODEL_DISTANCE]);
    double given = 0;
    out->settings.model_distance = PC_VR_MODEL_DISTANCE_DEFAULT;
    if (value && explicit_number(value, &given) &&
        (given == 0 || (given >= PC_VR_MODEL_DISTANCE_MIN && given <= PC_VR_MODEL_DISTANCE_MAX)))
        out->settings.model_distance = given;
    else if (value) out->settings_ignored |= 1u << PC_VR_SETTING_MODEL_DISTANCE;
    const double pace = number_setting(lookup, context, PC_VR_SETTING_HEADSET_PACE, PC_VR_HEADSET_PACE_DEFAULT, 0,
        false, PC_VR_HEADSET_PACE_MAX, out);
    if (pace == floor(pace)) out->settings.headset_pace = (unsigned)pace;
    else {  // (whole display frames only)
        out->settings.headset_pace = PC_VR_HEADSET_PACE_DEFAULT;
        out->settings_ignored |= 1u << PC_VR_SETTING_HEADSET_PACE;
    }
    out->settings.headset_pace_offset_ns = llround(number_setting(lookup, context, PC_VR_SETTING_PACE_OFFSET_MS,
        PC_VR_PACE_OFFSET_MS_DEFAULT, -PC_VR_PACE_OFFSET_MS_LIMIT, false, PC_VR_PACE_OFFSET_MS_LIMIT, out) * 1e6);
    out->settings.pose_wait_budget_ns = (uint64_t)llround(number_setting(lookup, context, PC_VR_SETTING_POSE_WAIT_MS,
        PC_VR_POSE_WAIT_MS_DEFAULT, 0, false, PC_VR_POSE_WAIT_MS_MAX, out) * 1e6);
    out->settings.desktop_picture = switch_setting(lookup, context, PC_VR_SETTING_DESKTOP_PICTURE, false);
    value = lookup(context, pc_vr_setting_keys[PC_VR_SETTING_ANTIALIASING]);
    out->settings.antialiasing = PC_VR_ANTIALIASING_DEFAULT;
    if (value) {
        unsigned mode = 0;
        while (mode < PC_VR_ANTIALIASING_COUNT && strcmp(value, pc_vr_antialiasing_names[mode])) ++mode;
        if (mode < PC_VR_ANTIALIASING_COUNT) out->settings.antialiasing = (PcVrAntialiasing)mode;
        else out->settings_ignored |= 1u << PC_VR_SETTING_ANTIALIASING;
    }
    out->settings.dlss_ratio = number_setting(lookup, context, PC_VR_SETTING_DLSS_RATIO, PC_VR_DLSS_RATIO_DEFAULT, 1,
        true, PC_VR_DLSS_RATIO_MAX, out);
    out->settings.dlss_eye_scale = number_setting(lookup, context, PC_VR_SETTING_DLSS_EYE_SCALE,
        PC_VR_DLSS_EYE_SCALE_DEFAULT, 1, true, PC_VR_DLSS_EYE_SCALE_MAX, out);
    out->settings.sound_hrtf = switch_setting(lookup, context, PC_VR_SETTING_SOUND_HRTF, true);

    out->display_rate_tracking = out->settings.headset_pace ||
                                 exactly_one(lookup(context, "SAVAGEWIND_PC_VR_DISPLAY_RATE_TRACKING"));
    out->target_refresh = out->settings.headset_pace == 2 && out->display_rate_tracking;

    // The diagnostics.
    out->diagnostics.perf = lookup(context, "SAVAGEWIND_PC_VR_PERF") != NULL;
    out->diagnostics.shot_count = present_list(lookup(context, "SAVAGEWIND_PC_VR_SHOT"),
        out->diagnostics.shot_presents, PC_VR_SHOT_PRESENTS);
    out->diagnostics.quick_menu_shots = exactly_one(lookup(context, "SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS"));
    value = lookup(context, "SAVAGEWIND_PC_VR_BENCH_HEAD");
    out->diagnostics.bench_head_requested = value != NULL;
    const long long bench_start = value ? atoll(value) : 0;
    out->diagnostics.bench_head_start = bench_start > 0 && bench_start <= 100000 ? (uint64_t)bench_start : 0;
    value = lookup(context, "SAVAGEWIND_LOG_PANES");
    out->diagnostics.log_panes = value && atoi(value) > 0;
    value = lookup(context, "SAVAGEWIND_LOG_SCREENS");
    out->diagnostics.log_screens = value && atoi(value) > 0;
}

// Appends to a line (append keeps the length it needed, past size when cut off).
typedef struct {
    char *text;
    size_t size, length;
} DescribeLine;

static void describe_append(DescribeLine *line, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void describe_append(DescribeLine *line, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const size_t room = line->length < line->size ? line->size - line->length : 0;
    const int written = vsnprintf(room ? line->text + line->length : NULL, room, format, arguments);
    va_end(arguments);
    if (written > 0) line->length += (size_t)written;
}

static void describe_presents(DescribeLine *line, const char *name, const uint64_t *presents, unsigned count) {
    describe_append(line, " %s=", name);
    for (unsigned index = 0; index < count; ++index)
        describe_append(line, "%s%llu", index ? "," : "", (unsigned long long)presents[index]);
}

// A setting's value as savagewind.ini holds it.
static void setting_value(const PcVrConfig *config, unsigned setting, char *text, size_t size) {
    switch (setting) {
    case PC_VR_SETTING_VR: snprintf(text, size, "%d", config->enabled); break;
    case PC_VR_SETTING_EYE_SCALE: snprintf(text, size, "%.9g", config->settings.eye_scale); break;
    case PC_VR_SETTING_GAME_UNITS_PER_METER:
        snprintf(text, size, "%.9g", config->settings.native_view_units_per_meter);
        break;
    case PC_VR_SETTING_MENU_DISTANCE: snprintf(text, size, "%.9g", config->settings.menu_distance); break;
    case PC_VR_SETTING_HUD_DEPTH: snprintf(text, size, "%.9g", config->settings.hud_depth); break;
    case PC_VR_SETTING_MODEL_DISTANCE: snprintf(text, size, "%.9g", config->settings.model_distance); break;
    case PC_VR_SETTING_HEADSET_PACE: snprintf(text, size, "%u", config->settings.headset_pace); break;
    case PC_VR_SETTING_PACE_OFFSET_MS:
        snprintf(text, size, "%.6g", config->settings.headset_pace_offset_ns / 1e6);
        break;
    case PC_VR_SETTING_POSE_WAIT_MS: snprintf(text, size, "%.6g", config->settings.pose_wait_budget_ns / 1e6); break;
    case PC_VR_SETTING_DESKTOP_PICTURE: snprintf(text, size, "%d", config->settings.desktop_picture); break;
    case PC_VR_SETTING_ANTIALIASING:
        snprintf(text, size, "%s", pc_vr_antialiasing_names[config->settings.antialiasing]);
        break;
    case PC_VR_SETTING_DLSS_RATIO: snprintf(text, size, "%.9g", config->settings.dlss_ratio); break;
    case PC_VR_SETTING_DLSS_EYE_SCALE: snprintf(text, size, "%.9g", config->settings.dlss_eye_scale); break;
    case PC_VR_SETTING_SOUND_HRTF: snprintf(text, size, "%d", config->settings.sound_hrtf); break;
    default: snprintf(text, size, "?"); break;
    }
}

size_t pc_vr_config_describe(const PcVrConfig *config, char *text, size_t size) {
    DescribeLine line = {.text = text, .size = size};
    if (size) text[0] = 0;
    char value[32];
    for (unsigned setting = PC_VR_SETTING_VR + 1; setting < PC_VR_SETTING_COUNT; ++setting) {
        setting_value(config, setting, value, sizeof value);
        describe_append(&line, "%s%s=%s", setting > PC_VR_SETTING_VR + 1 ? " " : "", pc_vr_setting_keys[setting],
                        value);
    }
    describe_append(&line, "; head pose every %s%s", config->display_rate_tracking ? "display frame" : "game present",
                    config->target_refresh ? ", alternate refreshes targeted" : "");

    describe_append(&line, "; diagnostics:");
    const size_t diagnostics_start = line.length;
    if (config->diagnostics.perf) describe_append(&line, " PERF");
    if (config->diagnostics.shot_count)
        describe_presents(&line, "SHOT", config->diagnostics.shot_presents, config->diagnostics.shot_count);
    if (config->diagnostics.quick_menu_shots) describe_append(&line, " QUICK_MENU_SHOTS");
    if (config->diagnostics.bench_head_start)
        describe_append(&line, " BENCH_HEAD=%llu", (unsigned long long)config->diagnostics.bench_head_start);
    else if (config->diagnostics.bench_head_requested) describe_append(&line, " BENCH_HEAD=invalid");
    if (config->diagnostics.log_panes) describe_append(&line, " LOG_PANES");
    if (config->diagnostics.log_screens) describe_append(&line, " LOG_SCREENS");
    if (line.length == diagnostics_start) describe_append(&line, " none");

    describe_append(&line, "; ignored:");
    const size_t ignored_start = line.length;
    for (unsigned setting = 0; setting < PC_VR_SETTING_COUNT; ++setting)
        if (config->settings_ignored >> setting & 1) describe_append(&line, " %s", pc_vr_setting_keys[setting]);
    if (line.length == ignored_start) describe_append(&line, " none");
    return line.length;
}

// ------------------------------------------------------------------------------- the process's configuration
static pthread_once_t process_configuration_once = PTHREAD_ONCE_INIT;
static PcVrConfig process_configuration;  // filled by process_configuration_read, never written after
// Set once the configuration is read: the readers' fast path (some read it per draw), without pthread_once's call.
static const PcVrConfig *_Atomic process_configuration_ready;

// The settings from savagewind.ini, the diagnostics from the environment (the launch options).
static const char *process_setting(const void *context, const char *name) {
    (void)context;
    return strncmp(name, "SAVAGEWIND_", 11) ? pc_vr_settings_value(name) : getenv(name);
}

static void process_configuration_read(void) {
    settings_load();  // (once per process, before anything else as a rule; it hands PC VR its lines)
    pc_vr_config_parse(process_setting, NULL, &process_configuration);
    if (process_configuration.enabled) {
        char text[1024];
        pc_vr_config_describe(&process_configuration, text, sizeof text);
        LOG("[pc-vr] configuration: %s\n", text);
    }
    atomic_store_explicit(&process_configuration_ready, &process_configuration, memory_order_release);
}

const PcVrConfig *pc_vr_config(void) {
    const PcVrConfig *ready = atomic_load_explicit(&process_configuration_ready, memory_order_acquire);
    if (ready) return ready;
    pthread_once(&process_configuration_once, process_configuration_read);
    return &process_configuration;
}
