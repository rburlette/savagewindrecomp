// Asset-free check of where a 2D draw into the final image goes (runtime/nvn/vulkan/vr_draw_route.h, D057, D072, D073,
// D088, D097): the route, the held-scene decision, and exec.c's hub around them. Named cases a reader can follow: a
// hearts draw in play goes to the HUD's image; an enemy marker into its eye's half; the same marker over a held scene
// is left out; a reticle there is left out without its screen being read; a world screen's piece without a link is
// left out and counted for the guard; the fade over each eye's view; a shop's backdrop by each fate; a draw that is
// not a HUD draw; the quick menu, the shop, the pause menu, no held scene and a scene after all for the held-scene
// decision. Then the code as it was at 5480e1e (exec.c: vr_quick_menu_over_world, vr_is_hud_draw, vr_hud_backdrop_fate
// and draw_'s routing), transcribed below with the executor's state as this file's, against the hub as exec.c has it
// now, on every combination of the discrete inputs: the gate's (eye images on, the HUD's image usable, one colour
// target, the final image), the frame's (placement, scene, certified eyes, the latch, the held scene), the settings
// (the HUD panel, the backdrop switch), the game's (quick menu, shop), the draw's tags (screen kind, a tag past the
// screen table included, marker value, pane role, pane number), the placements' answers and whether the draw samples
// a picture of the screen; then the backdrop's inputs in full (eight sets of sampled textures: none, one, two or
// three, a match on any of them by either flag, a texture not bound) on HUD draws in play and over the held scene. Compared:
// whether the draw goes on, vr_draw_to_hud_image, the move's active flag (left untouched or set), the backdrop flags, the
// counters, the latch, and the exact sequence of calls with side effects (the HUD setting, the pause state, the
// shop's screen, the first reading of the screen's kind, the placements, the texture test, the pane log with its
// kinds and names). No renderer, GPU, game or headset is involved.
#include "../runtime/nvn/vulkan/vr_draw_route.h"
#include "test_pc_vr_settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void rt_fatal_trace(void) { abort(); }

static unsigned checks, failures;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; if (failures < 10) printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

// ---------------------------------------------------------------------------- the calls with side effects, recorded
enum {
    EVENT_PANEL_DEPTH = 1,  // the HUD setting (pc_vr_panel_depth: takes a lock)
    EVENT_QUICK_UI,         // the game's pause state (framepace_quick_ui)
    EVENT_SHOP_OPEN,        // the shop's screen (pc_vr_shop_open: walks the screen list)
    EVENT_SCREEN_KIND,      // the draw's screen's kind, first reading in the draw (pc_vr_screen_kind: logged once a screen)
    EVENT_FADE,             // the fade's placement tried (vr_hud_fade)
    EVENT_MARKER,           // the marker's placement tried (vr_world_hud_draw)
    EVENT_TEXTURE,          // a sampled texture tested (vr_backdrop_source)
    EVENT_PANE_LOG,         // the pane log (pc_vr_pane_log_kind), + 16 x its kind
};
typedef struct { unsigned code; const char *name; } Event;
#define EVENTS_MAX 12  // (a draw makes at most 10 such calls)
static Event events[EVENTS_MAX];
static unsigned event_count, event_count_most;
static void record(unsigned code, const char *name) {
    if (event_count < EVENTS_MAX) events[event_count] = (Event){code, name};
    if (++event_count > event_count_most) event_count_most = event_count;
}

// ---------------------------------------------------------------------------------- the executor, as the code reads it
typedef struct { uint32_t flags; } NvnTexture;
typedef struct { unsigned slot; } ShaderTexture;
typedef struct { int ntex; ShaderTexture tex[3]; } ShaderInfo;
typedef struct { ShaderInfo *fi; } Prog;
static const NvnTexture ordinary_art = {2}, other_art = {4}, render_target = {8}, display_image = {1}, both = {9};
static const NvnTexture *sampled[3];  // (what each texture the fragment stage samples resolves to; NULL: none bound)
static ShaderInfo fragment_info = {2, {{0}, {1}, {2}}};
static Prog program = {&fragment_info};
static const NvnTexture *shader_texture_object(int stage, const ShaderTexture *texture) {
    (void)stage;
    return sampled[texture->slot];
}

// The textures a draw samples, by number: 0, two ordinary ones (no picture of the screen); 1, an ordinary one and a
// render target; 2, none; 3, a render target alone; 4, an ordinary one and a display image (flag bit 0); 5, none bound
// and a render target; 6, none bound alone; 7, two ordinary ones and a texture with both flags.
#define TEXTURE_SETS 8
static void sample_textures(unsigned set) {
    static const struct { int count; const NvnTexture *textures[3]; } sets[TEXTURE_SETS] = {
        {2, {&ordinary_art, &other_art}}, {2, {&ordinary_art, &render_target}}, {0, {NULL}},
        {1, {&render_target}},            {2, {&ordinary_art, &display_image}}, {2, {NULL, &render_target}},
        {1, {NULL}},                      {3, {&ordinary_art, &other_art, &both}},
    };
    fragment_info.ntex = sets[set].count;
    memcpy(sampled, sets[set].textures, sizeof sampled);
}

