// Part of input.c (D133): the quicksave macro (SAVAGEWIND_QUICKSAVE).
// Included where it stands by input.c, so it compiles in input.c's translation unit as before.

static bool quicksave_push_key(SDL_Scancode sc, bool down) {
    SDL_Event e = {0};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = QUICKSAVE_DEVICE;
    e.key.scancode = sc;
    e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    e.key.down = down;
    return SDL_PushEvent(&e);
}

static bool quicksave_keep_non_macro_event(void *unused, SDL_Event *e) {
    (void)unused;
    return !quicksave_event(e);
}

static bool quicksave_resolve_keys(SDL_Scancode keys[QUICKSAVE_B + 1]) {
    static const int slots[] = {
        [QUICKSAVE_PLUS] = BIND_PLUS, [QUICKSAVE_R] = BIND_R, [QUICKSAVE_A] = BIND_A,
        [QUICKSAVE_UP] = BIND_LS_UP, [QUICKSAVE_B] = BIND_B
    };
    for (int action = QUICKSAVE_PLUS; action <= QUICKSAVE_B; action++) {
        int slot = slots[action];
        int candidates[] = {g_bind_key[slot], g_bind_key2[slot]};
        keys[action] = SDL_SCANCODE_UNKNOWN;
        for (unsigned i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
            int scancode = candidates[i];
            if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT || scancode == SDL_SCANCODE_F5) continue;
            if (key_binding_slots((SDL_Scancode)scancode) == (UINT64_C(1) << slot)) {
                keys[action] = (SDL_Scancode)scancode;
                break;
            }
        }
        if (keys[action] == SDL_SCANCODE_UNKNOWN) return false;
    }
    return true;
}

static QuicksaveView quicksave_view(AutomationState *state) {
    *state = (AutomationState){0};
    bool fresh = automation_state_read(state);
    QuicksaveView view = {
        .screens = state->screen_mask, .pause = state->pause_state, .fresh = fresh,
        .world = (state->flags & AUTOMATION_STATE_WORLD_LOADED) != 0,
        .title = (state->flags & AUTOMATION_STATE_TITLE) != 0,
        .event = (state->flags & AUTOMATION_STATE_EVENT) != 0,
        .movie = (state->flags & AUTOMATION_STATE_MOVIE) != 0,
        .high_screens = (state->flags & AUTOMATION_STATE_HIGH_SCREENS) != 0
    };
    return view;
}

static void quicksave_stop(const char *reason) {
    atomic_store(&quicksave_active, false);
    SDL_FilterEvents(quicksave_keep_non_macro_event, NULL);
    keyboard_lock();
    memset(quicksave_keys, 0, sizeof quicksave_keys);
    recompute_keyboard();
    keyboard_unlock();
    atomic_store(&quicksave_deadline_ns, 0);
    LOG("[quicksave] %s\n", reason);
}

