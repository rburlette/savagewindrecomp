// PC VR's configuration (runtime/pc_vr_config.c) and the launch settings runtime/pc_vr.c answers from it, asset-free.
// From the fixture's own settings (pc_vr_config_parse): vr as upstream reads a switch; PC VR's shader translations with
// it or on their own, switching nothing else on; the settings' defaults, which are the play settings; savagewind.ini's
// lines taken, written back and read again; the units-per-metre override; every setting's accepted and refused values;
// the diagnostics; the configuration line. Then, through pc_vr.c, the exact eye size with raster matching and camera
// preservation (runtime/nvn/output_scale.c) and the quads' depths. With "environment": the process's one read of its
// settings file's lines and the environment, constant after it (its own process, as at startup).
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "test_game_sites_182.h"
#include "../runtime/pc_vr.c"
#include "../runtime/nvn/output_scale.c"
#include "test_pc_vr_settings.h"

Settings g_settings = {.fov = 100, .upscaler = UPSCALE_OFF};
volatile int g_movie_playing;
bool framepace_in_event(void) { return false; }
bool framepace_title_ui(void) { return false; }
bool framepace_menu_ui(void) { return false; }
bool framepace_loading_ui(void) { return false; }
bool framepace_map_ui(void) { return false; }
uint64_t framepace_shown_frame(void) { return 0; }
bool fsr3_available(void) { return false; }
bool fsr4_available(void) { return false; }
bool vk_dlss_available(void) { return false; }
bool vk_dlssg_available(void) { return false; }  // (output_scale.c: upstream's DLSS frame generation)
int vk_dlssg_max_mult(void) { return 2; }
bool xess_available(void) { return false; }
bool xess_usable(void) { return false; }
float winsize_refresh_rate(void) { return 0; }
uint64_t renderer_present_count(void) { return 0; }

const GameLibTables *gamelib_tables(void) { return NULL; }
Module *loader_module(const char *name) { (void)name; return NULL; }
uint64_t loader_find_symbol_in(const char *module, const char *name) { (void)module; (void)name; abort(); }
Cpu *kernel_current_cpu(void) { abort(); }
void guest_call(Cpu *cpu, uint64_t address) { (void)cpu; (void)address; abort(); }
uint64_t plat_now_ns(void) { return 1000000000ull; }
bool labo_motion_active(void) { abort(); }
void rt_fatal_trace(void) { abort(); }
void pc_vr_motion_contract_timestamp_reset(void) { abort(); }
// (what an eye shot's request reads and marks, runtime/pc_vr.c: nothing here asks for one)
bool control_place_line(char *line, size_t size) { (void)line; (void)size; abort(); }
XrPoseReadResult xr_runtime_get_pose(XrPoseSnapshot *out) { (void)out; abort(); }
void pc_vr_frame_trace_user_mark(void) { abort(); }
// The OpenXR runtime's recommended eye size (0: discovery has not run), as xr_runtime.c reports it.
static unsigned mock_view_width, mock_view_height;
bool xr_runtime_recommended_eye_size(unsigned *width, unsigned *height) {
    *width = mock_view_width;
    *height = mock_view_height;
    return mock_view_width && mock_view_height;
}

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "PC VR configuration check failed line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

// (upstream's own settings that output_scale.c reads, and the process's read in "environment")
static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// One setting on top of vr = 1.
static PcVrConfig with_setting(const char *name, const char *value) {
    PcVrConfig config;
    test_pc_vr_parse((const char *const[]){"vr", "1", name, value, NULL}, &config);
    return config;
}

static bool same_values(const PcVrConfig *first, const PcVrConfig *second) {
    return first->enabled == second->enabled &&
           !memcmp(&first->settings, &second->settings, sizeof first->settings) &&
           first->display_rate_tracking == second->display_rate_tracking &&
           first->target_refresh == second->target_refresh &&
           !memcmp(&first->diagnostics, &second->diagnostics, sizeof first->diagnostics);
}

// vr: on for a number other than 0, as upstream reads its switches; and the settings only PC VR takes.
static void test_enabled(void) {
    const char *off[] = {NULL, "", "0", "true", "no", "00"};
    for (unsigned i = 0; i < sizeof off / sizeof off[0]; ++i) {
        PcVrConfig config;
        test_pc_vr_parse((const char *const[]){"vr", off[i], "SAVAGEWIND_PC_VR_EYE_SIZE", "2064x2208", NULL}, &config);
        CHECK(!config.enabled && !config.diagnostics.eye_width && !config.diagnostics.eye_height);
        CHECK(!config.diagnostics.eye_size_malformed);
    }
    const char *on[] = {"1", "01", "2", " 1", "1 "};
    for (unsigned i = 0; i < sizeof on / sizeof on[0]; ++i) CHECK(with_setting("vr", on[i]).enabled);
    // The readers in pc_vr.c answer the process's configuration.
    test_pc_vr_configure((const char *const[]){"vr", "1", NULL});
    CHECK(pc_vr_enabled());
    test_pc_vr_configure((const char *const[]){"vr", "true", NULL});
    CHECK(!pc_vr_enabled());
}

// PC VR's shader translations: with PC VR whatever their own setting says, or by SAVAGEWIND_PC_VR_SHADERS exactly 1,
// which leaves PC VR off (no session) and every other value as without it.
static void test_shader_variants(void) {
    const char *values[] = {NULL, "", "0", "true", "01", "1 ", "2", "1"};
    PcVrConfig empty;
    test_pc_vr_parse((const char *const[]){NULL}, &empty);
    CHECK(!empty.enabled && !empty.shader_variants);
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; ++i) {
        PcVrConfig config;
        test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR_SHADERS", values[i], NULL}, &config);
        CHECK(config.shader_variants == (values[i] && !strcmp(values[i], "1")));
        CHECK(!config.enabled && same_values(&config, &empty));
        test_pc_vr_parse((const char *const[]){"vr", "1", "SAVAGEWIND_PC_VR_SHADERS", values[i], NULL},
                         &config);
        CHECK(config.enabled && config.shader_variants);
    }
    // The readers in pc_vr.c answer the process's configuration.
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR_SHADERS", "1", NULL});
    CHECK(pc_vr_shader_variants() && !pc_vr_enabled());
    test_pc_vr_configure((const char *const[]){"vr", "1", NULL});
    CHECK(pc_vr_shader_variants() && pc_vr_enabled());
    test_pc_vr_configure((const char *const[]){NULL});
    CHECK(!pc_vr_shader_variants() && !pc_vr_enabled());
}