static struct {
    int state;
    uint64_t scene_held;
    struct { bool scene_seen, placement_seen, over_world, over_world_known; unsigned certified; } frame;
} vr_eyes;
static struct { int state; } vr_hud_image;
static NvnTexture final_image;
static struct { struct { int ncolor; const NvnTexture *color[1]; } rt; } st;
static bool final_image_target;
static bool vr_is_final_image(const NvnTexture *texture) { return texture && final_image_target; }

#define PC_VR_PANEL_HUD 1
static bool panel_on, quick_ui, shop_up;
static double pc_vr_panel_depth(int which) {
    (void)which;
    record(EVENT_PANEL_DEPTH, NULL);
    return panel_on ? 1.75 : 0;
}
bool framepace_quick_ui(void);
bool framepace_quick_ui(void) {
    record(EVENT_QUICK_UI, NULL);
    return quick_ui;
}
bool pc_vr_shop_open(void) {
    record(EVENT_SHOP_OPEN, NULL);
    return shop_up;
}
static PcVrScreenKind screen_kinds[4] = {PC_VR_SCREEN_NONE, PC_VR_SCREEN_FLAT, PC_VR_SCREEN_HUD, PC_VR_SCREEN_WORLD};
static bool screen_kind_read;  // (this draw's first reading: later ones are the cached answer, with no effect)
PcVrScreenKind pc_vr_screen_kind(uint32_t tag) {
    if (!screen_kind_read) record(EVENT_SCREEN_KIND, NULL);
    screen_kind_read = true;
    return tag < 4 ? screen_kinds[tag] : PC_VR_SCREEN_NONE;
}
void pc_vr_pane_log_kind(uint32_t id, unsigned kind, const char *kind_name) {
    (void)id;
    record(EVENT_PANE_LOG + 16 * kind, kind_name);
}

static uint32_t vr_draw_screen, vr_draw_marker, vr_draw_pane;
static PcVrPaneRole vr_draw_role;
static bool fade_answer, marker_answer;
static bool vr_hud_fade(Prog *program_used) {
    (void)program_used;
    record(EVENT_FADE, NULL);
    return fade_answer;
}
static bool vr_world_hud_draw(void) {
    record(EVENT_MARKER, NULL);
    return marker_answer;
}
// The configuration exec.c reads (pc_vr_config()), here one of two parsed from the fixture's settings: with
// SAVAGEWIND_PC_VR_HUD_BACKDROP=1 (backdrops drawn as the game draws them) and without.
static PcVrConfig backdrop_drawn, backdrop_default;
static const PcVrConfig *configuration = &backdrop_default;
static bool vr_draw_to_hud_image, vr_backdrop_pictures_black, vr_backdrop_erase;
static struct { uint8_t active; } vr_world_hud;  // (a byte: 7 before the draw shows whether the draw set it)
static unsigned stat_skipped, vr_frame_world_left_out;
static struct { uint64_t frame_draws; } vr_unlinked_world;

static bool vr_backdrop_source(const NvnTexture *texture) {
    record(EVENT_TEXTURE, NULL);
    return texture && (texture->flags & 9);
}

// ------------------------------------------------------------------------------------ the code at 5480e1e (exec.c)
static bool vr_quick_menu_over_world_before(void) {
    if (vr_eyes.frame.scene_seen || vr_eyes.frame.placement_seen || vr_eyes.frame.certified) return false;
    if (!vr_eyes.frame.over_world_known) {
        extern bool framepace_quick_ui(void);  // (pause state 4, hle/framepace.c)
        vr_eyes.frame.over_world_known = true;
        vr_eyes.frame.over_world = vr_eyes.scene_held && pc_vr_panel_depth(PC_VR_PANEL_HUD) &&
            (framepace_quick_ui() || pc_vr_shop_open());
    }
    return vr_eyes.frame.over_world;
}

static bool vr_is_hud_draw_before(void) {
    if (vr_eyes.state <= 0 || vr_hud_image.state < 0 || st.rt.ncolor != 1 || !vr_is_final_image(st.rt.color[0]))
        return false;
    if (vr_eyes.frame.placement_seen ? !pc_vr_panel_depth(PC_VR_PANEL_HUD) : !vr_quick_menu_over_world_before())
        return false;
    return vr_draw_screen != 0;
}

static int vr_hud_backdrop_fate_before(Prog *program_used) {
    const int keep = configuration->play.hud_backdrop;
    const int fate = vr_draw_role == PC_VR_PANE_BOX ? 1 : vr_draw_role == PC_VR_PANE_WORLD_PICTURE ? 2 :
        vr_draw_role == PC_VR_PANE_EDGE_MASK ? 3 : 0;
    if (keep || !fate) return 0;
    for (int i = 0; i < program_used->fi->ntex; i++)
        if (vr_backdrop_source(shader_texture_object(1, &program_used->fi->tex[i]))) return fate;
    return 0;
}

