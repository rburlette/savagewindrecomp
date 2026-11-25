// For the fixtures: PC VR's configuration (runtime/pc_vr_config.c, compiled in here) from the fixture's own settings
// instead of savagewind.ini and the environment. Settings are name and value pairs ending with NULL, savagewind.ini's
// keys ("vr", "vr_eye_scale") and launch options ("SAVAGEWIND_PC_VR_SHOT") alike; a NULL value is an unset name.
#pragma once
#include <pthread.h>
#include <string.h>
#include "../runtime/pc_vr_config.c"
#include "../runtime/pc_vr_settings.inc"  // (savagewind.ini's lines, a part of settings.c)

// (settings.c: the process's read loads the settings first; a fixture hands PC VR its lines with pc_vr_settings_take)
void settings_load(void) {}

static inline const char *test_pc_vr_setting(const void *context, const char *name) {
    for (const char *const *pair = context; pair && *pair; pair += 2)
        if (!strcmp(pair[0], name)) return pair[1];
    return NULL;
}

// A configuration of the fixture's own.
static inline void test_pc_vr_parse(const char *const *settings, PcVrConfig *out) {
    pc_vr_config_parse(test_pc_vr_setting, settings, out);
}

static inline void test_pc_vr_no_environment(void) {}

// The process's configuration (pc_vr_config()), from these settings: the fixture takes the process's one read, so the
// environment is never read, and replaces what pc_vr_config() answers from then on.
static inline void test_pc_vr_configure(const char *const *settings) {
    pthread_once(&process_configuration_once, test_pc_vr_no_environment);
    test_pc_vr_parse(settings, &process_configuration);
}
