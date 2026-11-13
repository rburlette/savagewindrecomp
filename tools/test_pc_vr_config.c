// PC VR's configuration (runtime/pc_vr_config.c) and the launch settings runtime/pc_vr.c answers from it, asset-free.
// From the fixture's own settings (pc_vr_config_parse): SAVAGEWIND_PC_VR exactly 1; the play set's defaults, which are
// the play runs'; the aliases of the play configuration (1 agrees, another value is ignored, and none switches
// anything off); the units-per-metre override; every play setting's accepted and refused values; the diagnostics set;
// the configuration line. Then, through pc_vr.c, the exact eye size with raster matching and camera preservation
// (runtime/nvn/output_scale.c) and the quads' depths. With "environment": the process's one read of the environment,
// constant after it (its own process, as at startup).
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "test_game_sites_182.h"
#include "../runtime/pc_vr.c"
#include "../runtime/nvn/output_scale.c"
#include "test_pc_vr_settings.h"

Settings g_settings = {.fov = 100, .upscaler = UPSCALE_OFF};
bool framepace_in_event(void) { return false; }
bool framepace_title_ui(void) { return false; }
bool framepace_menu_ui(void) { return false; }
bool framepace_loading_ui(void) { return false; }
bool framepace_map_ui(void) { return false; }
uint64_t framepace_shown_frame(void) { return 0; }
bool fsr3_available(void) { return false; }
bool fsr4_available(void) { return false; }
bool vk_dlss_available(void) { return false; }
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

// One setting on top of SAVAGEWIND_PC_VR=1.
static PcVrConfig with_setting(const char *name, const char *value) {
    PcVrConfig config;
    test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1", name, value, NULL}, &config);
    return config;
}

static bool same_values(const PcVrConfig *first, const PcVrConfig *second) {
    return first->enabled == second->enabled && !memcmp(&first->play, &second->play, sizeof first->play) &&
           !memcmp(&first->diagnostics, &second->diagnostics, sizeof first->diagnostics);
}

// SAVAGEWIND_PC_VR exactly 1, and the settings only PC VR takes.
static void test_enabled(void) {
    const char *values[] = {NULL, "", "0", "true", "01", "1 ", " 1", "2"};
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; ++i) {
        PcVrConfig config;
        test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", values[i],
                                               "SAVAGEWIND_PC_VR_EYE_SIZE", "2064x2208", NULL}, &config);
        CHECK(!config.enabled && !config.play.eye_width && !config.play.eye_height);
        CHECK(!config.play.eye_size_malformed);
    }
    // The readers in pc_vr.c answer the process's configuration.
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL});
    CHECK(pc_vr_enabled());
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "true", NULL});
    CHECK(!pc_vr_enabled());
}

// SAVAGEWIND_PC_VR=1 alone: the play set's defaults (the play runs' values) and every diagnostic off.
static void test_defaults(void) {
    PcVrConfig config, off;
    test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL}, &config);
    CHECK(config.enabled);
    CHECK(config.play.native_view_units_per_meter == 14.53);
    CHECK(config.play.menu_distance == 0.77 && config.play.hud_depth == 0.9);
    CHECK(config.play.eye_images_flip == -1 && config.play.eye_scale == 1.0);
    CHECK(!config.play.eye_width && !config.play.eye_height && !config.play.eye_size_malformed);
    CHECK(!config.play.display_rate_tracking && !config.play.ui_parallax && !config.play.labo_mask_off);
    CHECK(!config.play.hud_backdrop && config.play.world_hud && !config.play.world_hud_upright);
    CHECK(config.play.world_hud_test_turn == 0 && config.play.rune_aim == PC_VR_RUNE_AIM_ON);
    const PcVrConfig empty = {0};
    CHECK(!memcmp(&config.diagnostics, &empty.diagnostics, sizeof config.diagnostics));
    CHECK(!config.aliases_seen && !config.aliases_ignored && !config.settings_ignored);
    // (PC VR off: the same values, unused)
    test_pc_vr_parse((const char *const[]){NULL}, &off);
    CHECK(!off.enabled && !memcmp(&off.play, &config.play, sizeof off.play));
}