// draw_'s routing; false where draw_ returned.
static bool route_before(Prog *pr) {
    vr_backdrop_pictures_black = vr_backdrop_erase = false;
    vr_draw_to_hud_image = vr_is_hud_draw_before();
    if (vr_draw_to_hud_image && vr_eyes.frame.over_world && (vr_draw_marker || vr_draw_role == PC_VR_PANE_RETICLE ||
                                                    pc_vr_screen_kind(vr_draw_screen) == PC_VR_SCREEN_WORLD)) {
        vr_draw_to_hud_image = false;
        ++vr_frame_world_left_out;
        if (vr_draw_pane) pc_vr_pane_log_kind(vr_draw_pane, 6, "held-world-left-out");
        stat_skipped++;
        return false;
    }
    if (vr_draw_to_hud_image && !vr_draw_marker && pc_vr_screen_kind(vr_draw_screen) == PC_VR_SCREEN_WORLD) {
        vr_draw_to_hud_image = false;
        ++vr_unlinked_world.frame_draws;
        if (vr_draw_pane) pc_vr_pane_log_kind(vr_draw_pane, 7, "world-unlinked-left-out");
        stat_skipped++;
        return false;
    }
    const bool hud_fade = vr_draw_to_hud_image && !vr_eyes.frame.over_world && vr_hud_fade(pr);
    vr_world_hud.active = hud_fade || (vr_draw_to_hud_image && !vr_eyes.frame.over_world && vr_world_hud_draw());
    if (vr_draw_pane && vr_draw_to_hud_image)
        pc_vr_pane_log_kind(vr_draw_pane, hud_fade ? 1 : vr_world_hud.active ? 2 : 0,
                            hud_fade ? "fade" : vr_world_hud.active ? "crosshair" : "hud");
    if (vr_world_hud.active) {
        vr_draw_to_hud_image = false;
    } else if (vr_draw_to_hud_image) {
        const int backdrop = vr_hud_backdrop_fate_before(pr);
        if (vr_draw_pane && backdrop)
            pc_vr_pane_log_kind(vr_draw_pane, 2 + (unsigned)backdrop, backdrop == 1 ? "backdrop-black" :
                                backdrop == 2 ? "backdrop-left-out" : "backdrop-erase");
        if (backdrop == 2) {
            vr_draw_to_hud_image = false;
            stat_skipped++;
            return false;
        }
        vr_backdrop_pictures_black = backdrop == 1 || backdrop == 3;
        vr_backdrop_erase = backdrop == 3;
    }
    return true;
}

// ------------------------------------------------------------------------------------------ the hub now (exec.c)
static bool vr_quick_menu_over_world(void) {
    const bool scene_unseen =
        !(vr_eyes.frame.scene_seen || vr_eyes.frame.placement_seen || vr_eyes.frame.certified);
    if (!scene_unseen) return false;  // (the latch untouched)
    if (!vr_eyes.frame.over_world_known) {
        extern bool framepace_quick_ui(void);  // (pause state 4, hle/framepace.c)
        vr_eyes.frame.over_world_known = true;
        const bool scene_held = vr_eyes.scene_held != 0;
        const bool hud_panel_on = scene_held && pc_vr_panel_depth(PC_VR_PANEL_HUD);
        const bool quick_menu = hud_panel_on && framepace_quick_ui();
        const bool shop_open = hud_panel_on && !quick_menu && pc_vr_shop_open();
        vr_eyes.frame.over_world =
            vr_frame_over_held_scene(scene_unseen, scene_held, hud_panel_on, quick_menu, shop_open);
    }
    return vr_eyes.frame.over_world;
}

static bool vr_is_hud_draw(void) {
    if (vr_eyes.state <= 0 || vr_hud_image.state < 0 || st.rt.ncolor != 1 || !vr_is_final_image(st.rt.color[0]))
        return false;
    if (vr_eyes.frame.placement_seen ? !pc_vr_panel_depth(PC_VR_PANEL_HUD) : !vr_quick_menu_over_world()) return false;
    return vr_draw_screen != 0;
}

static VrBackdropFate vr_hud_backdrop_fate(Prog *program_used) {
    const VrBackdropFate fate = vr_backdrop_fate_of_role(vr_draw_role);
    if (configuration->play.hud_backdrop || !fate) return VR_BACKDROP_NONE;
    for (int i = 0; i < program_used->fi->ntex; i++)
        if (vr_backdrop_source(shader_texture_object(1, &program_used->fi->tex[i]))) return fate;
    return VR_BACKDROP_NONE;
}

