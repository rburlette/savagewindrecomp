// Part of input.c (D133): test tooling's input state: the minimum key hold, key diagnostics, the automation's state.
// Included where it stands by input.c, so it compiles in input.c's translation unit as before.

// Opt-in, bounded metadata for diagnosing short host key presses. No text input or guest data is logged.
// The window loop can process both SDL edges before the guest's next GetNpadState call.
static _Atomic int input_diagnostics = -1;
static _Atomic uint64_t diagnostic_frame, diagnostic_player_samples;
static _Atomic unsigned diagnostic_events, diagnostic_samples;
static _Atomic int diagnostic_held_scancode;
static _Atomic int minimum_hold_ms = -1;
static _Atomic bool host_input_finished, host_input_controlled;
static _Atomic bool pad_handoff_pending;
static _Atomic int quicksave_request;  // 1 start, 2 cancel; UI thread -> render thread
static _Atomic bool quicksave_active;
static _Atomic uint64_t quicksave_deadline_ns;
static atomic_flag keyboard_state_lock = ATOMIC_FLAG_INIT;
static InputMinHoldKey minimum_hold_keys[SDL_SCANCODE_COUNT];
static void recompute_keyboard(void);

static int configured_minimum_hold_ms(void) {
    int ms = atomic_load(&minimum_hold_ms);
    if (ms >= 0) return ms;
    ms = input_min_hold_parse_ms(getenv("SAVAGEWIND_INPUT_MIN_HOLD_MS"));
    atomic_store(&minimum_hold_ms, ms);
    return ms;
}

static bool min_hold_reserved(SDL_Scancode sc) {
    return (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) || sc == SDL_SCANCODE_ESCAPE ||
           sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER;
}

static void keyboard_lock(void) {
    if (configured_minimum_hold_ms() || getenv("SAVAGEWIND_HOST_INPUT_UNTIL") ||
        getenv("SAVAGEWIND_QUICKSAVE"))
        while (atomic_flag_test_and_set_explicit(&keyboard_state_lock, memory_order_acquire)) SDL_Delay(0);
}

static void keyboard_unlock(void) {
    if (configured_minimum_hold_ms() || getenv("SAVAGEWIND_HOST_INPUT_UNTIL") ||
        getenv("SAVAGEWIND_QUICKSAVE"))
        atomic_flag_clear_explicit(&keyboard_state_lock, memory_order_release);
}

static bool min_hold_expire_locked(uint64_t now_ns) {
    bool changed = false;
    for (int scancode = 1; scancode < SDL_SCANCODE_COUNT; scancode++) {
        if (input_min_hold_expire(&minimum_hold_keys[scancode], now_ns)) {
            keys_down[scancode] = 0;
            if (atomic_load(&diagnostic_held_scancode) == scancode) atomic_store(&diagnostic_held_scancode, 0);
            changed = true;
        }
    }
    return changed;
}

static void min_hold_expire(void) {
    if (!configured_minimum_hold_ms()) return;
    keyboard_lock();
    if (min_hold_expire_locked(SDL_GetTicksNS())) recompute_keyboard();
    keyboard_unlock();
}

static bool diagnostics_enabled(void) {
    int on = atomic_load(&input_diagnostics);
    if (on < 0) {
        const char *setting = getenv("SAVAGEWIND_INPUT_DIAGNOSTICS");
        on = setting && !strcmp(setting, "1");
        atomic_store(&input_diagnostics, on);
    }
    return on != 0;
}

static uint64_t key_binding_slots(SDL_Scancode scancode) {
    uint64_t slots = 0;
    if (scancode == SDL_SCANCODE_UNKNOWN) return 0;
    for (int i = 0; i < BIND_KEY_COUNT; i++)
        if (g_bind_key[i] == scancode || g_bind_key2[i] == scancode) slots |= 1ull << i;
    return slots;
}