// The research-era names: 1 agrees with the play configuration, another value is ignored; none changes anything.
static void test_aliases(void) {
    static const char *const names[PC_VR_ALIAS_COUNT] = {
        "SAVAGEWIND_PC_VR_HEAD_LOOK", "SAVAGEWIND_PC_VR_PROJECTION_TRACE", "SAVAGEWIND_PC_VR_NATIVE_FOV_TRACE",
        "SAVAGEWIND_PC_VR_NATIVE_FOV_EXPERIMENT", "SAVAGEWIND_PC_VR_NATIVE_VIEW_EXPERIMENT",
        "SAVAGEWIND_PC_VR_NATIVE_RENDER_TRACE", "SAVAGEWIND_PC_VR_EYE_IMAGES", "SAVAGEWIND_PC_VR_FIRST_LIGHT"};
    PcVrConfig plain;
    test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL}, &plain);
    for (unsigned alias = 0; alias < PC_VR_ALIAS_COUNT; ++alias) {
        const PcVrConfig agreeing = with_setting(names[alias], "1");
        CHECK(agreeing.aliases_seen == 1u << alias && !agreeing.aliases_ignored && same_values(&agreeing, &plain));
        const char *others[] = {"0", "2", "true", ""};
        for (unsigned i = 0; i < sizeof others / sizeof others[0]; ++i) {
            const PcVrConfig ignored = with_setting(names[alias], others[i]);
            CHECK(ignored.aliases_seen == 1u << alias && ignored.aliases_ignored == 1u << alias);
            CHECK(same_values(&ignored, &plain) && !ignored.settings_ignored);
        }
    }
    // Every alias switched off at once still leaves the play configuration whole.
    PcVrConfig all_off;
    test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1", names[0], "0", names[1], "0", names[2], "0",
        names[3], "0", names[4], "0", names[5], "0", names[6], "0", names[7], "0", NULL}, &all_off);
    CHECK(all_off.aliases_seen == 0xff && all_off.aliases_ignored == 0xff && same_values(&all_off, &plain));
    // (and without PC VR an alias turns nothing on)
    PcVrConfig without;
    test_pc_vr_parse((const char *const[]){names[0], "1", names[6], "1", NULL}, &without);
    CHECK(!without.enabled && without.aliases_seen == 0x41 && !without.aliases_ignored);
}

// SAVAGEWIND_PC_VR_NATIVE_VIEW_UNITS_PER_METER: a finite positive decimal replaces 14.53; anything else is ignored.
static void test_units(void) {
    CHECK(with_setting("SAVAGEWIND_PC_VR_NATIVE_VIEW_UNITS_PER_METER", "20").play.native_view_units_per_meter == 20);
    CHECK(with_setting("SAVAGEWIND_PC_VR_NATIVE_VIEW_UNITS_PER_METER", "14.53").play.native_view_units_per_meter == 14.53);
    CHECK(with_setting("SAVAGEWIND_PC_VR_NATIVE_VIEW_UNITS_PER_METER", "2.5e1").play.native_view_units_per_meter == 25);
    CHECK(!with_setting("SAVAGEWIND_PC_VR_NATIVE_VIEW_UNITS_PER_METER", "20").settings_ignored);
    const char *bad[] = {"", "0", "-1", "nan", "inf", "1x", " 1", "1 ", "0x1p0", "1e999", "1e-999", "-"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        const PcVrConfig config = with_setting("SAVAGEWIND_PC_VR_NATIVE_VIEW_UNITS_PER_METER", bad[i]);
        CHECK(config.play.native_view_units_per_meter == 14.53);
        CHECK(config.settings_ignored == 1u << PC_VR_SETTING_NATIVE_VIEW_UNITS_PER_METER);
    }
}