// vr = 1 alone: the play settings (D130, D132) and every diagnostic off.
static void test_defaults(void) {
    PcVrConfig config, off;
    test_pc_vr_parse((const char *const[]){"vr", "1", NULL}, &config);
    CHECK(config.enabled);
    CHECK(config.settings.eye_scale == 2.5 && config.settings.native_view_units_per_meter == 14.53);
    CHECK(config.settings.menu_distance == 0.77 && config.settings.hud_depth == 0.9);
    CHECK(config.settings.model_distance == 0.8);
    CHECK(config.settings.headset_pace == 2 && config.settings.headset_pace_offset_ns == 1500000);
    CHECK(config.settings.pose_wait_budget_ns == 8500000 && !config.settings.desktop_picture);
    CHECK(config.settings.antialiasing == PC_VR_ANTIALIASING_DLSS);
    CHECK(config.settings.dlss_ratio == 1.5 && config.settings.dlss_eye_scale == 3.0);
    CHECK(config.display_rate_tracking && config.target_refresh);
    CHECK(config.diagnostics.eye_images_flip == -1 && !config.diagnostics.eye_width && !config.diagnostics.eye_height);
    CHECK(!config.diagnostics.eye_size_malformed && !config.diagnostics.ui_parallax);
    CHECK(!config.diagnostics.hud_backdrop && config.diagnostics.world_hud && !config.diagnostics.world_hud_upright);
    CHECK(config.diagnostics.world_hud_test_turn == 0 && config.diagnostics.rune_aim == PC_VR_RUNE_AIM_ON);
    CHECK(!config.diagnostics.perf);
    CHECK(!config.diagnostics.offer_trace && !config.diagnostics.shot_count && !config.diagnostics.quick_menu_shots);
    CHECK(!config.diagnostics.bench_head_requested && !config.diagnostics.log_panes && !config.diagnostics.log_screens);
    CHECK(!config.diagnostics.no_headset_mask && !config.diagnostics.copy_wait_ms && !config.settings_ignored);
    // (PC VR off: the same values, unused)
    test_pc_vr_parse((const char *const[]){NULL}, &off);
    CHECK(!off.enabled && !memcmp(&off.settings, &config.settings, sizeof off.settings));
}

// savagewind.ini's lines (runtime/pc_vr_settings.inc): PC VR takes its own keys only, writes every key back (as read,
// or its default), and what it writes parses to what it read.
static void test_ini_lines(void) {
    CHECK(!pc_vr_settings_value("vr") && !pc_vr_settings_value("vr_eye_scale"));
    CHECK(!pc_vr_settings_take("anisotropy", "16x") && !pc_vr_settings_take("vr_unknown", "1"));
    CHECK(!pc_vr_settings_take("VR", "1") && !pc_vr_settings_value("VR"));
    CHECK(pc_vr_settings_take("vr", "1") && !strcmp(pc_vr_settings_value("vr"), "1"));
    CHECK(pc_vr_settings_take("vr_menu_distance", "30"));  // (written back as read, ignored at the parse)
    FILE *file = tmpfile();
    if (!file) file = fopen("test_pc_vr_config.ini", "w+");
    CHECK(file != NULL);
    pc_vr_settings_write(file);
    rewind(file);
    char written[1024] = "";
    const size_t length = fread(written, 1, sizeof written - 1, file);
    written[length] = 0;
    fclose(file);
    remove("test_pc_vr_config.ini");
    const char *expected = "vr = 1\nvr_eye_scale = 2.5\nvr_game_units_per_meter = 14.53\nvr_menu_distance = 30\n"
        "vr_hud_depth = 0.9\nvr_model_distance = 0.8\nvr_headset_pace = 2\nvr_pace_offset_ms = 1.5\n"
        "vr_pose_wait_ms = 8.5\nvr_desktop_picture = 0\nvr_antialiasing = dlss\nvr_dlss_ratio = 1.5\n"
        "vr_dlss_eye_scale = 3\n";
    CHECK(!strcmp(written, expected));
    // (each line back through the settings file's own reading: the parse of every default is the play configuration)
    const char *lines[2 * PC_VR_SETTING_COUNT + 1];
    char keys[PC_VR_SETTING_COUNT][32], values[PC_VR_SETTING_COUNT][32];
    unsigned count = 0;
    for (char *line = strtok(written, "\n"); line && count < PC_VR_SETTING_COUNT; line = strtok(NULL, "\n"), ++count) {
        CHECK(sscanf(line, " %31[^= ] = %31[^\r\n]", keys[count], values[count]) == 2);
        lines[2 * count] = keys[count];
        lines[2 * count + 1] = !strcmp(keys[count], "vr_menu_distance") ? "0.77" : values[count];
    }
    lines[2 * count] = NULL;
    CHECK(count == PC_VR_SETTING_COUNT);
    PcVrConfig reread, plain;
    test_pc_vr_parse(lines, &reread);
    test_pc_vr_parse((const char *const[]){"vr", "1", NULL}, &plain);
    CHECK(same_values(&reread, &plain) && !reread.settings_ignored);
}