static VrDrawRoute vr_route_hud_draw(Prog *program_used) {
    const bool over_held_scene = vr_eyes.frame.over_world;
    const VrDrawLink link = vr_draw_link(vr_draw_marker);
    const PcVrScreenKind screen_kind = vr_draw_route_reads_screen(over_held_scene, link, vr_draw_role) ?
        pc_vr_screen_kind(vr_draw_screen) : PC_VR_SCREEN_NONE;
    VrDrawRoute route = vr_draw_route_ask(&(VrDrawTags){over_held_scene, link, vr_draw_role, screen_kind});
    if (route == VR_DRAW_LEFT_OUT_HELD_WORLD) {
        vr_draw_to_hud_image = false;
        ++vr_frame_world_left_out;
        if (vr_draw_pane)
            pc_vr_pane_log_kind(vr_draw_pane, vr_draw_route_log_kind(route), vr_draw_route_log_name(route));
        return route;
    }
    if (route == VR_DRAW_LEFT_OUT_UNLINKED_WORLD) {
        vr_draw_to_hud_image = false;
        ++vr_unlinked_world.frame_draws;
        if (vr_draw_pane)
            pc_vr_pane_log_kind(vr_draw_pane, vr_draw_route_log_kind(route), vr_draw_route_log_name(route));
        return route;
    }
    const bool fade_placed = route == VR_DRAW_EYE_HALF && vr_hud_fade(program_used);
    const bool marker_placed = !fade_placed && route == VR_DRAW_EYE_HALF && vr_world_hud_draw();
    route = vr_draw_route_placed(route, fade_placed, marker_placed);
    vr_world_hud.active = vr_draw_route_moved(route);
    if (vr_draw_pane)
        pc_vr_pane_log_kind(vr_draw_pane, vr_draw_route_log_kind(route), vr_draw_route_log_name(route));
    if (vr_world_hud.active) {
        vr_draw_to_hud_image = false;
        return route;
    }
    route = vr_draw_route_backdrop(route, vr_hud_backdrop_fate(program_used));
    if (vr_draw_pane && route != VR_DRAW_HUD)
        pc_vr_pane_log_kind(vr_draw_pane, vr_draw_route_log_kind(route), vr_draw_route_log_name(route));
    if (route == VR_DRAW_LEFT_OUT_BACKDROP) {
        vr_draw_to_hud_image = false;
        return route;
    }
    vr_backdrop_pictures_black = vr_draw_route_over_black(route);
    vr_backdrop_erase = vr_draw_route_erases(route);
    return route;
}

// (for the named cases: the route the hub applied; VR_DRAW_FINAL_IMAGE when it was not asked)
static VrDrawRoute last_route;
static VrDrawRoute routed(VrDrawRoute route) {
    last_route = route;
    return route;
}