// Each play setting's accepted and refused values.
static void test_play_settings(void) {
    const struct { const char *value; double expected; bool ignored; } menu[] = {
        {"2", 2, false}, {"7", 7, false}, {"0.1", 0.1, false}, {"20", 20, false}, {"0.77", 0.77, false},
        {"30", 0.77, true}, {"0.05", 0.77, true}, {"", 0.77, true}, {"abc", 0.77, true}, {"nan", 0.77, true}};
    for (unsigned i = 0; i < sizeof menu / sizeof menu[0]; ++i) {
        const PcVrConfig config = with_setting("SAVAGEWIND_PC_VR_MENU_DISTANCE", menu[i].value);
        CHECK(config.play.menu_distance == menu[i].expected && config.play.hud_depth == 0.9);
        CHECK(config.settings_ignored == (menu[i].ignored ? 1u << PC_VR_SETTING_MENU_DISTANCE : 0));
    }
    const struct { const char *value; double expected; bool ignored; } hud[] = {
        {"1.5", 1.5, false}, {"4", 4, false}, {"0.1", 0.1, false}, {"5", 0.9, true}, {"", 0.9, true}};
    for (unsigned i = 0; i < sizeof hud / sizeof hud[0]; ++i) {
        const PcVrConfig config = with_setting("SAVAGEWIND_PC_VR_HUD_DEPTH", hud[i].value);
        CHECK(config.play.hud_depth == hud[i].expected && config.play.menu_distance == 0.77);
        CHECK(config.settings_ignored == (hud[i].ignored ? 1u << PC_VR_SETTING_HUD_DEPTH : 0));
    }
    const struct { const char *value; int expected; bool ignored; } flips[] = {
        {"0", 0, false}, {"1", 1, false}, {"2", -1, true}, {"auto", -1, true}, {"", -1, true}, {"01", -1, true}};
    for (unsigned i = 0; i < sizeof flips / sizeof flips[0]; ++i) {
        const PcVrConfig config = with_setting("SAVAGEWIND_PC_VR_EYE_IMAGES_FLIP", flips[i].value);
        CHECK(config.play.eye_images_flip == flips[i].expected);
        CHECK(config.settings_ignored == (flips[i].ignored ? 1u << PC_VR_SETTING_EYE_IMAGES_FLIP : 0));
    }
    // (exact decimal text, in (1, 5]; 1 itself is refused as before)
    const struct { const char *value; double expected; } scales[] = {
        {"3", 3}, {"2.4", 2.4}, {"5", 5}, {"1.001", 1.001}, {"1", 1}, {"5.1", 1}, {"0.5", 1}, {"3x", 1},
        {" 3", 1}, {"-3", 1}, {"3e0", 1}, {"", 1}, {".", 1}};
    for (unsigned i = 0; i < sizeof scales / sizeof scales[0]; ++i) {
        const PcVrConfig config = with_setting("SAVAGEWIND_PC_VR_EYE_SCALE", scales[i].value);
        CHECK(config.play.eye_scale == scales[i].expected);
        CHECK(config.settings_ignored == (scales[i].expected == 1 ? 1u << PC_VR_SETTING_EYE_SCALE : 0));
    }
    // (taken without PC VR as well, as the renderer always has)
    PcVrConfig desktop;
    test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR_EYE_SCALE", "2", NULL}, &desktop);
    CHECK(!desktop.enabled && desktop.play.eye_scale == 2);
    // EYE_SIZE, PC VR only: a malformed one stops startup (the process's read), an empty one is none.
    PcVrConfig config = with_setting("SAVAGEWIND_PC_VR_EYE_SIZE", "2064x2208");
    CHECK(config.play.eye_width == 2064 && config.play.eye_height == 2208 && !config.play.eye_size_malformed);
    config = with_setting("SAVAGEWIND_PC_VR_EYE_SIZE", "");
    CHECK(!config.play.eye_width && !config.play.eye_height && !config.play.eye_size_malformed);
    const char *bad_sizes[] = {"0x2208", "2064x0", "2064", "2064X2208", " 2064x2208", "2065x2208", "112x2208",
        "4112x2208", "2064x2208junk"};
    for (unsigned i = 0; i < sizeof bad_sizes / sizeof bad_sizes[0]; ++i) {
        config = with_setting("SAVAGEWIND_PC_VR_EYE_SIZE", bad_sizes[i]);
        CHECK(config.play.eye_size_malformed && !config.play.eye_width && !config.play.eye_height);
    }
    // The switches that take exactly 1.
    CHECK(with_setting("SAVAGEWIND_PC_VR_DISPLAY_RATE_TRACKING", "1").play.display_rate_tracking);
    CHECK(!with_setting("SAVAGEWIND_PC_VR_DISPLAY_RATE_TRACKING", "true").play.display_rate_tracking);
    CHECK(with_setting("SAVAGEWIND_PC_VR_UI_PARALLAX", "1").play.ui_parallax);
    CHECK(!with_setting("SAVAGEWIND_PC_VR_UI_PARALLAX", "0").play.ui_parallax);
    CHECK(with_setting("SAVAGEWIND_PC_VR_HUD_BACKDROP", "1").play.hud_backdrop);
    CHECK(!with_setting("SAVAGEWIND_PC_VR_HUD_BACKDROP", "01").play.hud_backdrop);
    CHECK(with_setting("SAVAGEWIND_PC_VR_LABO_MASK_OFF", "1").play.labo_mask_off);
    CHECK(!with_setting("SAVAGEWIND_PC_VR_LABO_MASK_OFF", "yes").play.labo_mask_off);
    // WORLD_HUD: only exactly 0 is off (an empty value is a real one here, unlike Windows' environment).
    const struct { const char *value; bool on; } world[] = {
        {NULL, true}, {"0", false}, {"1", true}, {"00", true}, {"0 ", true}, {"", true}};
    for (unsigned i = 0; i < sizeof world / sizeof world[0]; ++i)
        CHECK(with_setting("SAVAGEWIND_PC_VR_WORLD_HUD", world[i].value).play.world_hud == world[i].on);
    // WORLD_HUD_ROLL: unset turns with the world's up, 0 upright, a number of degrees more (in radians here).
    const struct { const char *value; bool upright; double degrees; } rolls[] = {
        {NULL, false, 0}, {"0", true, 0}, {"90", false, 90}, {"-45", false, -45}, {"abc", false, 0}, {"", false, 0}};
    for (unsigned i = 0; i < sizeof rolls / sizeof rolls[0]; ++i) {
        config = with_setting("SAVAGEWIND_PC_VR_WORLD_HUD_ROLL", rolls[i].value);
        CHECK(config.play.world_hud_upright == rolls[i].upright);
        CHECK(config.play.world_hud_test_turn == rolls[i].degrees * (3.14159265358979323846 / 180.0));
    }
    // RUNE_AIM: 0 off, flip, anything else on.
    const struct { const char *value; PcVrRuneAim aim; } aims[] = {
        {NULL, PC_VR_RUNE_AIM_ON}, {"0", PC_VR_RUNE_AIM_OFF}, {"flip", PC_VR_RUNE_AIM_FLIP}, {"1", PC_VR_RUNE_AIM_ON},
        {"FLIP", PC_VR_RUNE_AIM_ON}, {"", PC_VR_RUNE_AIM_ON}};
    for (unsigned i = 0; i < sizeof aims / sizeof aims[0]; ++i)
        CHECK(with_setting("SAVAGEWIND_PC_VR_RUNE_AIM", aims[i].value).play.rune_aim == aims[i].aim);
}