// vr_game_units_per_meter: a finite positive number replaces 14.53; anything else is ignored.
static void test_units(void) {
    CHECK(with_setting("vr_game_units_per_meter", "20").settings.native_view_units_per_meter == 20);
    CHECK(with_setting("vr_game_units_per_meter", "14.53").settings.native_view_units_per_meter == 14.53);
    CHECK(with_setting("vr_game_units_per_meter", "2.5e1").settings.native_view_units_per_meter == 25);
    CHECK(!with_setting("vr_game_units_per_meter", "20").settings_ignored);
    const char *bad[] = {"", "0", "-1", "nan", "inf", "1x", " 1", "1 ", "0x1p0", "1e999", "1e-999", "-"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        const PcVrConfig config = with_setting("vr_game_units_per_meter", bad[i]);
        CHECK(config.settings.native_view_units_per_meter == 14.53);
        CHECK(config.settings_ignored == 1u << PC_VR_SETTING_GAME_UNITS_PER_METER);
    }
}

// A number setting's cases: the value, what it gives, and whether it is ignored (the default kept).
typedef struct { const char *value; double expected; bool ignored; } NumberCase;

static void check_numbers(const char *key, unsigned setting, const NumberCase *cases, unsigned count,
                          double (*read)(const PcVrConfig *)) {
    for (unsigned i = 0; i < count; ++i) {
        const PcVrConfig config = with_setting(key, cases[i].value);
        CHECK(read(&config) == cases[i].expected);
        CHECK(config.settings_ignored == (cases[i].ignored ? 1u << setting : 0));
    }
}

static double menu_of(const PcVrConfig *config) { return config->settings.menu_distance; }
static double hud_of(const PcVrConfig *config) { return config->settings.hud_depth; }
static double eye_scale_of(const PcVrConfig *config) { return config->settings.eye_scale; }
static double model_of(const PcVrConfig *config) { return config->settings.model_distance; }
static double pace_of(const PcVrConfig *config) { return config->settings.headset_pace; }
static double offset_of(const PcVrConfig *config) { return (double)config->settings.headset_pace_offset_ns; }
static double wait_of(const PcVrConfig *config) { return (double)config->settings.pose_wait_budget_ns; }
static double ratio_of(const PcVrConfig *config) { return config->settings.dlss_ratio; }
static double dlss_scale_of(const PcVrConfig *config) { return config->settings.dlss_eye_scale; }

#define CASES(list) list, sizeof list / sizeof list[0]

// Each setting's accepted and refused values.
static void test_settings(void) {
    const NumberCase menu[] = {
        {"2", 2, false}, {"7", 7, false}, {"0.1", 0.1, false}, {"20", 20, false}, {"0.77", 0.77, false},
        {"30", 0.77, true}, {"0.05", 0.77, true}, {"", 0.77, true}, {"abc", 0.77, true}, {"nan", 0.77, true}};
    check_numbers("vr_menu_distance", PC_VR_SETTING_MENU_DISTANCE, CASES(menu), menu_of);
    const NumberCase hud[] = {{"1.5", 1.5, false}, {"4", 4, false}, {"0.1", 0.1, false}, {"5", 0.9, true},
                              {"", 0.9, true}};
    check_numbers("vr_hud_depth", PC_VR_SETTING_HUD_DEPTH, CASES(hud), hud_of);
    // (within [1, 5]; 1 is not scaled)
    const NumberCase scales[] = {
        {"3", 3, false}, {"2.4", 2.4, false}, {"5", 5, false}, {"1.001", 1.001, false}, {"1", 1, false},
        {"3e0", 3, false}, {"5.1", 2.5, true}, {"0.5", 2.5, true}, {"3x", 2.5, true}, {" 3", 2.5, true},
        {"-3", 2.5, true}, {"", 2.5, true}, {".", 2.5, true}};
    check_numbers("vr_eye_scale", PC_VR_SETTING_EYE_SCALE, CASES(scales), eye_scale_of);
    // (a model's distance for its level of detail: a factor in [0.25, 2], or 0 for the game's own measure)
    const NumberCase model[] = {
        {"0.8", 0.8, false}, {"1", 1, false}, {"0.25", 0.25, false}, {"2", 2, false}, {"0", 0, false},
        {"0.6", 0.6, false}, {"0.2", 0.8, true}, {"2.5", 0.8, true}, {"-1", 0.8, true}, {"", 0.8, true},
        {"abc", 0.8, true}, {"nan", 0.8, true}};
    check_numbers("vr_model_distance", PC_VR_SETTING_MODEL_DISTANCE, CASES(model), model_of);
    // (the pace: whole display frames, 0 to 6)
    const NumberCase pace[] = {{"0", 0, false}, {"1", 1, false}, {"6", 6, false}, {"2", 2, false},
        {"7", 2, true}, {"-2", 2, true}, {"2.5", 2, true}, {"abc", 2, true}, {"", 2, true}};
    check_numbers("vr_headset_pace", PC_VR_SETTING_HEADSET_PACE, CASES(pace), pace_of);
    const NumberCase offsets[] = {
        {"4.25", 4250000, false}, {"2.5", 2500000, false}, {"50", 50000000, false}, {"-3", -3000000, false},
        {"-50", -50000000, false}, {"0", 0, false}, {"-1.5", -1500000, false}, {"50.5", 1500000, true},
        {"-50.5", 1500000, true}, {"abc", 1500000, true}, {"", 1500000, true}};
    check_numbers("vr_pace_offset_ms", PC_VR_SETTING_PACE_OFFSET_MS, CASES(offsets), offset_of);
    const NumberCase waits[] = {{"8.5", 8500000, false}, {"0", 0, false}, {"20", 20000000, false},
        {"20.5", 8500000, true}, {"-1", 8500000, true}, {"abc", 8500000, true}};
    check_numbers("vr_pose_wait_ms", PC_VR_SETTING_POSE_WAIT_MS, CASES(waits), wait_of);
    // (DLSS's render ratio, above 1 to 3, and its eye scale, above 1 to 5)
    const NumberCase ratios[] = {{"1.5", 1.5, false}, {"3", 3, false}, {"2", 2, false}, {"1", 1.5, true},
        {"0.5", 1.5, true}, {"3.01", 1.5, true}, {"-2", 1.5, true}, {"abc", 1.5, true}, {"", 1.5, true},
        {"1.5x", 1.5, true}};
    check_numbers("vr_dlss_ratio", PC_VR_SETTING_DLSS_RATIO, CASES(ratios), ratio_of);
    const NumberCase dlss_scales[] = {{"3.0", 3, false}, {"5", 5, false}, {"2.5", 2.5, false}, {"1", 3, true},
        {"5.5", 3, true}, {"0", 3, true}, {"abc", 3, true}, {"", 3, true}};
    check_numbers("vr_dlss_eye_scale", PC_VR_SETTING_DLSS_EYE_SCALE, CASES(dlss_scales), dlss_scale_of);
    // vr_antialiasing: its three words, anything else ignored
    const struct { const char *value; PcVrAntialiasing mode; bool ignored; } modes[] = {
        {"off", PC_VR_ANTIALIASING_OFF, false}, {"dlaa", PC_VR_ANTIALIASING_DLAA, false},
        {"dlss", PC_VR_ANTIALIASING_DLSS, false}, {"DLSS", PC_VR_ANTIALIASING_DLSS, true},
        {"none", PC_VR_ANTIALIASING_DLSS, true}, {"", PC_VR_ANTIALIASING_DLSS, true}};
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        const PcVrConfig config = with_setting("vr_antialiasing", modes[i].value);
        CHECK(config.settings.antialiasing == modes[i].mode);
        CHECK(config.settings_ignored == (modes[i].ignored ? 1u << PC_VR_SETTING_ANTIALIASING : 0));
    }
    // vr_desktop_picture: a switch, off by default
    CHECK(with_setting("vr_desktop_picture", "1").settings.desktop_picture);
    CHECK(!with_setting("vr_desktop_picture", "0").settings.desktop_picture);
    CHECK(!with_setting("vr_desktop_picture", "yes").settings.desktop_picture);
    // (parsed without PC VR as well, for savagewind.ini; the renderer scales the eye targets only in PC VR)
    PcVrConfig desktop;
    test_pc_vr_parse((const char *const[]){"vr_eye_scale", "2", NULL}, &desktop);
    CHECK(!desktop.enabled && desktop.settings.eye_scale == 2);
    // The head pose every display frame: with the pace, or without it by its launch option (exactly 1)
    CHECK(with_setting("vr_headset_pace", "1").display_rate_tracking);
    CHECK(!with_setting("vr_headset_pace", "0").display_rate_tracking);
    PcVrConfig tracked;
    test_pc_vr_parse((const char *const[]){"vr", "1", "vr_headset_pace", "0",
        "SAVAGEWIND_PC_VR_DISPLAY_RATE_TRACKING", "1", NULL}, &tracked);
    CHECK(tracked.display_rate_tracking && !tracked.target_refresh);
    test_pc_vr_parse((const char *const[]){"vr", "1", "vr_headset_pace", "0",
        "SAVAGEWIND_PC_VR_DISPLAY_RATE_TRACKING", "true", NULL}, &tracked);
    CHECK(!tracked.display_rate_tracking);
}

