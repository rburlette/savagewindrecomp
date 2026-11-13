// PC VR's configuration: the parse of its settings and the process's one read of them. Interface and the settings
// themselves: runtime/pc_vr_config.h.
#include "pc_vr_config.h"
#include "pc_vr_eye_size.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

// Each alias's environment name and its name in the configuration line, in PC_VR_ALIAS_* order.
static const char *const alias_names[PC_VR_ALIAS_COUNT][2] = {
    {"SAVAGEWIND_PC_VR_HEAD_LOOK", "HEAD_LOOK"},
    {"SAVAGEWIND_PC_VR_PROJECTION_TRACE", "PROJECTION_TRACE"},
    {"SAVAGEWIND_PC_VR_NATIVE_FOV_TRACE", "NATIVE_FOV_TRACE"},
    {"SAVAGEWIND_PC_VR_NATIVE_FOV_EXPERIMENT", "NATIVE_FOV_EXPERIMENT"},
    {"SAVAGEWIND_PC_VR_NATIVE_VIEW_EXPERIMENT", "NATIVE_VIEW_EXPERIMENT"},
    {"SAVAGEWIND_PC_VR_NATIVE_RENDER_TRACE", "NATIVE_RENDER_TRACE"},
    {"SAVAGEWIND_PC_VR_EYE_IMAGES", "EYE_IMAGES"},
    {"SAVAGEWIND_PC_VR_FIRST_LIGHT", "FIRST_LIGHT"},
};

// Each checked play setting's name in the configuration line, in PC_VR_SETTING_* order.
static const char *const setting_names[PC_VR_SETTING_COUNT] = {
    "NATIVE_VIEW_UNITS_PER_METER", "MENU_DISTANCE", "HUD_DEPTH", "EYE_IMAGES_FLIP", "EYE_SCALE",
};

static bool exactly_one(const char *value) { return value && !strcmp(value, "1"); }