// draw_'s routing now; false where draw_ returns.
static bool route_now(Prog *pr) {
    vr_backdrop_pictures_black = vr_backdrop_erase = false;
    vr_draw_to_hud_image = vr_is_hud_draw();
    if (!vr_draw_to_hud_image) {
        vr_world_hud.active = false;
    } else if (vr_draw_route_left_out(routed(vr_route_hud_draw(pr)))) {
        stat_skipped++;
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------------------------------------- helpers
// Everything a draw's routing leaves behind.
typedef struct {
    bool goes_on, vr_draw_to_hud_image, black, erase, over_world, over_world_known;
    uint8_t active;
    unsigned skipped, world_left_out;
    uint64_t unlinked;
    unsigned event_count;
    Event events[EVENTS_MAX];
} Outcome;

static void outcome(bool goes_on, Outcome *result) {
    result->goes_on = goes_on;
    result->vr_draw_to_hud_image = vr_draw_to_hud_image;
    result->black = vr_backdrop_pictures_black;
    result->erase = vr_backdrop_erase;
    result->over_world = vr_eyes.frame.over_world;
    result->over_world_known = vr_eyes.frame.over_world_known;
    result->active = vr_world_hud.active;
    result->skipped = stat_skipped;
    result->world_left_out = vr_frame_world_left_out;
    result->unlinked = vr_unlinked_world.frame_draws;
    result->event_count = event_count < EVENTS_MAX ? event_count : EVENTS_MAX;
    memcpy(result->events, events, result->event_count * sizeof events[0]);
}

static bool same_outcome(const Outcome *first, const Outcome *second) {
    if (first->goes_on != second->goes_on || first->vr_draw_to_hud_image != second->vr_draw_to_hud_image || first->black != second->black ||
        first->erase != second->erase || first->over_world != second->over_world ||
        first->over_world_known != second->over_world_known || first->active != second->active ||
        first->skipped != second->skipped || first->world_left_out != second->world_left_out ||
        first->unlinked != second->unlinked || first->event_count != second->event_count)
        return false;
    for (unsigned i = 0; i < first->event_count; ++i) {
        if (first->events[i].code != second->events[i].code) return false;
        const char *first_name = first->events[i].name, *second_name = second->events[i].name;
        if (!first_name != !second_name || (first_name && strcmp(first_name, second_name))) return false;
    }
    return true;
}

// The state before a draw: everything this file reads, from the inputs; the counters and the record empty.
typedef struct {
    bool eyes_on, hud_image_usable, one_target, final_image, placement_seen, panel_on, scene_seen;
    unsigned certified;
    bool latch_known, latch_value, scene_held, quick_menu, shop_open;
    unsigned screen;  // 0: no screen's draw; 1, 2, 3: a flat, the HUD's, a world screen; 300: a tag past the table
    uint32_t marker;
    PcVrPaneRole role;
    uint32_t pane;
    bool fade_placed, marker_placed, backdrop_switch;
    unsigned texture_set;  // (sample_textures)
} Inputs;

static void set_up(const Inputs *inputs) {
    vr_eyes.state = inputs->eyes_on ? 1 : 0;
    vr_hud_image.state = inputs->hud_image_usable ? 1 : -1;
    st.rt.ncolor = inputs->one_target ? 1 : 2;
    st.rt.color[0] = &final_image;
    final_image_target = inputs->final_image;
    vr_eyes.frame.placement_seen = inputs->placement_seen;
    panel_on = inputs->panel_on;
    vr_eyes.frame.scene_seen = inputs->scene_seen;
    vr_eyes.frame.certified = inputs->certified;
    vr_eyes.frame.over_world_known = inputs->latch_known;
    vr_eyes.frame.over_world = inputs->latch_value;
    vr_eyes.scene_held = inputs->scene_held ? 41 : 0;
    quick_ui = inputs->quick_menu;
    shop_up = inputs->shop_open;
    vr_draw_screen = inputs->screen;
    vr_draw_marker = inputs->marker;
    vr_draw_role = inputs->role;
    vr_draw_pane = inputs->pane;
    fade_answer = inputs->fade_placed;
    marker_answer = inputs->marker_placed;
    configuration = inputs->backdrop_switch ? &backdrop_drawn : &backdrop_default;
    sample_textures(inputs->texture_set);
    vr_world_hud.active = 7;
    last_route = VR_DRAW_FINAL_IMAGE;
    stat_skipped = vr_frame_world_left_out = 0;
    vr_unlinked_world.frame_draws = 0;
    vr_draw_to_hud_image = vr_backdrop_pictures_black = vr_backdrop_erase = true;  // (routing sets them first)
    event_count = 0;
    screen_kind_read = false;
}

// A draw in a scene frame in play: the eye images on, the HUD's image usable, into the final image alone, the scene
// placed, the HUD panel on, a screen's draw.
static Inputs in_play(unsigned screen, uint32_t marker, PcVrPaneRole role) {
    return (Inputs){.eyes_on = true, .hud_image_usable = true, .one_target = true, .final_image = true,
                    .placement_seen = true, .panel_on = true, .scene_seen = true, .certified = 3, .scene_held = true,
                    .screen = screen, .marker = marker, .role = role, .pane = 5};
}

// The same over the held scene: a quick menu's frame without a scene, the latch not yet decided.
static Inputs over_held_scene(unsigned screen, uint32_t marker, PcVrPaneRole role) {
    Inputs inputs = in_play(screen, marker, role);
    inputs.placement_seen = inputs.scene_seen = false;
    inputs.certified = 0;
    inputs.quick_menu = true;
    return inputs;
}

static Outcome run_now(const Inputs *inputs) {
    set_up(inputs);
    Outcome result;
    outcome(route_now(&program), &result);
    return result;
}

static void run_before_into(const Inputs *inputs, Outcome *result) {
    set_up(inputs);
    outcome(route_before(&program), result);
}

static void run_now_into(const Inputs *inputs, Outcome *result) {
    set_up(inputs);
    outcome(route_now(&program), result);
}

// Whether the record holds this call.
static bool called(const Outcome *result, unsigned code) {
    for (unsigned i = 0; i < result->event_count; ++i)
        if (result->events[i].code == code) return true;
    return false;
}

int main(void) {
    test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL}, &backdrop_default);
    test_pc_vr_parse((const char *const[]){"SAVAGEWIND_PC_VR", "1", "SAVAGEWIND_PC_VR_HUD_BACKDROP", "1", NULL},
                     &backdrop_drawn);
    CHECK(!backdrop_default.play.hud_backdrop && backdrop_drawn.play.hud_backdrop);
    // ---- Named cases, through the hub as exec.c has it.
    // The hearts, in play: the flat HUD's screen, no marker, an ordinary pane, no placement, not a backdrop: the
    // HUD's image as drawn.
    Inputs inputs = in_play(2, 0, PC_VR_PANE_NONE);
    Outcome result = run_now(&inputs);
    CHECK(last_route == VR_DRAW_HUD && result.goes_on && result.vr_draw_to_hud_image && !result.black && !result.erase);
    CHECK(result.active == 0 && called(&result, EVENT_FADE) && called(&result, EVENT_MARKER));
    CHECK(called(&result, EVENT_SCREEN_KIND) && called(&result, EVENT_PANE_LOG + 16 * 0));
    // An enemy marker in play: a world screen's draw linked to a report, which the marker's placement takes: into its
    // eye's half, moved, not to the HUD's image; its screen not read (it has a link).
    inputs = in_play(3, 77, PC_VR_PANE_NONE);
    inputs.marker_placed = true;
    result = run_now(&inputs);
    CHECK(last_route == VR_DRAW_EYE_HALF_MARKER && result.goes_on && !result.vr_draw_to_hud_image && result.active == 1);
    CHECK(!called(&result, EVENT_SCREEN_KIND) && called(&result, EVENT_FADE) && called(&result, EVENT_PANE_LOG + 16 * 2));
    // A marker the placement refuses (behind the eye, say) stays on the panel.
    inputs.marker_placed = false;
    result = run_now(&inputs);
    CHECK(last_route == VR_DRAW_HUD && result.vr_draw_to_hud_image && result.active == 0);
    // The same marker over a held scene (a quick menu): left out, counted for the shots' log, skipped, nothing tried,
    // the move left as the last routed draw left it.
    inputs = over_held_scene(3, 77, PC_VR_PANE_NONE);
    inputs.marker_placed = true;
    result = run_now(&inputs);
    CHECK(last_route == VR_DRAW_LEFT_OUT_HELD_WORLD && !result.goes_on && !result.vr_draw_to_hud_image && result.skipped == 1);
    CHECK(result.world_left_out == 1 && result.active == 7 && !called(&result, EVENT_MARKER));
    CHECK(result.over_world && result.over_world_known);  // (the latch decided at this first HUD draw)
    // The game's fixed HUD place counts as a marker value there too.
    inputs = over_held_scene(2, PC_VR_MARKER_FLAT, PC_VR_PANE_NONE);
    result = run_now(&inputs);
    CHECK(last_route == VR_DRAW_LEFT_OUT_HELD_WORLD && result.world_left_out == 1);
    // A reticle's pane over the held scene: left out by its role, its screen not read.
    inputs = over_held_scene(2, 0, PC_VR_PANE_RETICLE);
    result = run_now(&inputs);
    CHECK(last_route == VR_DRAW_LEFT_OUT_HELD_WORLD && !called(&result, EVENT_SCREEN_KIND));
    // The quick menu itself over the held scene: a flat screen's ordinary pane goes to the HUD's image, nothing placed.
    inputs = over_held_scene(1, 0, PC_VR_PANE_NONE);
    result = run_now(&inputs);
    CHECK(last_route == VR_DRAW_HUD && result.vr_draw_to_hud_image && !called(&result, EVENT_FADE) && result.active == 0);
    // A world screen's piece without a link, in play: left out by the guard, counted for its log, skipped.
    inputs = in_play(3, 0, PC_VR_PANE_NONE);
    result = run_now(&inputs);
    CHECK(last_route == VR_DRAW_LEFT_OUT_UNLINKED_WORLD && !result.goes_on && result.unlinked == 1 && result.skipped == 1);
    CHECK(result.world_left_out == 0 && result.active == 7 && called(&result, EVENT_PANE_LOG + 16 * 7));
    // ... and the same with the game's fixed place: drawn flat on the panel, not counted.
    inputs.marker = PC_VR_MARKER_FLAT;
    result = run_now(&inputs);
    CHECK(last_route == VR_DRAW_HUD && result.unlinked == 0);
    // The fade in play: the fade's placement takes it; the marker's is not tried.
    inputs = in_play(1, 0, PC_VR_PANE_FADE);
    inputs.fade_placed = inputs.marker_placed = true;
    result = run_now(&inputs);
    CHECK(last_route == VR_DRAW_EYE_HALF_FADE && result.active == 1 && !result.vr_draw_to_hud_image && !called(&result, EVENT_MARKER));
    // A shop's backdrop over the held scene, by each fate: a box over black; the game's picture of the world left
    // out; the item row's end masks erasing; with SAVAGEWIND_PC_VR_HUD_BACKDROP=1, or without a picture of the screen
    // sampled, as drawn.
    const struct { PcVrPaneRole role; bool backdrop_switch, samples_screen; VrDrawRoute route; bool goes_on, black, erase; }
        backdrops[] = {
            {PC_VR_PANE_BOX, false, true, VR_DRAW_HUD_OVER_BLACK, true, true, false},
            {PC_VR_PANE_WORLD_PICTURE, false, true, VR_DRAW_LEFT_OUT_BACKDROP, false, false, false},
            {PC_VR_PANE_EDGE_MASK, false, true, VR_DRAW_HUD_ERASE, true, true, true},
            {PC_VR_PANE_BOX, true, true, VR_DRAW_HUD, true, false, false},
            {PC_VR_PANE_BOX, false, false, VR_DRAW_HUD, true, false, false},
        };
    for (unsigned i = 0; i < sizeof backdrops / sizeof backdrops[0]; ++i) {
        inputs = over_held_scene(1, 0, backdrops[i].role);
        inputs.quick_menu = false;
        inputs.shop_open = true;
        inputs.backdrop_switch = backdrops[i].backdrop_switch;
        inputs.texture_set = backdrops[i].samples_screen ? 1 : 0;
        result = run_now(&inputs);
        CHECK(result.over_world && last_route == backdrops[i].route && result.goes_on == backdrops[i].goes_on);
        CHECK(result.black == backdrops[i].black && result.erase == backdrops[i].erase);
        CHECK(result.vr_draw_to_hud_image == vr_draw_route_to_hud_image(backdrops[i].route));
        CHECK(result.skipped == !backdrops[i].goes_on);
    }
    // Not a HUD draw: the scene's own draws (no screen), a draw with two colour targets, a draw into another image,
    // the HUD panel off: drawn where the game draws them, nothing moved, the route not asked.
    for (unsigned i = 0; i < 4; ++i) {
        inputs = in_play(i ? 2 : 0, 0, PC_VR_PANE_NONE);
        if (i == 1) inputs.one_target = false;
        if (i == 2) inputs.final_image = false;
        if (i == 3) inputs.panel_on = false;
        result = run_now(&inputs);
        CHECK(last_route == VR_DRAW_FINAL_IMAGE && result.goes_on && !result.vr_draw_to_hud_image && result.active == 0);
        CHECK(!called(&result, EVENT_SCREEN_KIND) && !called(&result, EVENT_FADE));
    }
    // The held-scene decision: a quick menu or a shop over a held scene with the HUD panel on; not the pause menu,
    // not without a held scene, not with the HUD panel off, not once the frame drew a scene.
    CHECK(vr_frame_over_held_scene(true, true, true, true, false) && vr_frame_over_held_scene(true, true, true, false, true));
    CHECK(!vr_frame_over_held_scene(true, true, true, false, false));
    CHECK(!vr_frame_over_held_scene(true, false, true, true, true) && !vr_frame_over_held_scene(true, true, false, true, true));
    CHECK(!vr_frame_over_held_scene(false, true, true, true, true));
    // (through the hub: the shop's screen is not read in a quick menu; nothing is read without a held scene; the latch
    // is left alone once a scene was seen)
    inputs = over_held_scene(1, 0, PC_VR_PANE_NONE);
    result = run_now(&inputs);
    CHECK(result.over_world && called(&result, EVENT_QUICK_UI) && !called(&result, EVENT_SHOP_OPEN));
    inputs.scene_held = false;
    result = run_now(&inputs);
    CHECK(!result.over_world && result.over_world_known && !called(&result, EVENT_PANEL_DEPTH) && last_route == VR_DRAW_FINAL_IMAGE);
    inputs = over_held_scene(1, 0, PC_VR_PANE_NONE);
    inputs.scene_seen = true;
    result = run_now(&inputs);
    CHECK(!result.over_world_known && !called(&result, EVENT_QUICK_UI) && last_route == VR_DRAW_FINAL_IMAGE);
    // The pure route: the screen's kind is never looked at where vr_draw_route_reads_screen says it is not read.
    unsigned unread_screen_disagreements = 0;
    for (unsigned over = 0; over < 2; ++over)
        for (unsigned link = 0; link < 3; ++link)
            for (unsigned role = 0; role <= PC_VR_PANE_BOX; ++role) {
                if (vr_draw_route_reads_screen(over, (VrDrawLink)link, (PcVrPaneRole)role)) continue;
                const VrDrawRoute unread = vr_draw_route_ask(&(VrDrawTags){over, (VrDrawLink)link, (PcVrPaneRole)role,
                                                                          PC_VR_SCREEN_NONE});
                for (unsigned kind = 0; kind <= PC_VR_SCREEN_WORLD; ++kind)
                    unread_screen_disagreements +=
                        vr_draw_route_ask(&(VrDrawTags){over, (VrDrawLink)link, (PcVrPaneRole)role,
                                                        (PcVrScreenKind)kind}) != unread;
            }
    CHECK(unread_screen_disagreements == 0);

    // ---- Against the code as it was, on every combination of the discrete inputs (the textures: two ordinary ones,
    // or an ordinary one and a render target; every texture set in the backdrop's own loop below).
    static const uint32_t markers[3] = {0, PC_VR_MARKER_FLAT, 77};
    static const unsigned screens[5] = {0, 1, 2, 3, 300};
    static const unsigned sizes[] = {2, 2, 2, 2, 2, 2, 2, 4, 2, 2, 2, 2, 2, 5, 3, PC_VR_PANE_BOX + 1, 2, 2, 2, 2, 2};
    const unsigned dimensions = sizeof sizes / sizeof sizes[0];
    uint64_t combinations = 1;
    for (unsigned i = 0; i < dimensions; ++i) combinations *= sizes[i];
    uint64_t disagreements = 0, routes[VR_DRAW_LEFT_OUT_BACKDROP + 1] = {0};
    for (uint64_t combination = 0; combination < combinations; ++combination) {
        unsigned value[sizeof sizes / sizeof sizes[0]];
        uint64_t rest = combination;
        for (unsigned i = 0; i < dimensions; ++i) {
            value[i] = (unsigned)(rest % sizes[i]);
            rest /= sizes[i];
        }
        const Inputs case_inputs = {value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7],
                                    value[8], value[9], value[10], value[11], value[12], screens[value[13]],
                                    markers[value[14]],
                                    (PcVrPaneRole)value[15], value[16] ? 5 : 0, value[17], value[18], value[19],
                                    value[20]};
        static Outcome before, now;
        run_before_into(&case_inputs, &before);
        run_now_into(&case_inputs, &now);
        ++routes[last_route];
        if (!same_outcome(&before, &now)) {
            if (++disagreements < 4) printf("disagreement at combination %llu\n", (unsigned long long)combination);
        }
    }
    CHECK(disagreements == 0);
    CHECK(combinations == 47185920 && event_count_most < EVENTS_MAX);
    // (every route is reached)
    for (unsigned route = 0; route <= VR_DRAW_LEFT_OUT_BACKDROP; ++route)
        CHECK(route == VR_DRAW_EYE_HALF || routes[route] > 0);
    printf("vr_draw_route exhaustive: %llu combinations, %llu disagreements; routes: final image %llu, HUD %llu, over "
           "black %llu, erase %llu, marker %llu, fade %llu, left out held %llu, unlinked %llu, backdrop %llu\n",
           (unsigned long long)combinations, (unsigned long long)disagreements,
           (unsigned long long)routes[VR_DRAW_FINAL_IMAGE], (unsigned long long)routes[VR_DRAW_HUD],
           (unsigned long long)routes[VR_DRAW_HUD_OVER_BLACK], (unsigned long long)routes[VR_DRAW_HUD_ERASE],
           (unsigned long long)routes[VR_DRAW_EYE_HALF_MARKER], (unsigned long long)routes[VR_DRAW_EYE_HALF_FADE],
           (unsigned long long)routes[VR_DRAW_LEFT_OUT_HELD_WORLD],
           (unsigned long long)routes[VR_DRAW_LEFT_OUT_UNLINKED_WORLD],
           (unsigned long long)routes[VR_DRAW_LEFT_OUT_BACKDROP]);

    // ---- The backdrop's inputs in full, on HUD draws in play and over the held scene: every texture set (none
    // sampled, one, two, three; a match on the first, the second or the third; by the render target's flag, the display
    // image's or both; a texture not bound), every role, the switch, the screen (a tag past the table included), the
    // marker value, the pane number and the placements' answers.
    uint64_t backdrop_combinations = 0, backdrop_disagreements = 0, texture_tests = 0;
    uint64_t backdrop_routes[VR_DRAW_LEFT_OUT_BACKDROP + 1] = {0};
    for (unsigned over = 0; over < 2; ++over)
        for (unsigned screen = 1; screen < 5; ++screen)
            for (unsigned marker = 0; marker < 3; ++marker)
                for (unsigned role = 0; role <= PC_VR_PANE_BOX; ++role)
                    for (unsigned pane = 0; pane < 2; ++pane)
                        for (unsigned placements = 0; placements < 4; ++placements)
                            for (unsigned backdrop_switch = 0; backdrop_switch < 2; ++backdrop_switch)
                                for (unsigned texture_set = 0; texture_set < TEXTURE_SETS; ++texture_set) {
                                    Inputs case_inputs = over ? over_held_scene(screens[screen], markers[marker],
                                                                                (PcVrPaneRole)role)
                                                              : in_play(screens[screen], markers[marker],
                                                                        (PcVrPaneRole)role);
                                    case_inputs.pane = pane ? 5 : 0;
                                    case_inputs.fade_placed = placements & 1;
                                    case_inputs.marker_placed = placements >> 1;
                                    case_inputs.backdrop_switch = backdrop_switch;
                                    case_inputs.texture_set = texture_set;
                                    static Outcome before, now;
                                    run_before_into(&case_inputs, &before);
                                    run_now_into(&case_inputs, &now);
                                    ++backdrop_combinations;
                                    ++backdrop_routes[last_route];
                                    backdrop_disagreements += !same_outcome(&before, &now);
                                    texture_tests += called(&now, EVENT_TEXTURE);
                                }
    CHECK(backdrop_combinations == 18432 && backdrop_disagreements == 0 && texture_tests > 900);
    CHECK(backdrop_routes[VR_DRAW_HUD_OVER_BLACK] && backdrop_routes[VR_DRAW_HUD_ERASE] &&
          backdrop_routes[VR_DRAW_LEFT_OUT_BACKDROP]);
    printf("vr_draw_route backdrops: %llu combinations, %llu disagreements; draws that tested textures %llu; over black "
           "%llu, erase %llu, left out %llu\n", (unsigned long long)backdrop_combinations,
           (unsigned long long)backdrop_disagreements, (unsigned long long)texture_tests,
           (unsigned long long)backdrop_routes[VR_DRAW_HUD_OVER_BLACK],
           (unsigned long long)backdrop_routes[VR_DRAW_HUD_ERASE],
           (unsigned long long)backdrop_routes[VR_DRAW_LEFT_OUT_BACKDROP]);

    printf("vr_draw_route fixture: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