// The diagnostics: each one on its own, no setting changes.
static void test_diagnostics(void) {
    PcVrConfig plain;
    test_pc_vr_parse((const char *const[]){"vr", "1", NULL}, &plain);
    PcVrConfig config = with_setting("SAVAGEWIND_PC_VR_PERF", "0");  // (any value)
    CHECK(config.diagnostics.perf && !memcmp(&config.settings, &plain.settings, sizeof plain.settings));
    CHECK(with_setting("SAVAGEWIND_PC_VR_OFFER_TRACE", "1").diagnostics.offer_trace);
    CHECK(!with_setting("SAVAGEWIND_PC_VR_OFFER_TRACE", "2").diagnostics.offer_trace);
    // Alternate refreshes targeted: pace 2 with the head pose every display frame, unless TARGET_REFRESH says other
    // than 1
    const struct { const char *pace, *tracking, *target; bool enabled; } target_cases[] = {
        {"2", NULL, NULL, true}, {"2", "1", "1", true}, {"2", NULL, "0", false}, {"2", NULL, "true", false},
        {"0", "1", "1", false}, {"1", NULL, NULL, false}, {"3", "1", "1", false}};
    for (unsigned i = 0; i < sizeof target_cases / sizeof target_cases[0]; ++i) {
        test_pc_vr_parse((const char *const[]){"vr", "1", "vr_headset_pace", target_cases[i].pace,
            "SAVAGEWIND_PC_VR_DISPLAY_RATE_TRACKING", target_cases[i].tracking,
            "SAVAGEWIND_PC_VR_TARGET_REFRESH", target_cases[i].target, NULL}, &config);
        CHECK(config.target_refresh == target_cases[i].enabled);
        char target_line[1024];
        pc_vr_config_describe(&config, target_line, sizeof target_line);
        CHECK((strstr(target_line, "alternate refreshes targeted") != NULL) == target_cases[i].enabled);
    }
    config = with_setting("SAVAGEWIND_PC_VR_SHOT", "5498");
    CHECK(config.diagnostics.shot_count == 1 && config.diagnostics.shot_presents[0] == 5498);
    config = with_setting("SAVAGEWIND_PC_VR_SHOT", "1,2,3,4,5,6,7,8,9");
    CHECK(config.diagnostics.shot_count == 8 && config.diagnostics.shot_presents[7] == 8);
    // (a present list skips zeros and stops at the first part that is not a number)
    config = with_setting("SAVAGEWIND_PC_VR_SHOT", "100,0,300");
    CHECK(config.diagnostics.shot_count == 2 && config.diagnostics.shot_presents[1] == 300);
    CHECK(!with_setting("SAVAGEWIND_PC_VR_SHOT", "abc").diagnostics.shot_count);
    config = with_setting("SAVAGEWIND_PC_VR_SHOT", "7x,9");
    CHECK(config.diagnostics.shot_count == 1 && config.diagnostics.shot_presents[0] == 7);
    CHECK(with_setting("SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS", "1").diagnostics.quick_menu_shots);
    CHECK(!with_setting("SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS", "yes").diagnostics.quick_menu_shots);
    CHECK(with_setting("SAVAGEWIND_PC_VR_NO_HEADSET_MASK", "1").diagnostics.no_headset_mask);
    CHECK(!with_setting("SAVAGEWIND_PC_VR_NO_HEADSET_MASK", "yes").diagnostics.no_headset_mask);
    // The XR owner's first wait for a copy: its own unless 1 to 100 milliseconds.
    CHECK(with_setting("SAVAGEWIND_PC_VR_COPY_WAIT_MS", "100").diagnostics.copy_wait_ms == 100);
    CHECK(with_setting("SAVAGEWIND_PC_VR_COPY_WAIT_MS", "1").diagnostics.copy_wait_ms == 1);
    const char *bad_copy_wait[] = {"0", "101", "-3", "abc", ""};
    for (unsigned i = 0; i < sizeof bad_copy_wait / sizeof bad_copy_wait[0]; ++i)
        CHECK(!with_setting("SAVAGEWIND_PC_VR_COPY_WAIT_MS", bad_copy_wait[i]).diagnostics.copy_wait_ms);
    config = with_setting("SAVAGEWIND_PC_VR_BENCH_HEAD", "3000");
    CHECK(config.diagnostics.bench_head_requested && config.diagnostics.bench_head_start == 3000);
    const char *bad_bench[] = {"0", "100001", "-5", "abc", ""};
    for (unsigned i = 0; i < sizeof bad_bench / sizeof bad_bench[0]; ++i) {
        config = with_setting("SAVAGEWIND_PC_VR_BENCH_HEAD", bad_bench[i]);
        CHECK(config.diagnostics.bench_head_requested && !config.diagnostics.bench_head_start);
    }
    CHECK(with_setting("SAVAGEWIND_LOG_PANES", "1").diagnostics.log_panes);
    CHECK(!with_setting("SAVAGEWIND_LOG_PANES", "0").diagnostics.log_panes);
    CHECK(with_setting("SAVAGEWIND_LOG_SCREENS", "2").diagnostics.log_screens);
    CHECK(!with_setting("SAVAGEWIND_LOG_SCREENS", "-1").diagnostics.log_screens);
    config = with_setting("SAVAGEWIND_LOG_SCREENS", "1");
    CHECK(!memcmp(&config.settings, &plain.settings, sizeof plain.settings) && !config.settings_ignored);
    // EYE_IMAGES_FLIP: 0 or 1, otherwise the window present's order
    const struct { const char *value; int expected; } flips[] = {
        {"0", 0}, {"1", 1}, {"2", -1}, {"auto", -1}, {"", -1}, {"01", -1}};
    for (unsigned i = 0; i < sizeof flips / sizeof flips[0]; ++i) {
        config = with_setting("SAVAGEWIND_PC_VR_EYE_IMAGES_FLIP", flips[i].value);
        CHECK(config.diagnostics.eye_images_flip == flips[i].expected && !config.settings_ignored);
    }
    // EYE_SIZE, PC VR only: a malformed one stops startup (the process's read), an empty one is none.
    config = with_setting("SAVAGEWIND_PC_VR_EYE_SIZE", "2064x2208");
    CHECK(config.diagnostics.eye_width == 2064 && config.diagnostics.eye_height == 2208);
    CHECK(!config.diagnostics.eye_size_malformed);
    config = with_setting("SAVAGEWIND_PC_VR_EYE_SIZE", "");
    CHECK(!config.diagnostics.eye_width && !config.diagnostics.eye_height && !config.diagnostics.eye_size_malformed);
    const char *bad_sizes[] = {"0x2208", "2064x0", "2064", "2064X2208", " 2064x2208", "112x2208",
        "4112x2208", "2064x2208junk"};
    for (unsigned i = 0; i < sizeof bad_sizes / sizeof bad_sizes[0]; ++i) {
        config = with_setting("SAVAGEWIND_PC_VR_EYE_SIZE", bad_sizes[i]);
        CHECK(config.diagnostics.eye_size_malformed && !config.diagnostics.eye_width);
        CHECK(!config.diagnostics.eye_height);
    }
    // The A/B switches that take exactly 1.
    CHECK(with_setting("SAVAGEWIND_PC_VR_UI_PARALLAX", "1").diagnostics.ui_parallax);
    CHECK(!with_setting("SAVAGEWIND_PC_VR_UI_PARALLAX", "0").diagnostics.ui_parallax);
    CHECK(with_setting("SAVAGEWIND_PC_VR_HUD_BACKDROP", "1").diagnostics.hud_backdrop);
    CHECK(!with_setting("SAVAGEWIND_PC_VR_HUD_BACKDROP", "01").diagnostics.hud_backdrop);
    // WORLD_HUD: only exactly 0 is off (an empty value is a real one here, unlike Windows' environment).
    const struct { const char *value; bool on; } world[] = {
        {NULL, true}, {"0", false}, {"1", true}, {"00", true}, {"0 ", true}, {"", true}};
    for (unsigned i = 0; i < sizeof world / sizeof world[0]; ++i)
        CHECK(with_setting("SAVAGEWIND_PC_VR_WORLD_HUD", world[i].value).diagnostics.world_hud == world[i].on);
    // WORLD_HUD_ROLL: unset turns with the world's up, 0 upright, a number of degrees more (in radians here).
    const struct { const char *value; bool upright; double degrees; } rolls[] = {
        {NULL, false, 0}, {"0", true, 0}, {"90", false, 90}, {"-45", false, -45}, {"abc", false, 0}, {"", false, 0}};
    for (unsigned i = 0; i < sizeof rolls / sizeof rolls[0]; ++i) {
        config = with_setting("SAVAGEWIND_PC_VR_WORLD_HUD_ROLL", rolls[i].value);
        CHECK(config.diagnostics.world_hud_upright == rolls[i].upright);
        CHECK(config.diagnostics.world_hud_test_turn == rolls[i].degrees * (3.14159265358979323846 / 180.0));
    }
    // RUNE_AIM: 0 off, flip, anything else on.
    const struct { const char *value; PcVrRuneAim aim; } aims[] = {
        {NULL, PC_VR_RUNE_AIM_ON}, {"0", PC_VR_RUNE_AIM_OFF}, {"flip", PC_VR_RUNE_AIM_FLIP}, {"1", PC_VR_RUNE_AIM_ON},
        {"FLIP", PC_VR_RUNE_AIM_ON}, {"", PC_VR_RUNE_AIM_ON}};
    for (unsigned i = 0; i < sizeof aims / sizeof aims[0]; ++i)
        CHECK(with_setting("SAVAGEWIND_PC_VR_RUNE_AIM", aims[i].value).diagnostics.rune_aim == aims[i].aim);
}