// The diagnostics set: each one on its own, nothing else changes.
static void test_diagnostics(void) {
    PcVrConfig plain;
    test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL}, &plain);
    PcVrConfig config = with_setting("SAVAGEWIND_PC_VR_HEAD_LOOK_TIMING", "1");
    CHECK(config.diagnostics.head_look_timing && !memcmp(&config.play, &plain.play, sizeof plain.play));
    CHECK(!with_setting("SAVAGEWIND_PC_VR_HEAD_LOOK_TIMING", "true").diagnostics.head_look_timing);
    CHECK(with_setting("SAVAGEWIND_PC_VR_PROJECTION_LIST", "2").diagnostics.projection_list == 2);
    CHECK(!with_setting("SAVAGEWIND_PC_VR_PROJECTION_LIST", "x").diagnostics.projection_list);
    CHECK(with_setting("SAVAGEWIND_PC_VR_PERF", "0").diagnostics.perf);  // (any value)
    CHECK(with_setting("SAVAGEWIND_PC_VR_OFFER_TRACE", "1").diagnostics.offer_trace);
    CHECK(!with_setting("SAVAGEWIND_PC_VR_OFFER_TRACE", "2").diagnostics.offer_trace);
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
    CHECK(!memcmp(&config.play, &plain.play, sizeof plain.play) && !config.settings_ignored);
}

