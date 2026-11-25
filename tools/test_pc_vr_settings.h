// For the fixtures: PC VR's configuration (runtime/pc_vr_config.c, compiled in here) from the fixture's own settings
// instead of the environment. Settings are name and value pairs ending with NULL; a NULL value is an unset name.
#pragma once
#include <pthread.h>
#include <string.h>
#include "../runtime/pc_vr_config.c"

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
