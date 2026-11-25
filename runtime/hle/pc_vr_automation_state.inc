// Part of framepace.c (D133): the automation's observations of the game (automation_state.h).
// Included where it stands by framepace.c, so it compiles in framepace.c's translation unit as before.

// Opt-in observations only. Atomic payload fields avoid the C data race of a seqlock over plain data.
// The writer flag also makes an unexpected concurrent VFR call skip rather than mix two samples.
static struct {
    atomic_flag writer;
    atomic_uint_fast64_t sequence, sampled_ns, screen_mask;
    atomic_uint screen_count, pause_state, flags;
    atomic_bool valid;
} automation_pub = {.writer = ATOMIC_FLAG_INIT};

static bool automation_enabled(void) {
    static atomic_int mode;  // 0 uninitialized, 1 off, 2 on; process-latched like the other runtime switches
    int value = atomic_load(&mode);
    if (!value) {
        const char *setting = getenv("SAVAGEWIND_QUICKSAVE");
        int expected = 0, chosen = setting && !strcmp(setting, "1") ? 2 : 1;
        atomic_compare_exchange_strong(&mode, &expected, chosen);
        value = atomic_load(&mode);
    }
    return value == 2;
}

static bool automation_guest_range(uint64_t address, uint64_t bytes, uint64_t alignment) {
    return bytes && address >= GUEST_LO && address < GUEST_HI && !(address & (alignment - 1)) &&
           bytes <= GUEST_HI - address;
}

static bool automation_guest_read(uint64_t address, void *out, size_t bytes, uint64_t alignment) {
    if (!automation_guest_range(address, bytes, alignment)) return false;
#ifdef _WIN32
    // A reserved guest address is not proof of readable memory. Do not invoke the lazy-commit fault handler.
    uint64_t end = address + bytes;
    for (uint64_t at = address; at < end;) {
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery((void *)(uintptr_t)at, &mi, sizeof mi) != sizeof mi || mi.State != MEM_COMMIT ||
            (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        DWORD protection = mi.Protect & 0xff;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY)
            return false;
        uint64_t base = (uint64_t)(uintptr_t)mi.BaseAddress, size = (uint64_t)mi.RegionSize;
        if (base > at || size <= at - base || size > UINT64_MAX - base) return false;
        at = base + size;
    }
    // Fail closed if permissions change between the check and copy, instead of faulting into guest recovery.
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)address, out, bytes, &copied) && copied == bytes;
#else
    (void)out;
    return false;
#endif
}

static bool automation_main_read(uint64_t address, void *out, size_t bytes, uint64_t alignment) {
    const Module *main_module = loader_module("main");
    if (!main_module || main_module->base != MAIN_BASE || main_module->size > MODULES_END - MAIN_BASE ||
        address < main_module->base || address - main_module->base > main_module->size ||
        bytes > main_module->size - (address - main_module->base))
        return false;
    return automation_guest_read(address, out, bytes, alignment);
}

static bool automation_member(uint64_t object, uint64_t offset, void *out, size_t bytes, uint64_t alignment) {
    if (!automation_guest_range(object, 8, 8) || offset > GUEST_HI - object) return false;
    return automation_guest_read(object + offset, out, bytes, alignment);
}

static void automation_publish(const AutomationState *state,
                               bool valid) {  // writer flag held, or guest threads stopped
    atomic_fetch_add(&automation_pub.sequence, 1);
    atomic_store(&automation_pub.sampled_ns, state->sampled_ns);
    atomic_store(&automation_pub.screen_mask, state->screen_mask);
    atomic_store(&automation_pub.screen_count, state->screen_count);
    atomic_store(&automation_pub.pause_state, state->pause_state);
    atomic_store(&automation_pub.flags, state->flags);
    atomic_store(&automation_pub.valid, valid);
    atomic_fetch_add(&automation_pub.sequence, 1);
}