// The "[pc-vr] configuration:" line: what play-pc-vr.ps1's environment gives (start-pc-vr.ps1 exports the aliases,
// the units and the quads' depths on every run; play adds the eye scale and the screens log), an ignored alias and
// setting, every diagnostic, and a line cut off.
static void test_describe(void) {
    PcVrConfig config;
    test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1",
        "SAVAGEWIND_PC_VR_HEAD_LOOK", "1", "SAVAGEWIND_PC_VR_PROJECTION_TRACE", "1",
        "SAVAGEWIND_PC_VR_NATIVE_FOV_TRACE", "1", "SAVAGEWIND_PC_VR_NATIVE_FOV_EXPERIMENT", "1",
        "SAVAGEWIND_PC_VR_NATIVE_VIEW_EXPERIMENT", "1", "SAVAGEWIND_PC_VR_NATIVE_VIEW_UNITS_PER_METER", "14.53",
        "SAVAGEWIND_PC_VR_NATIVE_RENDER_TRACE", "1", "SAVAGEWIND_PC_VR_EYE_IMAGES", "1",
        "SAVAGEWIND_PC_VR_FIRST_LIGHT", "1", "SAVAGEWIND_PC_VR_MENU_DISTANCE", "0.77",
        "SAVAGEWIND_PC_VR_HUD_DEPTH", "0.9", "SAVAGEWIND_PC_VR_EYE_SCALE", "3", "SAVAGEWIND_LOG_SCREENS", "1", NULL},
        &config);
    char text[1024];
    const char *play = "NATIVE_VIEW_UNITS_PER_METER=14.53 MENU_DISTANCE=0.77 HUD_DEPTH=0.9 EYE_IMAGES_FLIP=auto "
        "EYE_SCALE=3 EYE_SIZE=none DISPLAY_RATE_TRACKING=0 UI_PARALLAX=0 HUD_BACKDROP=0 LABO_MASK_OFF=0 "
        "WORLD_HUD=1 WORLD_HUD_ROLL=world RUNE_AIM=1; diagnostics: LOG_SCREENS; aliases: HEAD_LOOK PROJECTION_TRACE "
        "NATIVE_FOV_TRACE NATIVE_FOV_EXPERIMENT NATIVE_VIEW_EXPERIMENT NATIVE_RENDER_TRACE EYE_IMAGES FIRST_LIGHT; "
        "ignored: none";
    CHECK(pc_vr_config_describe(&config, text, sizeof text) == strlen(play) && !strcmp(text, play));
    printf("[pc-vr] configuration: %s\n", text);
    test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1", "SAVAGEWIND_PC_VR_EYE_IMAGES", "0",
        "SAVAGEWIND_PC_VR_HEAD_LOOK", "1", "SAVAGEWIND_PC_VR_MENU_DISTANCE", "30",
        "SAVAGEWIND_PC_VR_NATIVE_VIEW_UNITS_PER_METER", "x", "SAVAGEWIND_PC_VR_EYE_SIZE", "2064x2208",
        "SAVAGEWIND_PC_VR_WORLD_HUD_ROLL", "90", "SAVAGEWIND_PC_VR_RUNE_AIM", "flip", NULL}, &config);
    pc_vr_config_describe(&config, text, sizeof text);
    CHECK(strstr(text, " EYE_SIZE=2064x2208 ") && strstr(text, " WORLD_HUD_ROLL=90 ") && strstr(text, " RUNE_AIM=flip;"));
    CHECK(strstr(text, "; diagnostics: none; aliases: HEAD_LOOK; ignored: EYE_IMAGES NATIVE_VIEW_UNITS_PER_METER "
                       "MENU_DISTANCE") && !strstr(text, "aliases: HEAD_LOOK EYE_IMAGES"));
    test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1", "SAVAGEWIND_PC_VR_HEAD_LOOK_TIMING", "1",
        "SAVAGEWIND_PC_VR_PROJECTION_LIST", "2",
        "SAVAGEWIND_PC_VR_PERF", "1", "SAVAGEWIND_PC_VR_OFFER_TRACE", "1", "SAVAGEWIND_PC_VR_SHOT", "5498",
        "SAVAGEWIND_PC_VR_QUICK_MENU_SHOTS", "1", "SAVAGEWIND_PC_VR_BENCH_HEAD", "x", "SAVAGEWIND_LOG_PANES", "1",
        "SAVAGEWIND_PC_VR_WORLD_HUD_ROLL", "0", NULL}, &config);
    pc_vr_config_describe(&config, text, sizeof text);
    CHECK(strstr(text, " WORLD_HUD_ROLL=0 ") && strstr(text, "; diagnostics: HEAD_LOOK_TIMING "
        "PROJECTION_LIST=2 PERF OFFER_TRACE SHOT=5498 QUICK_MENU_SHOTS BENCH_HEAD=invalid LOG_PANES; aliases: none; "
        "ignored: none"));
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
        {"eye-size", (const char *const[]){"SAVAGEWIND_PC_VR", "1", "SAVAGEWIND_PC_VR_EYE_SIZE", "2064x2208", NULL},
         true, true},
        {"eye-size-default", (const char *const[]){"SAVAGEWIND_PC_VR", "1", "SAVAGEWIND_PC_VR_EYE_SIZE", "", NULL},
         true, false},
        {"eye-size-off", (const char *const[]){"SAVAGEWIND_PC_VR", "0", "SAVAGEWIND_PC_VR_EYE_SIZE", "2064x2208", NULL},
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
            // Production texture allocation uses float scales and lroundf. Original 800x900 eye chains and the
            // 960x1080 halves of the final image must land on identical pixels, including their half-size passes.
            for (unsigned div = 1; div <= 16; div *= 2) {
                CHECK(lroundf((800.0f / div) * ((float)width / 800)) == lroundf((960.0f / div) * (float)out.sx));
                CHECK(lroundf((900.0f / div) * ((float)height / 900)) == lroundf((1080.0f / div) * (float)out.sy));
            }
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
        "2065x2208", "2064x2209", "112x2208", "4112x2208", "999999999999999999999x2208"};
    unsigned width, height;
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        width = height = 42;
        CHECK(!pc_vr_parse_eye_size(bad[i], &width, &height) && width == 0 && height == 0);
    }
    CHECK(pc_vr_parse_eye_size("128x128", &width, &height) && width == 128 && height == 128);
    CHECK(pc_vr_parse_eye_size("4096x4096", &width, &height) && width == 4096 && height == 4096);
}

