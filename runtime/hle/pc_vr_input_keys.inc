// Part of input.c (D133): PC VR's live keys: world scale, panel distances, the grade, the DLAA modes.
// Included where it stands by input.c, so it compiles in input.c's translation unit as before.

// PC VR world scale, live (native eye views only): '-' smaller, a toy in front of you; '=' or '+' bigger; '0' back to
// the launch value. 25% per press. Host keys: never guest buttons while the native views run.
static bool world_scale_event(const SDL_Event *e) {
    if ((e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_KEY_UP) || !pc_vr_world_scale_active()) return false;
    const SDL_Scancode sc = e->key.scancode;
    double factor;
    if (sc == SDL_SCANCODE_MINUS || sc == SDL_SCANCODE_KP_MINUS) factor = 1.25;
    else if (sc == SDL_SCANCODE_EQUALS || sc == SDL_SCANCODE_KP_PLUS) factor = 0.8;
    else if (sc == SDL_SCANCODE_0 || sc == SDL_SCANCODE_KP_0) factor = 0;
    else return false;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) pc_vr_world_scale(factor);
    return true;
}

// PC VR quad depths, live, 25% per press: menus (D056) '[' nearer, ']' farther, '\' back to the launch value; the
// gameplay HUD relative to the camera target near Link (D057) ',' nearer, '.' farther, '/' back.
static bool panel_distance_event(const SDL_Event *e) {
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_KEY_UP) return false;
    const SDL_Scancode sc = e->key.scancode;
    const PcVrPanel which = sc == SDL_SCANCODE_LEFTBRACKET || sc == SDL_SCANCODE_RIGHTBRACKET ||
        sc == SDL_SCANCODE_BACKSLASH ? PC_VR_PANEL_MENU : PC_VR_PANEL_HUD;
    double factor;
    if (sc == SDL_SCANCODE_LEFTBRACKET || sc == SDL_SCANCODE_COMMA) factor = 0.8;
    else if (sc == SDL_SCANCODE_RIGHTBRACKET || sc == SDL_SCANCODE_PERIOD) factor = 1.25;
    else if (sc == SDL_SCANCODE_BACKSLASH || sc == SDL_SCANCODE_SLASH) factor = 0;
    else return false;
    if (!pc_vr_panel_depth(which)) return false;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) pc_vr_panel_depth_adjust(which, factor);
    return true;
}

// PC VR colour grading, live, to compare in the headset (the settings stay as they are): ';' turns the grade (LUT,
// contrast, saturation, white balance, exposure) off and on again; ''' steps the sliders through the presets (Natural,
// Warm, Cool, Vivid, Cinematic, Muted) and back to the settings' own, the LUT as set.
static bool grade_toggle_event(const SDL_Event *e) {
    if ((e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_KEY_UP) || !pc_vr_enabled()) return false;
    const SDL_Scancode sc = e->key.scancode;
    if (sc != SDL_SCANCODE_SEMICOLON && sc != SDL_SCANCODE_APOSTROPHE) return false;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        extern void color_grade_toggle_live(void), color_grade_cycle_live(void);
        if (sc == SDL_SCANCODE_SEMICOLON) color_grade_toggle_live();
        else color_grade_cycle_live();
    }
    return true;
}

// PC VR's DLAA modes, live, to compare in the headset unless vr_antialiasing = off: '9' steps none (the game's FXAA),
// DLAA, DLSS (runtime/nvn/vulkan/vr_dlaa.h). A host key while PC VR runs, never a guest button.
static bool dlaa_mode_event(const SDL_Event *e) {
    if ((e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_KEY_UP) || !pc_vr_enabled() ||
        e->key.scancode != SDL_SCANCODE_9)
        return false;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        extern void vr_dlaa_next_mode(void);
        vr_dlaa_next_mode();
    }
    return true;
}
