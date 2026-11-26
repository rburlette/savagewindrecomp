// Part of input.c (D133): test tooling's input state: key diagnostics and the host input script's.
// Included where it stands by input.c, so it compiles in input.c's translation unit as before.

// Opt-in, bounded metadata for diagnosing short host key presses. No text input or guest data is logged.
// The window loop can process both SDL edges before the guest's next GetNpadState call.
static _Atomic int input_diagnostics = -1;
static _Atomic uint64_t diagnostic_frame, diagnostic_player_samples;
static _Atomic unsigned diagnostic_events, diagnostic_samples;
static _Atomic int diagnostic_held_scancode;
static _Atomic bool host_input_finished, host_input_controlled;
static _Atomic bool pad_handoff_pending;
static atomic_flag keyboard_state_lock = ATOMIC_FLAG_INIT;

static void keyboard_lock(void) {
    if (getenv("SAVAGEWIND_HOST_INPUT_UNTIL"))
        while (atomic_flag_test_and_set_explicit(&keyboard_state_lock, memory_order_acquire)) SDL_Delay(0);
}

static void keyboard_unlock(void) {
    if (getenv("SAVAGEWIND_HOST_INPUT_UNTIL"))
        atomic_flag_clear_explicit(&keyboard_state_lock, memory_order_release);
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