// Quad depths (menus D056 in metres, HUD D057 as a multiple of the camera target's distance): latched at the first
// read, 25% live steps clamped to each range (0.1..20 m, 0.1..4), 0 restores the launch value, each panel on its own;
// nothing without PC VR's eye images. 2D content reaches the headset only on quads (D068), so neither panel can be
// turned off.
static void test_panel_depths(void) {
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1", "SAVAGEWIND_PC_VR_MENU_DISTANCE", "2",
                                               "SAVAGEWIND_PC_VR_HUD_DEPTH", "", NULL});
    CHECK(pc_vr_panel_depth(PC_VR_PANEL_MENU) == 2 && pc_vr_panel_depth(PC_VR_PANEL_HUD) == 0.9);
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1", "SAVAGEWIND_PC_VR_MENU_DISTANCE", "7", NULL});
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
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR_MENU_DISTANCE", "2", NULL});
    CHECK(pc_vr_panel_depth(PC_VR_PANEL_MENU) == 0 && pc_vr_panel_depth(PC_VR_PANEL_HUD) == 0);
    CHECK(pc_vr_panel_depth_adjust(PC_VR_PANEL_MENU, 1.25) == 0);
    panels[PC_VR_PANEL_MENU].read = panels[PC_VR_PANEL_HUD].read = false;
}