static void quicksave_frame(void) {
    static QuicksaveControl control;
    static SDL_Scancode held;
    static uint64_t release_frame;
    static uint64_t test_frame;
    static bool test_checked, test_fired, test_first_queued, test_first_succeeded;
    static bool test_repeat, test_second_fired;
    if (!quicksave_enabled()) return;
    extern uint64_t renderer_present_count(void);
    uint64_t frame = renderer_present_count();
    uint64_t now = SDL_GetTicksNS();
    if (!test_checked) {
        test_checked = true;
        const char *spec = getenv("SAVAGEWIND_QUICKSAVE_TEST_FRAME");
        const char *repeat = getenv("SAVAGEWIND_QUICKSAVE_TEST_REPEAT");
        test_repeat = repeat && !strcmp(repeat, "1");
        if (spec) {
            uint64_t until = 0, shot = 0;
            if (!host_input_parse_until(spec, &test_frame) ||
                !host_input_parse_until(getenv("SAVAGEWIND_HOST_INPUT_UNTIL"), &until) ||
                !host_input_parse_until(getenv("SAVAGEWIND_WINDOW_SHOT"), &shot) ||
                !host_input_control_compatible(getenv("SAVAGEWIND_HOST_INPUT"),
                                               getenv("SAVAGEWIND_INPUT"),
                                               getenv("SAVAGEWIND_BIND_TEST"),
                                               getenv("SAVAGEWIND_SCRIPT_CLOCK")) ||
                !getenv("SAVAGEWIND_EXIT_AFTER_SHOT") ||
                strcmp(getenv("SAVAGEWIND_EXIT_AFTER_SHOT"), "1") ||
                test_frame <= until || shot < test_frame + (test_repeat ? 1200 : 420) ||
                (repeat && !test_repeat)) {
                LOG("[quicksave] invalid bounded test trigger; disabled\n");
                test_frame = 0;
            }
        }
    }
    if (test_frame && !test_fired && frame >= test_frame) {
        test_fired = true;
        int no_request = 0;
        if (atomic_load(&host_input_finished) && !atomic_load(&quicksave_active) &&
            atomic_compare_exchange_strong(&quicksave_request, &no_request, 1)) {
            // Same one-shot request consumed by physical F5; the bounded run supplies no OS keypress.
            LOG("[quicksave] bounded test trigger at present %llu\n", (unsigned long long)frame);
            test_first_queued = true;
        } else LOG("[quicksave] test trigger refused: startup input or player request active\n");
    }
    if (test_repeat && test_first_queued && !test_second_fired && frame >= test_frame + 600) {
        test_second_fired = true;
        int no_request = 0;
        if (test_first_succeeded && control.stage == QUICKSAVE_FINISHED &&
            atomic_load(&host_input_finished) && !atomic_load(&quicksave_active) &&
            atomic_compare_exchange_strong(&quicksave_request, &no_request, 1)) {
            LOG("[quicksave] bounded repeat trigger at present %llu\n", (unsigned long long)frame);
        } else LOG("[quicksave] bounded repeat refused: first run incomplete or input active\n");
    }
    int request = atomic_exchange(&quicksave_request, 0);
    if (request == 2) {
        if (control.stage != QUICKSAVE_IDLE && control.stage != QUICKSAVE_FINISHED) quicksave_stop("cancelled by player");
        control.stage = QUICKSAVE_FINISHED;
        held = SDL_SCANCODE_UNKNOWN;
        return;
    }
    if (request != 1 && !atomic_load(&quicksave_active)) return;
    SDL_Scancode keys[QUICKSAVE_B + 1];
    bool bindings_valid = quicksave_resolve_keys(keys);
    if (request == 1) {
        if (!atomic_load(&win_focused) || menu_input_captured() ||
            getenv("SAVAGEWIND_INPUT") || getenv("SAVAGEWIND_BIND_TEST") ||
            (getenv("SAVAGEWIND_HOST_INPUT") && !atomic_load(&host_input_finished)) ||
            !bindings_valid) {
            LOG("[quicksave] unavailable (focus, overlay, script, or bindings)\n");
            return;
        }
        control = (QuicksaveControl){0};
        atomic_store(&quicksave_active, true);
        atomic_store(&quicksave_deadline_ns, now + 15000000000ull);
    }
    if (!atomic_load(&quicksave_active)) return;
    if (held != SDL_SCANCODE_UNKNOWN && frame >= release_frame) {
        if (!quicksave_push_key(held, false)) {
            quicksave_stop("key release queue failed");
            control.stage = QUICKSAVE_FINISHED;
            held = SDL_SCANCODE_UNKNOWN;
            return;
        }
        held = SDL_SCANCODE_UNKNOWN;
    }
    if (menu_input_captured() || !atomic_load(&win_focused) || !bindings_valid) {
        quicksave_stop("overlay, focus, or bindings changed");
        control.stage = QUICKSAVE_FINISHED;
        held = SDL_SCANCODE_UNKNOWN;
        return;
    }
    AutomationState snapshot = {0};
    QuicksaveAction action = quicksave_advance(&control, quicksave_view(&snapshot), frame, now);
    if (action == QUICKSAVE_ABORT || action == QUICKSAVE_DONE) {
        if (test_first_queued && !test_second_fired && action == QUICKSAVE_DONE) test_first_succeeded = true;
        quicksave_stop(action == QUICKSAVE_DONE ? "returned to gameplay" : "state changed or timed out; UI left for player");
        held = SDL_SCANCODE_UNKNOWN;
        return;
    }
    if (action == QUICKSAVE_NONE) return;
    if (!atomic_load(&quicksave_active) || atomic_load(&quicksave_request) == 2) return;
    held = keys[action];
    if (!quicksave_push_key(held, true)) {
        quicksave_stop("key press queue failed");
        control.stage = QUICKSAVE_FINISHED;
        held = SDL_SCANCODE_UNKNOWN;
        return;
    }
    release_frame = frame + 4;  // observed menu repeat begins after about six presented frames
    if (test_frame)
        LOG("[quicksave] step %d at present %llu pause=%u mask=%#llx flags=%#x screens=%u\n",
            (int)action, (unsigned long long)frame, snapshot.pause_state,
            (unsigned long long)snapshot.screen_mask, snapshot.flags, snapshot.screen_count);
    else LOG("[quicksave] step %d at present %llu\n", (int)action, (unsigned long long)frame);
}