// A decimal with nothing else around it, finite and above 0 (the units per metre).
static bool explicit_positive_number(const char *text, double *out) {
    if (!text || !*text || strspn(text, "0123456789.+-eE") != strlen(text)) return false;
    errno = 0;
    char *end = NULL;
    const double value = strtod(text, &end);
    if (errno == ERANGE || end == text || *end || !isfinite(value) || !(value > 0)) return false;
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

// A quad's depth setting: the number given when it is within [minimum, maximum], else the default (and the setting is
// marked ignored when a value was given).
static double panel_setting(const char *value, double fallback, double minimum, double maximum, unsigned setting,
                            PcVrConfig *out) {
    const double given = value ? atof(value) : fallback;
    if (given >= minimum && given <= maximum) return given;
    out->settings_ignored |= 1u << setting;
    return fallback;
}

void pc_vr_config_parse(PcVrConfigLookup lookup, const void *context, PcVrConfig *out) {
    memset(out, 0, sizeof *out);
    out->enabled = exactly_one(lookup(context, "SAVAGEWIND_PC_VR"));

    // The play set.
    const char *value = lookup(context, "SAVAGEWIND_PC_VR_NATIVE_VIEW_UNITS_PER_METER");
    out->play.native_view_units_per_meter = PC_VR_NATIVE_VIEW_UNITS_PER_METER_DEFAULT;
    if (value && !explicit_positive_number(value, &out->play.native_view_units_per_meter))
        out->settings_ignored |= 1u << PC_VR_SETTING_NATIVE_VIEW_UNITS_PER_METER;
    out->play.menu_distance = panel_setting(lookup(context, "SAVAGEWIND_PC_VR_MENU_DISTANCE"),
        PC_VR_MENU_DISTANCE_DEFAULT, PC_VR_MENU_DISTANCE_MIN, PC_VR_MENU_DISTANCE_MAX,
        PC_VR_SETTING_MENU_DISTANCE, out);
    out->play.hud_depth = panel_setting(lookup(context, "SAVAGEWIND_PC_VR_HUD_DEPTH"),
        PC_VR_HUD_DEPTH_DEFAULT, PC_VR_HUD_DEPTH_MIN, PC_VR_HUD_DEPTH_MAX, PC_VR_SETTING_HUD_DEPTH, out);
    value = lookup(context, "SAVAGEWIND_PC_VR_EYE_IMAGES_FLIP");
    out->play.eye_images_flip = value && (!strcmp(value, "0") || !strcmp(value, "1")) ? value[0] == '1' : -1;
    if (value && out->play.eye_images_flip < 0) out->settings_ignored |= 1u << PC_VR_SETTING_EYE_IMAGES_FLIP;
    // (exact decimal text only: 1 is "not scaled" and also not taken, as before)
    value = lookup(context, "SAVAGEWIND_PC_VR_EYE_SCALE");
    char *end = NULL;
    const double scale = value && *value && strspn(value, "0123456789.") == strlen(value) ? strtod(value, &end) : 0;
    out->play.eye_scale = value && end && !*end && scale > 1.0 && scale <= 5.0 ? scale : 1.0;
    if (value && out->play.eye_scale == 1.0) out->settings_ignored |= 1u << PC_VR_SETTING_EYE_SCALE;
    value = lookup(context, "SAVAGEWIND_PC_VR_EYE_SIZE");
    if (out->enabled && value && *value &&
        !pc_vr_parse_eye_size(value, &out->play.eye_width, &out->play.eye_height))
        out->play.eye_size_malformed = true;
    out->play.display_rate_tracking = exactly_one(lookup(context, "SAVAGEWIND_PC_VR_DISPLAY_RATE_TRACKING"));
    out->play.ui_parallax = exactly_one(lookup(context, "SAVAGEWIND_PC_VR_UI_PARALLAX"));
    out->play.hud_backdrop = exactly_one(lookup(context, "SAVAGEWIND_PC_VR_HUD_BACKDROP"));
    value = lookup(context, "SAVAGEWIND_PC_VR_WORLD_HUD");
    out->play.world_hud = !(value && !strcmp(value, "0"));
    value = lookup(context, "SAVAGEWIND_PC_VR_WORLD_HUD_ROLL");
    out->play.world_hud_upright = value && !strcmp(value, "0");
    out->play.world_hud_test_turn = value ? atof(value) * (3.14159265358979323846 / 180.0) : 0;
    value = lookup(context, "SAVAGEWIND_PC_VR_RUNE_AIM");
    out->play.rune_aim = !value ? PC_VR_RUNE_AIM_ON : !strcmp(value, "0") ? PC_VR_RUNE_AIM_OFF :
                         !strcmp(value, "flip") ? PC_VR_RUNE_AIM_FLIP : PC_VR_RUNE_AIM_ON;

    // The diagnostics set.
    out->diagnostics.head_look_timing = exactly_one(lookup(context, "SAVAGEWIND_PC_VR_HEAD_LOOK_TIMING"));
    value = lookup(context, "SAVAGEWIND_PC_VR_PROJECTION_LIST");
    out->diagnostics.projection_list = value ? atoi(value) : 0;
    out->diagnostics.perf = lookup(context, "SAVAGEWIND_PC_VR_PERF") != NULL;
    out->diagnostics.offer_trace = exactly_one(lookup(context, "SAVAGEWIND_PC_VR_OFFER_TRACE"));
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

    // The aliases of the play configuration.
    for (unsigned alias = 0; alias < PC_VR_ALIAS_COUNT; ++alias) {
        value = lookup(context, alias_names[alias][0]);
        if (!value) continue;
        out->aliases_seen |= 1u << alias;
        if (!exactly_one(value)) out->aliases_ignored |= 1u << alias;
    }
}

// Appends to the configuration line (describe_append keeps the length it needed, past size when cut off).
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

size_t pc_vr_config_describe(const PcVrConfig *config, char *text, size_t size) {
    DescribeLine line = {.text = text, .size = size};
    if (size) text[0] = 0;
    describe_append(&line, "NATIVE_VIEW_UNITS_PER_METER=%.9g MENU_DISTANCE=%.9g HUD_DEPTH=%.9g EYE_IMAGES_FLIP=%s",
        config->play.native_view_units_per_meter, config->play.menu_distance, config->play.hud_depth,
        config->play.eye_images_flip < 0 ? "auto" : config->play.eye_images_flip ? "1" : "0");
    describe_append(&line, " EYE_SCALE=%.9g", config->play.eye_scale);
    if (config->play.eye_width)
        describe_append(&line, " EYE_SIZE=%ux%u", config->play.eye_width, config->play.eye_height);
    else describe_append(&line, " EYE_SIZE=none");
    describe_append(&line, " DISPLAY_RATE_TRACKING=%d UI_PARALLAX=%d HUD_BACKDROP=%d",
        config->play.display_rate_tracking, config->play.ui_parallax, config->play.hud_backdrop);
    describe_append(&line, " WORLD_HUD=%d", config->play.world_hud);
    if (config->play.world_hud_upright) describe_append(&line, " WORLD_HUD_ROLL=0");
    else if (config->play.world_hud_test_turn != 0)
        describe_append(&line, " WORLD_HUD_ROLL=%.6g",
                        config->play.world_hud_test_turn * (180.0 / 3.14159265358979323846));
    else describe_append(&line, " WORLD_HUD_ROLL=world");
    describe_append(&line, " RUNE_AIM=%s", config->play.rune_aim == PC_VR_RUNE_AIM_OFF ? "0" :
                    config->play.rune_aim == PC_VR_RUNE_AIM_FLIP ? "flip" : "1");

    describe_append(&line, "; diagnostics:");
    const size_t diagnostics_start = line.length;
    if (config->diagnostics.head_look_timing) describe_append(&line, " HEAD_LOOK_TIMING");
    if (config->diagnostics.projection_list)
        describe_append(&line, " PROJECTION_LIST=%d", config->diagnostics.projection_list);
    if (config->diagnostics.perf) describe_append(&line, " PERF");
    if (config->diagnostics.offer_trace) describe_append(&line, " OFFER_TRACE");
    if (config->diagnostics.shot_count)
        describe_presents(&line, "SHOT", config->diagnostics.shot_presents, config->diagnostics.shot_count);
    if (config->diagnostics.quick_menu_shots) describe_append(&line, " QUICK_MENU_SHOTS");
    if (config->diagnostics.bench_head_start)
        describe_append(&line, " BENCH_HEAD=%llu", (unsigned long long)config->diagnostics.bench_head_start);
    else if (config->diagnostics.bench_head_requested) describe_append(&line, " BENCH_HEAD=invalid");
    if (config->diagnostics.log_panes) describe_append(&line, " LOG_PANES");
    if (config->diagnostics.log_screens) describe_append(&line, " LOG_SCREENS");
    if (line.length == diagnostics_start) describe_append(&line, " none");

    describe_append(&line, "; aliases:");
    const size_t aliases_start = line.length;
    for (unsigned alias = 0; alias < PC_VR_ALIAS_COUNT; ++alias)
        if ((config->aliases_seen & ~config->aliases_ignored) >> alias & 1)
            describe_append(&line, " %s", alias_names[alias][1]);
    if (line.length == aliases_start) describe_append(&line, " none");

    describe_append(&line, "; ignored:");
    const size_t ignored_start = line.length;
    for (unsigned alias = 0; alias < PC_VR_ALIAS_COUNT; ++alias)
        if (config->aliases_ignored >> alias & 1) describe_append(&line, " %s", alias_names[alias][1]);
    for (unsigned setting = 0; setting < PC_VR_SETTING_COUNT; ++setting)
        if (config->settings_ignored >> setting & 1) describe_append(&line, " %s", setting_names[setting]);
    if (line.length == ignored_start) describe_append(&line, " none");
    return line.length;
}

// ------------------------------------------------------------------------------- the process's configuration
static pthread_once_t process_configuration_once = PTHREAD_ONCE_INIT;
static PcVrConfig process_configuration;  // filled by process_configuration_read, never written after
// Set once the configuration is read: the readers' fast path (some read it per draw), without pthread_once's call.
static const PcVrConfig *_Atomic process_configuration_ready;

static const char *environment_setting(const void *context, const char *name) {
    (void)context;
    return getenv(name);
}

static void process_configuration_read(void) {
    pc_vr_config_parse(environment_setting, NULL, &process_configuration);
    if (process_configuration.enabled) {
        if (process_configuration.play.eye_size_malformed)
            FATAL("SAVAGEWIND_PC_VR_EYE_SIZE requires WxH, each axis 128..4096 and a multiple of 16\n");
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