// The "[pc-vr] configuration:" line: vr = 1 alone (what the play runs' settings file holds), ignored settings with
// diagnostics, every other diagnostic, and a line cut off.
static void test_describe(void) {
    PcVrConfig config;
    test_pc_vr_parse((const char *const[]){"vr", "1", NULL}, &config);
    char text[1024];
    const char *play = "vr_eye_scale=2.5 vr_game_units_per_meter=14.53 vr_menu_distance=0.77 vr_hud_depth=0.9 "
        "vr_model_distance=0.8 vr_headset_pace=2 vr_pace_offset_ms=1.5 vr_pose_wait_ms=8.5 vr_desktop_picture=0 "
        "vr_antialiasing=dlss vr_dlss_ratio=1.5 vr_dlss_eye_scale=3; head pose every display frame, alternate "
        "refreshes targeted; diagnostics: none; ignored: none";
    CHECK(pc_vr_config_describe(&config, text, sizeof text) == strlen(play) && !strcmp(text, play));
    printf("[pc-vr] configuration: %s\n", text);
    test_pc_vr_parse((const char *const[]){"vr", "1", "vr_menu_distance", "30", "vr_headset_pace", "0",
        "vr_game_units_per_meter", "x", "SAVAGEWIND_PC_VR_EYE_SIZE", "2064x2208",
        "SAVAGEWIND_PC_VR_WORLD_HUD_ROLL", "90", "SAVAGEWIND_PC_VR_RUNE_AIM", "flip", NULL}, &config);
    pc_vr_config_describe(&config, text, sizeof text);
    CHECK(strstr(text, " vr_menu_distance=0.77 ") && strstr(text, " vr_headset_pace=0 "));
    CHECK(strstr(text, "; head pose every game present; diagnostics: EYE_SIZE=2064x2208 WORLD_HUD_ROLL=90 "
                       "RUNE_AIM=flip; ignored: vr_game_units_per_meter vr_menu_distance"));
    test_pc_vr_parse((const char *const[]){"vr", "1", "SAVAGEWIND_PC_VR_PERF", "1", "SAVAGEWIND_PC_VR_OFFER_TRACE", "1", "SAVAGEWIND_PC_VR_SHOT", "5498",
        "SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS", "1", "SAVAGEWIND_PC_VR_BENCH_HEAD", "x", "SAVAGEWIND_LOG_PANES", "1",
        "SAVAGEWIND_PC_VR_WORLD_HUD_ROLL", "0", NULL}, &config);
    pc_vr_config_describe(&config, text, sizeof text);
    CHECK(strstr(text, "; diagnostics: PERF OFFER_TRACE SHOT=5498 "
        "QUICK_MENU_SHOTS BENCH_HEAD=invalid LOG_PANES WORLD_HUD_ROLL=0; ignored: none"));
    // (cut off: the length it needed, and a terminated text)
    char short_text[16];
    memset(short_text, 'x', sizeof short_text);
    CHECK(pc_vr_config_describe(&config, short_text, sizeof short_text) == strlen(text));
    CHECK(short_text[15] == 0 && !strncmp(short_text, text, 15));
}

