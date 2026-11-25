// Part of am.c (D133): PC VR's Labo motion profile, read by the sample adapter.
// Included where it stands by am.c, so it compiles in am.c's translation unit as before.

// Separate from the message queue: the sample adapter reads this from other threads.
static pthread_mutex_t labo_motion_lock = PTHREAD_MUTEX_INITIALIZER;
static bool labo_motion_profile_known;
static bool labo_motion_profile_enabled;
static bool labo_motion_vr_active;
static unsigned labo_motion_transitions_logged;

static bool labo_motion_profile_locked(void) {
    if (!labo_motion_profile_known) {
        const char *setting = getenv("SAVAGEWIND_LABO_MOTION");
        // PC VR remains docked/boost even if a standalone diagnostic variable was
        // inherited from the shell.
        labo_motion_profile_enabled = !pc_vr_enabled() && setting && strcmp(setting, "1") == 0;
        labo_motion_profile_known = true;
        if (labo_motion_profile_enabled)
            LOG("[am] Labo motion diagnostic profile enabled (handheld, normal performance, VR lifecycle)\n");
    }
    return labo_motion_profile_enabled;
}

static bool labo_motion_service_locked(void) {
    return pc_vr_enabled() || labo_motion_profile_locked();
}

bool labo_motion_enabled(void) {
    pthread_mutex_lock(&labo_motion_lock);
    bool enabled = labo_motion_profile_locked();
    pthread_mutex_unlock(&labo_motion_lock);
    return enabled;
}

bool labo_motion_active(void) {
    pthread_mutex_lock(&labo_motion_lock);
    bool active = labo_motion_service_locked() && labo_motion_vr_active;
    pthread_mutex_unlock(&labo_motion_lock);
    return active;
}

bool labo_motion_sample_acquire(void) {
    pthread_mutex_lock(&labo_motion_lock);
    if (labo_motion_service_locked() && labo_motion_vr_active) return true;
    pthread_mutex_unlock(&labo_motion_lock);
    return false;
}

void labo_motion_sample_release(void) {
    pthread_mutex_unlock(&labo_motion_lock);
}

static void labo_motion_set_active(bool active) {
    pthread_mutex_lock(&labo_motion_lock);
    if (labo_motion_service_locked() && labo_motion_vr_active != active) {
        labo_motion_vr_active = active;
        if (labo_motion_transitions_logged < 16)
            LOG("[am] Labo VR lifecycle %s\n", active ? "begin" : "end");
        else if (labo_motion_transitions_logged == 16)
            LOG("[am] Labo VR lifecycle transition log limit reached\n");
        if (labo_motion_transitions_logged <= 16) ++labo_motion_transitions_logged;
    }
    pthread_mutex_unlock(&labo_motion_lock);
}