// The process's one read of the environment (its own process): the values set before it, constant after it.
static int environment_test(void) {
    set_env("SAVAGEWIND_PC_VR", "1");
    set_env("SAVAGEWIND_PC_VR_HEAD_LOOK", "1");
    set_env("SAVAGEWIND_PC_VR_EYE_IMAGES", "2");
    set_env("SAVAGEWIND_PC_VR_NATIVE_VIEW_UNITS_PER_METER", "20");
    set_env("SAVAGEWIND_PC_VR_EYE_SIZE", "2064x2208");
    set_env("SAVAGEWIND_PC_VR_UI_PARALLAX", "1");
    set_env("SAVAGEWIND_LOG_PANES", "1");
    const PcVrConfig *config = pc_vr_config();  // (logs its "[pc-vr] configuration:" line)
    CHECK(config->enabled && pc_vr_enabled() && config->play.ui_parallax);
    CHECK(config->play.native_view_units_per_meter == 20 && config->diagnostics.log_panes);
    CHECK(config->aliases_seen == (1u << PC_VR_ALIAS_HEAD_LOOK | 1u << PC_VR_ALIAS_EYE_IMAGES));
    CHECK(config->aliases_ignored == 1u << PC_VR_ALIAS_EYE_IMAGES);
    unsigned width, height;
    CHECK(pc_vr_eye_size(&width, &height) && width == 2064 && height == 2208);
    set_env("SAVAGEWIND_PC_VR", "0");
    set_env("SAVAGEWIND_PC_VR_EYE_SIZE", "1920x2160");
    set_env("SAVAGEWIND_PC_VR_UI_PARALLAX", "0");
    set_env("SAVAGEWIND_PC_VR_NATIVE_VIEW_UNITS_PER_METER", "30");
    CHECK(pc_vr_config() == config && pc_vr_enabled() && config->play.ui_parallax);
    CHECK(config->play.native_view_units_per_meter == 20);
    CHECK(pc_vr_eye_size(&width, &height) && width == 2064 && height == 2208);
    printf("PC VR configuration from the environment: %u checks passed (read once, constant after)\n", checks);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "environment")) return environment_test();
    if (argc != 1) return 2;
    test_enabled();
    test_defaults();
    test_aliases();
    test_units();
    test_play_settings();
    test_diagnostics();
    test_describe();
    test_panels_off();
    test_eye_size();
    test_panel_depths();
    printf("PC VR configuration fixture: %u checks passed\n", checks);
    return 0;
}

// hle/labo_guest_memory.h: a new access generation per call keeps its page cache cold here.
uint64_t plat_guest_access_generation(void) { static uint64_t generation; return ++generation; }