// The exact eye size through pc_vr.c and output_scale.c: one size for every renderer and XR thread, the original 800x900
// eye chains and the final image's halves on identical pixels, the camera's FOV kept.
static void test_eye_size(void) {
    set_env("SAVAGEWIND_SCALE", "2");  // (upstream's final-image scale, read once by output_scale.c: the exact size wins)
    set_env("SAVAGEWIND_WIDE_TEST", "");
    const struct { const char *name; const char *const *settings; bool enabled, exact; } modes[] = {
        {"eye-size", (const char *const[]){"vr", "1", "SAVAGEWIND_PC_VR_EYE_SIZE", "2064x2208", NULL},
         true, true},
        {"eye-size-default", (const char *const[]){"vr", "1", "SAVAGEWIND_PC_VR_EYE_SIZE", "", NULL},
         true, false},
        {"eye-size-off", (const char *const[]){"vr", "0", "SAVAGEWIND_PC_VR_EYE_SIZE", "2064x2208", NULL},
         false, false},
    };
    for (unsigned mode = 0; mode < sizeof modes / sizeof modes[0]; ++mode) {
        test_pc_vr_configure(modes[mode].settings);
        const bool exact = modes[mode].exact, enabled = modes[mode].enabled;
        unsigned width = 42, height = 42;
        CHECK(pc_vr_eye_size(&width, &height) == exact);
        CHECK(width == (exact ? 2064u : 0u) && height == (exact ? 2208u : 0u));
        OutScale out;
        output_scale(3840, 2400, true, &out);
        // (desktop: upstream widens or narrows the views to the window's shape, here 16:10; PC VR keeps 16:9)
        CHECK(lround(1920 * out.sx) == (exact ? 4128 : enabled ? 3840 : 3456));
        CHECK(lround(1080 * out.sy) == (exact ? 2208 : 2160));
        if (exact) {
            // Production texture allocation uses float scales and lroundf. The 800x900 eye targets and the
            // 960x1080 halves of the final image must land on identical pixels: the eye's finished scene is placed
            // in its half 1:1. (Only at full size: their fractions are unrelated targets.)
            CHECK(lroundf(800.0f * ((float)width / 800)) == lroundf(960.0f * (float)out.sx));
            CHECK(lroundf(900.0f * ((float)height / 900)) == lroundf(1080.0f * (float)out.sy));
            CHECK(lround(1600 * out.scene_sx) == 4128 && lround(900 * out.scene_sy) == 2208);
            CHECK(fabs(out.vw / out.vh - 4128.0 / 2208.0) < 1e-12);
            CHECK(out.shape == 1 && g_view_shape == 1 && g_fov == 1); // raster size cannot widen native camera FOV
            CHECK(out.crop[0] == 0 && out.crop[1] == 0 && out.crop[2] == 1 && out.crop[3] == 1);
            CHECK(!output_ui_needs_centering((float)out.sx, (float)out.sy, pc_vr_enabled()));
            CHECK(output_ui_needs_centering((float)out.sx, (float)out.sy, false)); // desktop ultrawide stays centered
            CHECK(!output_ui_needs_centering(2, 2, false));
        }
    }
    const char *bad[] = {NULL, "", "0x2208", "2064x0", "2064", "2064x", "x2208", "2064X2208",
        "2064x2208 ", " 2064x2208", "+2064x2208", "02064x2208", "2064x2208junk", "2064x2208x16",
        "112x2208", "4112x2208", "999999999999999999999x2208"};
    unsigned width, height;
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        width = height = 42;
        CHECK(!pc_vr_parse_eye_size(bad[i], &width, &height) && width == 0 && height == 0);
    }
    CHECK(pc_vr_parse_eye_size("128x128", &width, &height) && width == 128 && height == 128);
    CHECK(pc_vr_parse_eye_size("4096x4096", &width, &height) && width == 4096 && height == 4096);
    CHECK(pc_vr_parse_eye_size("2451x2644", &width, &height) && width == 2451 && height == 2644);
}