static bool automation_collect(AutomationState *state) {
    // Game sites: the UI root slot (PC VR's group), the world and event managers (upstream's).
    uint64_t slot, mgr, screens, wm, evt;
    uint64_t entries[256];
    if (!automation_main_read(PAUSE_STATE, &state->pause_state, sizeof state->pause_state, 4) ||
        !automation_main_read(g_site.automation_screen_root, &slot, sizeof slot, 8) ||
        !automation_guest_read(slot, &mgr, sizeof mgr, 8) ||
        !automation_member(mgr, 0x28, &state->screen_count, sizeof state->screen_count, 4) ||
        !state->screen_count || state->screen_count > 256 ||
        !automation_member(mgr, 0x30, &screens, sizeof screens, 8) ||
        !automation_guest_read(screens, entries, state->screen_count * sizeof entries[0], 8)) return false;
    for (uint32_t i = 0; i < state->screen_count; i++) {
        if (!entries[i]) continue;
        // Screen objects themselves are not dereferenced, but reject malformed non-null identities.
        if (!automation_guest_range(entries[i], 8, 8)) return false;
        if (i < 64) state->screen_mask |= UINT64_C(1) << i;
        else state->flags |= AUTOMATION_STATE_HIGH_SCREENS;
    }
    if (state->pause_state) state->flags |= AUTOMATION_STATE_PAUSED;
    if (state->screen_mask & (UINT64_C(1) << 1)) state->flags |= AUTOMATION_STATE_TITLE;
    if (!automation_main_read(g_site.world_mgr, &wm, sizeof wm, 8) ||
        !automation_main_read(g_site.event_mgr, &evt, sizeof evt, 8)) return false;
    if (wm) {
        uint32_t count;
        if (!automation_member(wm, 0x5c0, &count, sizeof count, 4)) return false;
        if (count) state->flags |= AUTOMATION_STATE_WORLD_LOADED;
    }
    if (evt) {
        uint64_t active;
        if (!automation_member(evt, 0x1d2b8, &active, sizeof active, 8) ||
            (active && !automation_guest_range(active, 8, 8))) return false;
        if (active) state->flags |= AUTOMATION_STATE_EVENT;
    }
    int movie = __atomic_load_n(&g_movie_playing, __ATOMIC_RELAXED);
    if (movie < 0) return false;
    if (movie) state->flags |= AUTOMATION_STATE_MOVIE;
    // Reject a UI transition that changed the container, its entries or pause state while sampled. This is
    // still an observation at the VFR hook, not a transaction locking every guest UI/event writer.
    uint64_t slot2, mgr2, screens2;
    uint64_t entries2[256];
    uint32_t count2, pause2;
    return automation_main_read(PAUSE_STATE, &pause2, sizeof pause2, 4) && pause2 == state->pause_state &&
           automation_main_read(g_site.automation_screen_root, &slot2, sizeof slot2, 8) && slot2 == slot &&
           automation_guest_read(slot2, &mgr2, sizeof mgr2, 8) && mgr2 == mgr &&
           automation_member(mgr2, 0x28, &count2, sizeof count2, 4) && count2 == state->screen_count &&
           automation_member(mgr2, 0x30, &screens2, sizeof screens2, 8) && screens2 == screens &&
           automation_guest_read(screens2, entries2, count2 * sizeof entries2[0], 8) &&
           !memcmp(entries, entries2, count2 * sizeof entries2[0]);
}

static void automation_sample(void) {
    if (!automation_enabled() || atomic_flag_test_and_set(&automation_pub.writer)) return;
    AutomationState state = {.sampled_ns = plat_now_ns()};
    bool valid = !rt_reboot_requested() && automation_collect(&state);
    if (!valid) memset(&state, 0, sizeof state);
    automation_publish(&state, valid);
    atomic_flag_clear(&automation_pub.writer);
}

bool automation_state_read(AutomationState *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!automation_enabled() || rt_reboot_requested()) return false;
    for (int attempt = 0; attempt < 4; attempt++) {
        uint64_t before = atomic_load(&automation_pub.sequence);
        if (before & 1) continue;
        AutomationState state = {
            .sampled_ns = atomic_load(&automation_pub.sampled_ns),
            .screen_mask = atomic_load(&automation_pub.screen_mask),
            .screen_count = atomic_load(&automation_pub.screen_count),
            .pause_state = atomic_load(&automation_pub.pause_state),
            .flags = atomic_load(&automation_pub.flags)
        };
        bool valid = atomic_load(&automation_pub.valid);
        if (before != atomic_load(&automation_pub.sequence)) continue;
        uint64_t now = plat_now_ns();
        if (!valid || !state.sampled_ns || now < state.sampled_ns || now - state.sampled_ns >= UINT64_C(500000000))
            return false;
        *out = state;
        return true;
    }
    return false;
}

RT_ON_RESET(automation_state_reset) {
    // Reset hooks run after all guest threads (the only sampling writers) have stopped.
    AutomationState state = {0};
    automation_publish(&state, false);
    atomic_flag_clear(&automation_pub.writer);
}
