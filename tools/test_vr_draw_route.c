// Asset-free check of where a 2D draw into the final image goes (runtime/nvn/vulkan/vr_draw_route.h, D057, D072, D073,
// D088, D097): the route, the held-scene decision, and exec.c's hub around them. Named cases a reader can follow: a
// hearts draw in play goes to the HUD's image; an enemy marker into its eye's half; the same marker over a held scene
// is left out; a reticle there is left out without its screen being read; a world screen's piece without a link is
// left out and counted for the guard; the fade over each eye's view; a shop's backdrop by each fate; a draw that is
// not a HUD draw; the quick menu, the shop, the pause menu, no held scene and a scene after all for the held-scene
// decision; then, on the pure route, that a screen's kind it does not read makes no difference. exec.c's hub is
// transcribed below with the executor's state as this file's. No renderer, GPU, game or headset is involved.
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
static unsigned event_count;
static void record(unsigned code, const char *name) {
    if (event_count < EVENTS_MAX) events[event_count] = (Event){code, name};
    ++event_count;
}

// ---------------------------------------------------------------------------------- the executor, as the code reads it
typedef struct { uint32_t flags; } NvnTexture;
typedef struct { unsigned slot; } ShaderTexture;
typedef struct { int ntex; ShaderTexture tex[3]; } ShaderInfo;
typedef struct { ShaderInfo *fi; } Prog;
static const NvnTexture ordinary_art = {2}, other_art = {4}, render_target = {8};
static const NvnTexture *sampled[3];  // (what each texture the fragment stage samples resolves to; NULL: none bound)
static ShaderInfo fragment_info = {2, {{0}, {1}, {2}}};
static Prog program = {&fragment_info};
static const NvnTexture *shader_texture_object(int stage, const ShaderTexture *texture) {
    (void)stage;
    return sampled[texture->slot];
}

// The textures a draw samples, by number: 0, two ordinary ones (no picture of the screen); 1, an ordinary one and a
// render target.
#define TEXTURE_SETS 2
static void sample_textures(unsigned set) {
    static const struct { int count; const NvnTexture *textures[3]; } sets[TEXTURE_SETS] = {
        {2, {&ordinary_art, &other_art}}, {2, {&ordinary_art, &render_target}},
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
static bool vr_draw_to_hud_image, vr_backdrop_pictures_black, vr_backdrop_erase;
static struct { uint8_t active; } vr_world_hud;  // (a byte: 7 before the draw shows whether the draw set it)
static unsigned stat_skipped, vr_frame_world_left_out;
static struct { uint64_t frame_draws; } vr_unlinked_world;

static bool vr_backdrop_source(const NvnTexture *texture) {
    record(EVENT_TEXTURE, NULL);
    return texture && (texture->flags & 9);
}

// ---------------------------------------------------------------------------------------------- the hub (exec.c)
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
    if (!fate) return VR_BACKDROP_NONE;
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

// draw_'s routing; false where draw_ returns.
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

// The state before a draw: everything this file reads, from the inputs; the counters and the record empty.
typedef struct {
    bool eyes_on, hud_image_usable, one_target, final_image, placement_seen, panel_on, scene_seen;
    unsigned certified;
    bool latch_known, latch_value, scene_held, quick_menu, shop_open;
    unsigned screen;  // 0: no screen's draw; 1, 2, 3: a flat, the HUD's, a world screen; 300: a tag past the table
    uint32_t marker;
    PcVrPaneRole role;
    uint32_t pane;
    bool fade_placed, marker_placed;
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

// Whether the record holds this call.
static bool called(const Outcome *result, unsigned code) {
    for (unsigned i = 0; i < result->event_count; ++i)
        if (result->events[i].code == code) return true;
    return false;
}

int main(void) {
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
    // out; the item row's end masks erasing; without a picture of the screen sampled, as drawn.
    const struct { PcVrPaneRole role; bool samples_screen; VrDrawRoute route; bool goes_on, black, erase; }
        backdrops[] = {
            {PC_VR_PANE_BOX, true, VR_DRAW_HUD_OVER_BLACK, true, true, false},
            {PC_VR_PANE_WORLD_PICTURE, true, VR_DRAW_LEFT_OUT_BACKDROP, false, false, false},
            {PC_VR_PANE_EDGE_MASK, true, VR_DRAW_HUD_ERASE, true, true, true},
            {PC_VR_PANE_BOX, false, VR_DRAW_HUD, true, false, false},
        };
    for (unsigned i = 0; i < sizeof backdrops / sizeof backdrops[0]; ++i) {
        inputs = over_held_scene(1, 0, backdrops[i].role);
        inputs.quick_menu = false;
        inputs.shop_open = true;
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

    printf("vr_draw_route fixture: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