// The eye size from an eye scale in the OpenXR runtime's view shape: the scale's pixels of 800x900, whole pixels.
static void test_eye_size_from_view(void) {
    unsigned width = 42, height = 42;
    // Link's recommended 1824x1968 (0.927): EyeScale 3.0's 6,480,000 pixels, 2400x2700 in Labo's shape.
    CHECK(pc_vr_eye_size_for_view(3.0, 1824, 1968, &width, &height) && width == 2451 && height == 2644);
    CHECK(fabs((double)width * height / 6480000.0 - 1.0) < 0.001);
    CHECK(fabs((double)width / height - 1824.0 / 1968.0) < 0.001);
    CHECK(pc_vr_eye_size_for_view(2.4, 1824, 1968, &width, &height) && width == 1961 && height == 2115);
    CHECK(pc_vr_eye_size_for_view(3.0, 800, 900, &width, &height) && width == 2400 && height == 2700);
    CHECK(pc_vr_eye_size_for_view(5.0, 100, 4000, &width, &height) && width == 671 && height == 4096);
    CHECK(!pc_vr_eye_size_for_view(1.0, 1824, 1968, &width, &height) && !width && !height);
    CHECK(!pc_vr_eye_size_for_view(3.0, 0, 1968, &width, &height) && !width && !height);
    // Through pc_vr.c: only in PC VR, only once the runtime is discovered; an explicit EYE_SIZE wins.
    const char *const scaled[] = {"vr", "1", "vr_eye_scale", "3", NULL};
    test_pc_vr_configure(scaled);
    mock_view_width = mock_view_height = 0;
    CHECK(!pc_vr_eye_size(&width, &height));
    mock_view_width = 1824, mock_view_height = 1968;
    CHECK(pc_vr_eye_size(&width, &height) && width == 2451 && height == 2644);
    test_pc_vr_configure((const char *const[]){"vr", "1", "vr_eye_scale", "3",
                                               "SAVAGEWIND_PC_VR_EYE_SIZE", "2064x2208", NULL});
    CHECK(pc_vr_eye_size(&width, &height) && width == 2064 && height == 2208);
    test_pc_vr_configure((const char *const[]){"vr", "0", "vr_eye_scale", "3", NULL});
    CHECK(!pc_vr_eye_size(&width, &height));
    test_pc_vr_configure((const char *const[]){"vr", "1", "vr_eye_scale", "1", NULL});
    CHECK(!pc_vr_eye_size(&width, &height));
    // (vr = 1 alone: the play settings' 2.5 in the view's shape)
    test_pc_vr_configure((const char *const[]){"vr", "1", NULL});
    unsigned play_width = 0, play_height = 0;
    CHECK(pc_vr_eye_size_for_view(2.5, 1824, 1968, &play_width, &play_height));
    CHECK(pc_vr_eye_size(&width, &height) && width == play_width && height == play_height);
    mock_view_width = mock_view_height = 0;
}

// Quad depths (menus D056 in metres, HUD D057 as a multiple of the camera target's distance): latched at the first
// read, 25% live steps clamped to each range (0.1..20 m, 0.1..4), 0 restores the launch value, each panel on its own;
// nothing without PC VR's eye images. 2D content reaches the headset only on quads (D068), so neither panel can be
// turned off.
static void test_panel_depths(void) {
    test_pc_vr_configure((const char *const[]){"vr", "1", "vr_menu_distance", "2",
                                               "vr_hud_depth", "", NULL});
    CHECK(pc_vr_panel_depth(PC_VR_PANEL_MENU) == 2 && pc_vr_panel_depth(PC_VR_PANEL_HUD) == 0.9);
    test_pc_vr_configure((const char *const[]){"vr", "1", "vr_menu_distance", "7", NULL});
    CHECK(pc_vr_panel_depth(PC_VR_PANEL_MENU) == 2);  // (latched at the first read)
    CHECK(fabs(pc_vr_panel_depth_adjust(PC_VR_PANEL_MENU, 0.8) - 1.6) < 1e-12);
    CHECK(fabs(pc_vr_panel_depth(PC_VR_PANEL_MENU) - 1.6) < 1e-12 && pc_vr_panel_depth(PC_VR_PANEL_HUD) == 0.9);
    for (unsigned i = 0; i < 40; ++i) (void)pc_vr_panel_depth_adjust(PC_VR_PANEL_MENU, 0.8);
    CHECK(pc_vr_panel_depth(PC_VR_PANEL_MENU) == 0.1);
    for (unsigned i = 0; i < 40; ++i) (void)pc_vr_panel_depth_adjust(PC_VR_PANEL_MENU, 1.25);
    CHECK(pc_vr_panel_depth(PC_VR_PANEL_MENU) == 20);
    CHECK(pc_vr_panel_depth_adjust(PC_VR_PANEL_MENU, 0) == 2);
    CHECK(fabs(pc_vr_panel_depth_adjust(PC_VR_PANEL_HUD, 1.25) - 1.125) < 1e-12);
    for (unsigned i = 0; i < 40; ++i) (void)pc_vr_panel_depth_adjust(PC_VR_PANEL_HUD, 1.25);
    CHECK(pc_vr_panel_depth(PC_VR_PANEL_HUD) == 4);
    for (unsigned i = 0; i < 40; ++i) (void)pc_vr_panel_depth_adjust(PC_VR_PANEL_HUD, 0.8);
    CHECK(pc_vr_panel_depth(PC_VR_PANEL_HUD) == 0.1 && pc_vr_panel_depth_adjust(PC_VR_PANEL_HUD, 0) == 0.9);
    CHECK(pc_vr_panel_depth(PC_VR_PANEL_MENU) == 2 && pc_vr_panel_depth_adjust((PcVrPanel)2, 0.8) == 0);
    // The HUD distance: the target at once without a previous one or after a long gap, eased in log distance
    // (one time constant covers 1 - 1/e of the way), clamped, and held without a focus distance.
    CHECK(fabs(pc_vr_hud_distance(0.9, 0.4, 0, 0.03) - 0.36) < 1e-12);
    CHECK(fabs(pc_vr_hud_distance(0.9, 0.4, 1.5, INFINITY) - 0.36) < 1e-12);
    CHECK(fabs(pc_vr_hud_distance(1, 0.2, 0.2, 0.03) - 0.2) < 1e-12);
    const double eased = pc_vr_hud_distance(1, 0.8, 0.2, 0.25);
    CHECK(fabs(log(eased / 0.2) - (1 - exp(-1.0)) * log(4.0)) < 1e-12);
    CHECK(fabs(pc_vr_hud_distance(1, 0.8, 0.2, 0) - 0.2) < 1e-12);
    CHECK(pc_vr_hud_distance(0.1, 0.2, 0, 1) == 0.1 && pc_vr_hud_distance(4, 9, 0, 1) == 20);
    CHECK(fabs(pc_vr_hud_distance(0.9, 0, 0.5, 0.03) - 0.5) < 1e-12 && pc_vr_hud_distance(0.9, NAN, 0, 0.03) == 0.77);
    CHECK(fabs(pc_vr_hud_distance(0.9, 0.4, 0.5, NAN) - 0.36) < 1e-12);
}

// Without PC VR's eye images (PC VR off), no quad. (The panels latch at their first read, as a process does once; the
// fixture forgets that read afterwards for test_panel_depths.)
static void test_panels_off(void) {
    test_pc_vr_configure((const char *const[]){"vr_menu_distance", "2", NULL});
    CHECK(pc_vr_panel_depth(PC_VR_PANEL_MENU) == 0 && pc_vr_panel_depth(PC_VR_PANEL_HUD) == 0);
    CHECK(pc_vr_panel_depth_adjust(PC_VR_PANEL_MENU, 1.25) == 0);
    panels[PC_VR_PANEL_MENU].read = panels[PC_VR_PANEL_HUD].read = false;
}

// The process's one read (its own process): savagewind.ini's lines as settings.c hands them over and the launch
// options in the environment, the values given before it, constant after it. A setting's name in the environment and
// a launch option's in the settings file are not read.
static int environment_test(void) {
    CHECK(pc_vr_settings_take("vr", "1") && pc_vr_settings_take("vr_game_units_per_meter", "20"));
    set_env("vr_eye_scale", "3");  // (not a launch option)
    set_env("SAVAGEWIND_PC_VR_EYE_SIZE", "2064x2208");
    set_env("SAVAGEWIND_PC_VR_UI_PARALLAX", "1");
    set_env("SAVAGEWIND_LOG_PANES", "1");
    CHECK(!pc_vr_settings_take("SAVAGEWIND_PC_VR_HUD_BACKDROP", "1"));  // (not a setting)
    const PcVrConfig *config = pc_vr_config();  // (logs its "[pc-vr] configuration:" line)
    CHECK(config->enabled && pc_vr_enabled() && config->diagnostics.ui_parallax && !config->diagnostics.hud_backdrop);
    CHECK(config->settings.native_view_units_per_meter == 20 && config->diagnostics.log_panes);
    CHECK(config->settings.eye_scale == 2.5);
    unsigned width, height;
    CHECK(pc_vr_eye_size(&width, &height) && width == 2064 && height == 2208);
    CHECK(pc_vr_settings_take("vr", "0") && pc_vr_settings_take("vr_game_units_per_meter", "30"));
    set_env("SAVAGEWIND_PC_VR_EYE_SIZE", "1920x2160");
    set_env("SAVAGEWIND_PC_VR_UI_PARALLAX", "0");
    CHECK(pc_vr_config() == config && pc_vr_enabled() && config->diagnostics.ui_parallax);
    CHECK(config->settings.native_view_units_per_meter == 20);
    CHECK(pc_vr_eye_size(&width, &height) && width == 2064 && height == 2208);
    printf("PC VR configuration, its one read: %u checks passed (constant after)\n", checks);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "environment")) return environment_test();
    if (argc != 1) return 2;
    test_enabled();
    test_shader_variants();
    test_defaults();
    test_ini_lines();
    test_units();
    test_settings();
    test_diagnostics();
    test_describe();
    test_panels_off();
    test_eye_size();
    test_eye_size_from_view();
    test_panel_depths();
    printf("PC VR configuration fixture: %u checks passed\n", checks);
    return 0;
}

// hle/labo_guest_memory.h: a new access generation per call keeps its page cache cold here.
uint64_t plat_guest_access_generation(void) { static uint64_t generation; return ++generation; }
