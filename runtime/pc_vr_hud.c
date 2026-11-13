// PC VR world-tied HUD (D073, D088): where the game puts HUD pieces that belong to something in the world (enemy
// markers, health bars, the Z-target cursor, item names, speech balloons), so the renderer can draw each one at its
// 3D point in both eyes instead of on the HUD's flat panel. Interface: runtime/pc_vr_hud.h.
//
// How the game places such a piece. The HUD has a helper that turns a world position into a HUD layout position
// (1.9.0 0x7000e1dde0): it projects the point with the UI camera (a copy of the world camera: fovy 50 degrees, 16:9),
// scales the result to the 1280x720 layout and lets the Labo VR object adjust it last. Its caller then copies the two
// floats into the translate of the piece's UI pane. Patched game code (tools/patches.py) reports to this file:
//
//   1. Each helper result (rt_hud_point) and each speech balloon placement (rt_balloon_point). The reports go into a
//      small ring buffer with a sequence number, where the renderer finds a draw's report by its number.
//   2. Which UI screen (rt_screen_draw_begin) and which pane (rt_pane_draw_begin) the game is drawing on the current
//      thread. The command recorder (runtime/nvn/cmdbuf.c) stores both in every draw it records meanwhile.
//   3. Each store of a helper result into a piece's pane (rt_marker_pane_bind, rt_marker_component_bind; a balloon's
//      in rt_balloon_point), which binds that pane to the report it stores (the marker links, at the end of this file).
//
// The bindings of point 3 decide which marker a draw belongs to (pc_vr_marker_drawing): the report bound to the draw's
// pane or the nearest of its parents that has a binding, however old; a pane the game gave a fixed HUD place is drawn
// flat; a pane the game moved without a known world point, or without a binding, has no marker. Nothing is matched by
// position or nearness.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_sites.h"
#include "hle.h"
#include "kernel.h"
#include "pc_vr.h"
#include "pc_vr_hud.h"
#include "rt.h"

uint64_t renderer_present_count(void);

// ------------------------------------------------------------------------------------------ the ring of marker reports
// The newest HUD_POINTS reports, oldest overwritten first. Written on the game thread, read on the render thread.
#define HUD_POINTS 128
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static PcVrHudPoint ring[HUD_POINTS];
static unsigned ring_next;   // the slot the next report goes into
static unsigned ring_count;  // how many slots hold a report (at most HUD_POINTS)
static uint32_t last_seq;    // the sequence number given to the newest report

// The newest report is at ring_next - 1, the one before it at ring_next - 2, and so on.
#define RING_NEWEST(age) (&ring[(ring_next + HUD_POINTS - 1 - (age)) % HUD_POINTS])

// True when the guest range [address, address + size) lies inside the guest's address space.
static bool guest(uint64_t address, uint64_t size) {
    return address >= GUEST_LO && address < GUEST_HI && size <= GUEST_HI - address;
}

// Stores one report in the ring, stamped with the current frame and the next sequence number, and returns that
// number (also put in point->seq). A report with a position that is not a finite number in a sane range is dropped
// (0); an odd offset is treated as no offset. (x == x is false only for NaN.)
static uint32_t report(PcVrHudPoint *point) {
    if (!(point->offset == point->offset) || point->offset > 4 || point->offset < -4) point->offset = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (!(point->layout[i] == point->layout[i]) || point->layout[i] > 1e6f || point->layout[i] < -1e6f) return 0;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!(point->camera[i] == point->camera[i]) || point->camera[i] > 1e7f || point->camera[i] < -1e7f) return 0;
    }
    point->frame = renderer_present_count();
    pthread_mutex_lock(&ring_lock);
    // (1 to PC_VR_MARKER_SEQ_MAX: 0 means "no marker" to the renderer, and the numbers above are other answers)
    if (++last_seq > PC_VR_MARKER_SEQ_MAX) last_seq = 1;
    point->seq = last_seq;
    ring[ring_next] = *point;
    ring_next = (ring_next + 1) % HUD_POINTS;
    if (ring_count < HUD_POINTS) ++ring_count;
    pthread_mutex_unlock(&ring_lock);
    return point->seq;
}

// The helper's last report on this thread, for the store of its result into a pane that follows the call (the marker
// links, at the end of this file). Cleared each time the helper reports, so a report that report() dropped leaves
// none, and used up by the first store after it. Stamped with the boot it was made in: a guest stack address from
// before a reboot never binds. In 1.9.0 the helper has one exit before its report, for a null output pointer, which
// none of its callers passes, so every call reports. All ten callers are patched: the eight that store into the pane,
// the one that calls the component's set-position function, and the wheel-like piece, where the helper's call
// precedes the flag's assignment (pcvr_marker_wheel_follow) on the same straight path.
typedef enum { HELPER_REPORT_NONE, HELPER_REPORT_READY, HELPER_REPORT_USED } HelperReportState;
static _Thread_local struct {
    PcVrHudPoint point;  // the report
    uint64_t output;     // the guest address the helper wrote the position to (its x19)
    uint32_t boot;       // marker_link_boot when it was made
    uint8_t state;       // HelperReportState
} helper_report;
static _Atomic uint32_t marker_link_boot;  // counts the reboots (pc_vr_marker_link_reset)

// (the marker links, at the end of this file)
#define MARKER_LINK_BALLOON_SITE 12  // a speech balloon's store (tools/patches.py numbers the other stores 1 to 11)
static void marker_link_store(uint64_t pane, const PcVrHudPoint *point, uint32_t site);
static void marker_link_refuse(uint64_t pane, uint64_t source, uint32_t site, const char *why);

// Called by the patched helper (tools/patches.py _hud_point) once it has computed a position. layout_address is the
// helper's x19, pointing at the two layout floats; camera_address is sp + 0x14, pointing at the point in the UI
// camera's space (three floats). The helper's caller passed the horizontal offset in s8 (the low 32 bits of v8): the
// stamina wheel, for one, sits beside Link rather than on him.
void rt_hud_point(Cpu *cpu, uint64_t layout_address, uint64_t camera_address) {
    helper_report.state = HELPER_REPORT_NONE;
    if (!cpu || !pc_vr_enabled() || !guest(layout_address, 8) || !guest(camera_address, 12)) return;
    PcVrHudPoint point = {0};
    memcpy(point.layout, (const void *)(uintptr_t)layout_address, sizeof point.layout);
    memcpy(point.camera, (const void *)(uintptr_t)camera_address, sizeof point.camera);
    const uint32_t offset_bits = (uint32_t)cpu->v[8];
    memcpy(&point.offset, &offset_bits, sizeof point.offset);
    if (!report(&point)) return;
    helper_report.point = point;
    helper_report.output = layout_address;
    helper_report.boot = atomic_load_explicit(&marker_link_boot, memory_order_relaxed);
    helper_report.state = HELPER_REPORT_READY;
}

// D088: a speech balloon. Called by the patched balloon code (tools/patches.py _balloon_point) right after the game
// stored the balloon pane's x. At that point the guest registers hold:
//   s0          the pane's x (layout units)
//   s8          the pane's y
//   s9          half the layout's width times the head's NDC x, which is the pane's x before its horizontal offset
//   [x29-0x18]  half the layout's width (then half its height)
//   [sp+0x1c]   the speaker's head in the UI camera's space (three floats)
// The offset is stored as the helper's is, in NDC: (x - s9) divided by half the layout's width. The pane (x8) is bound
// to the report (the marker links); without a report, its world point is unknown.
void rt_balloon_point(Cpu *cpu) {
    if (!cpu || !pc_vr_enabled() || !guest(cpu->sp + 0x1c, 12) || !guest(cpu->x[29] - 0x18, 8)) return;
    PcVrHudPoint point = {0};
    uint32_t x_bits = (uint32_t)cpu->v[0], y_bits = (uint32_t)cpu->v[8], unshifted_x_bits = (uint32_t)cpu->v[9];
    float unshifted_x, half_width;
    memcpy(&point.layout[0], &x_bits, 4);
    memcpy(&point.layout[1], &y_bits, 4);
    memcpy(&unshifted_x, &unshifted_x_bits, 4);
    memcpy(&half_width, (const void *)(uintptr_t)(cpu->x[29] - 0x18), 4);
    memcpy(point.camera, (const void *)(uintptr_t)(cpu->sp + 0x1c), sizeof point.camera);
    point.offset = half_width > 1 ? (point.layout[0] - unshifted_x) / half_width : 0;
    if (report(&point)) marker_link_store(cpu->x[8], &point, MARKER_LINK_BALLOON_SITE);
    else marker_link_refuse(cpu->x[8], 0, MARKER_LINK_BALLOON_SITE, "the balloon's own report was dropped");
}

// (the marker links, at the end of this file)
static bool marker_link_report_by_seq(uint32_t seq, PcVrHudPoint *out);

// From the ring, or else from the bindings' copies: a piece the game stopped placing (a pause, a quick menu, a piece
// that stays where it is) keeps its report however many newer ones pushed it out of the ring.
bool pc_vr_hud_point_by_seq(uint32_t seq, PcVrHudPoint *out) {
    if (!seq || seq > PC_VR_MARKER_SEQ_MAX) return false;
    bool found = false;
    pthread_mutex_lock(&ring_lock);
    for (unsigned age = 0; age < ring_count && !found; ++age) {
        const PcVrHudPoint *point = RING_NEWEST(age);
        if (point->seq == seq) {
            *out = *point;
            found = true;
        }
    }
    pthread_mutex_unlock(&ring_lock);
    return found || marker_link_report_by_seq(seq, out);
}

RT_ON_RESET(pc_vr_hud_reset) {
    pthread_mutex_lock(&ring_lock);
    ring_next = ring_count = 0;
    pthread_mutex_unlock(&ring_lock);
}

// ----------------------------------------------------------------------------------------------- the game's UI screens
// The game's UI screen manager (its address: g_site.screen_mgr) holds one object per screen id. Each screen names its
// layout ("PauseMenu_00", "MessageTipsRunTime_00", ...) and keeps a display state (runtime/pc_cursor.c reads "shown"
// as active). Dialogs are created when they open and deleted once closed (the inventory's item menu,
// SystemWindow_00). These are read from the render thread while the game runs, so every read stays in mapped memory
// (the main executable's image or the heap): a screen deleted meanwhile reads stale bytes but never faults.
#define SCREEN_MGR_COUNT 0x28    // uint32_t: the number of screen ids
#define SCREEN_MGR_SCREENS 0x30  // guest pointer to the array of screen pointers, by id (NULL: none)
#define SCREEN_MGR_SIZE 0x38     // the manager bytes read
#define SCREEN_LAYOUT_NAME 0xd0  // guest pointer to the screen's layout name
#define SCREEN_STATE 0xfe        // uint8_t: 0 hidden, 1 opening, 2 shown, 3 closing
#define SCREEN_SIZE 0x100        // the screen bytes read
#define SCREEN_NAME_MAX 48       // longest layout name accepted, with its terminator

// True when [address, address + size) is in the main executable's image or in the heap.
static bool mapped(uint64_t address, uint64_t size) {
    const uint64_t heap_top = HEAP_BASE + kernel_heap_size();
    return (address >= MAIN_BASE && address < g_site.main_end && size <= g_site.main_end - address) ||
           (address >= HEAP_BASE && address < heap_top && size <= heap_top - address);
}

// The screen's layout name, or NULL when it does not look like one: 3 to 47 printable ASCII characters.
static const char *screen_name(uint64_t screen) {
    if (!mapped(screen, SCREEN_SIZE)) return NULL;
    const uint64_t name_address = *(const uint64_t *)(uintptr_t)(screen + SCREEN_LAYOUT_NAME);
    if (!mapped(name_address, SCREEN_NAME_MAX)) return NULL;
    const char *name = (const char *)(uintptr_t)name_address;
    for (unsigned i = 0; i < SCREEN_NAME_MAX; ++i) {
        if (!name[i]) return i >= 3 ? name : NULL;
        if (name[i] < 0x20 || name[i] >= 0x7f) return NULL;
    }
    return NULL;
}

// Calls fn for every screen that exists, with its id, layout name and display state.
typedef void ScreenFn(unsigned id, const char *name, unsigned state, void *context);
static void screens_each(ScreenFn *callback, void *context) {
    if (!g_site.screen_mgr) return;
    const uint64_t manager = *(const uint64_t *)(uintptr_t)g_site.screen_mgr;
    if (!mapped(manager, SCREEN_MGR_SIZE)) return;
    const uint32_t screen_count = *(const uint32_t *)(uintptr_t)(manager + SCREEN_MGR_COUNT);
    const uint64_t screens = *(const uint64_t *)(uintptr_t)(manager + SCREEN_MGR_SCREENS);
    if (screen_count > 256 || !mapped(screens, 8ull * screen_count)) return;
    for (uint32_t id = 0; id < screen_count; ++id) {
        const uint64_t screen = ((const uint64_t *)(uintptr_t)screens)[id];
        const char *name = screen ? screen_name(screen) : NULL;
        if (name) callback(id, name, *(const uint8_t *)(uintptr_t)(screen + SCREEN_STATE), context);
    }
}

// Sets *(bool *)context when the shop's screen is up (opening, shown or closing).
static void shop_screen(unsigned id, const char *name, unsigned state, void *context) {
    (void)id;
    if (state && !strcmp(name, "ShopBG_00")) *(bool *)context = true;
}

bool pc_vr_shop_open(void) {
    bool open = false;
    if (pc_vr_enabled()) screens_each(shop_screen, &open);
    return open;
}

// SAVAGEWIND_LOG_SCREENS: the state and name last logged for each screen id, so only changes are logged.
static uint8_t logged_state[256];
static const char *logged_name[256];
static void log_change(unsigned id, const char *name, unsigned state, void *context) {
    (void)context;
    if (id >= 256 || (logged_name[id] == name && logged_state[id] == state)) return;
    LOG("[pc-vr-screens] present %llu %s (id %#x) %s\n", (unsigned long long)renderer_present_count(), name, id,
        state == 0 ? "hidden" : state == 1 ? "opening" : state == 2 ? "shown" : state == 3 ? "closing" : "?");
    logged_name[id] = name;
    logged_state[id] = (uint8_t)state;
}

// D088: the screen each UI draw comes from. The game's screen loop (tools/patches.py _screen_draw) calls begin before
// it draws a screen and end after, on its own thread; the recorder (runtime/nvn/cmdbuf.c) stores the current tag in
// every draw recorded meanwhile, and the renderer asks what kind of screen made the draw (pc_vr_screen_kind).
// A tag is the screen's id + 1, so that 0 can mean "not inside a screen's draw".
static _Thread_local uint32_t screen_tag_drawing;

// (the marker links, at the end of this file: a world screen's new object clears the bindings)
static void marker_link_screen_seen(uint32_t id, uint64_t screen);

uint32_t rt_screen_draw_begin(uint64_t screen, uint32_t id) {
    const uint32_t previous = screen_tag_drawing;
    screen_tag_drawing = id < 256 ? id + 1 : 0;
    if (id < 256 && pc_vr_enabled()) marker_link_screen_seen(id, screen);
    return previous;
}

void rt_screen_draw_end(uint32_t previous) { screen_tag_drawing = previous; }

uint32_t pc_vr_screen_drawing(void) { return pc_vr_enabled() ? screen_tag_drawing : 0; }

const char *pc_vr_screen_name(uint32_t tag) {
    if (!tag || tag > 256 || !g_site.screen_mgr) return NULL;
    const uint64_t manager = *(const uint64_t *)(uintptr_t)g_site.screen_mgr;
    if (!mapped(manager, SCREEN_MGR_SIZE)) return NULL;
    const uint32_t screen_count = *(const uint32_t *)(uintptr_t)(manager + SCREEN_MGR_COUNT);
    const uint64_t screens = *(const uint64_t *)(uintptr_t)(manager + SCREEN_MGR_SCREENS);
    if (tag > screen_count || screen_count > 256 || !mapped(screens, 8ull * screen_count)) return NULL;
    const uint64_t screen = ((const uint64_t *)(uintptr_t)screens)[tag - 1];
    return screen ? screen_name(screen) : NULL;
}

// What kind of screen a tagged draw came from, by its layout name (zeldaret/botw class names; the play sessions'
// screen logs and pane trees). Whatever the game anchors to the world, it positions with the helper, and draws of it
// link to their marker through their pane's binding (pc_vr_marker_drawing; speech balloons through rt_balloon_point).
// Two screens hold nothing else (pane trees, 2026-11-11): MainScreen3D_00 (enemy gauges and marks, the Z-target
// cursor, item names, the stamina wheel) and Message3D_00 (the speech balloons). The renderer leaves out these
// screens' draws over a held scene (a quick menu, a shop), and anywhere their draws whose panes have no binding
// (exec.c draw_). The flat HUD (MainScreen_00) holds the aiming reticles.
//
// Called on the renderer's thread only. A screen's id is its type, so a kind once read stays right and is cached. A
// screen that is gone by the time its draw executes (a closed popup) reads as flat, which is what it was.
PcVrScreenKind pc_vr_screen_kind(uint32_t tag) {
    static uint8_t kind_by_tag[257];  // the kind + 1; 0: not read yet
    if (!tag || tag > 256) return PC_VR_SCREEN_NONE;
    if (kind_by_tag[tag]) return (PcVrScreenKind)(kind_by_tag[tag] - 1);
    const char *name = pc_vr_screen_name(tag);
    if (!name) return PC_VR_SCREEN_FLAT;
    const PcVrScreenKind kind = !strcmp(name, "MainScreen_00") ? PC_VR_SCREEN_HUD :
        !strcmp(name, "MainScreen3D_00") || !strcmp(name, "Message3D_00") ? PC_VR_SCREEN_WORLD : PC_VR_SCREEN_FLAT;
    kind_by_tag[tag] = (uint8_t)(kind + 1);
    LOG("[pc-vr-screens] %s (id %#x): %s\n", name, tag - 1,
        kind == PC_VR_SCREEN_HUD ? "the flat HUD, with the reticles" :
        kind == PC_VR_SCREEN_WORLD ? "tied to the world: left out over a held scene, and without a marker link" :
        "flat but for what its panes' markers anchor");
    return kind;
}

// ------------------------------------------------------------------------------------------- the UI pane being drawn
// D088: the marker each draw over the world belongs to, exactly. The game positions a marker's pane with the helper's
// result: its callers copy the two floats into the pane's translate (nn::ui2d::Pane +0x30 and +0x34) and set the
// pane's dirty flag (+0x58), and each such store binds the pane to that report (the marker links, at the end of this
// file). The pane's children (a health bar's frame and fill, the Z-target cursor's arrow, a balloon's background) are
// drawn inside it, each under its own pane while it draws. So the pane being drawn, or one of its parents, is the one
// the game bound.
#define PANE_PARENT 0x18  // guest pointer to the parent pane (NULL at the root)
#define PANE_SIZE 0x60    // the pane bytes that must be mapped to read it

// The pane the game is drawing on this thread (tools/patches.py _pane_draw), 0 outside a pane's draw.
static _Thread_local uint64_t pane_drawing;

uint64_t rt_pane_draw_begin(uint64_t pane) {
    const uint64_t previous = pane_drawing;
    pane_drawing = pane;
    return previous;
}

void rt_pane_draw_end(uint64_t previous) { pane_drawing = previous; }

// How many panes up the parent chain to look for a marker's binding (the pane being drawn and 15 parents).
#define PANE_DEPTH 16

// (the marker links, at the end of this file)
static uint32_t marker_link_answer(const uint64_t *panes, unsigned depth);

uint32_t pc_vr_marker_drawing(uint32_t screen_tag) {
    if (!pane_drawing || !screen_tag || !pc_vr_enabled()) return 0;
    // The pane being drawn and its parents, nearest first; the binding of the nearest that has one answers.
    uint64_t panes[PANE_DEPTH];
    unsigned depth = 0;
    for (uint64_t pane = pane_drawing; pane && depth < PANE_DEPTH && mapped(pane, PANE_SIZE);
         pane = *(const uint64_t *)(uintptr_t)(pane + PANE_PARENT))
        panes[depth++] = pane;
    return marker_link_answer(panes, depth);
}

// --------------------------------------------------------------------------------------------- the UI's screen parallax
static _Atomic bool ui_parallax_zeroed;

bool pc_vr_ui_centred(void) { return atomic_load_explicit(&ui_parallax_zeroed, memory_order_relaxed); }

uint32_t rt_ui_parallax(uint32_t amount_bits) {
    const bool zeroed = pc_vr_enabled() && !pc_vr_config()->play.ui_parallax;
    atomic_store_explicit(&ui_parallax_zeroed, zeroed, memory_order_relaxed);
    static unsigned logged;
    if (logged < 4) {
        ++logged;
        float amount;
        memcpy(&amount, &amount_bits, sizeof amount);
        LOG("[pc-vr-ui] the game sets a view's UI screen parallax to %.4f: %s\n", amount,
            zeroed ? "stored as zero" : "kept");
    }
    return zeroed ? 0 : amount_bits;
}

// ------------------------------------------------------------------------------------------------ the rune target test
// D078: rune targets (stasis, magnesis) are picked on the GPU: one draw looks for an object around a screen position
// held in a small constants block. The game sets the test up each frame a rune is raised (1.9.0 0x700126f6a8, its
// only caller 0x70012679a4): it takes the next of the test's two constants blocks, lets the Labo VR object adjust the
// position (0.5625, 0.5 in Labo mode) and fills the block. The patched call of that adjustment reports the block
// here, so the renderer knows the test's draw by the constants it binds, whatever the draw looks like.
#define RUNE_TEST_BLOCKS 4  // the newest blocks kept (the game alternates between two)
#define RUNE_TEST_FRAMES 3  // a block counts as the test's for this many presented frames after its setup
// Written on the game thread, read on the render thread for every fragment constants block it binds: plain atomics,
// and the last setup's frame first, so the answer is one load while no rune is raised.
static struct {
    _Atomic uint64_t constants;  // the block's guest address (0: unused)
    _Atomic uint64_t frame;      // renderer_present_count() at its last setup
} rune_test_blocks[RUNE_TEST_BLOCKS];
static _Atomic uint64_t rune_test_frame;  // renderer_present_count() + 1 at the last setup of any block (0: never)

void rt_rune_test(uint64_t constants, uint64_t position) {
    if (!pc_vr_enabled() || !guest(constants, 32)) return;
    const uint64_t now = renderer_present_count();
    // The block's own slot, or else the one set up longest ago.
    unsigned slot = 0;
    for (unsigned i = 0; i < RUNE_TEST_BLOCKS; ++i) {
        if (atomic_load(&rune_test_blocks[i].constants) == constants) {
            slot = i;
            break;
        }
        if (atomic_load(&rune_test_blocks[i].frame) < atomic_load(&rune_test_blocks[slot].frame)) slot = i;
    }
    atomic_store(&rune_test_blocks[slot].frame, now);
    atomic_store(&rune_test_blocks[slot].constants, constants);
    atomic_store(&rune_test_frame, now + 1);
    static unsigned logged;
    if (logged < 4 && guest(position, 8)) {
        ++logged;
        float game_position[2];
        memcpy(game_position, (const void *)(uintptr_t)position, sizeof game_position);
        LOG("[pc-vr-rune-aim] the game sets the target test up at %.4f %.4f, constants at %llx (present %llu)\n",
            game_position[0], game_position[1], (unsigned long long)constants, (unsigned long long)now);
    }
}

bool pc_vr_rune_test_constants(uint64_t address) {
    const uint64_t last = atomic_load_explicit(&rune_test_frame, memory_order_relaxed);
    if (!last || !address) return false;
    const uint64_t now = renderer_present_count();
    if (last + RUNE_TEST_FRAMES <= now) return false;  // (no rune raised)
    for (unsigned i = 0; i < RUNE_TEST_BLOCKS; ++i) {
        if (atomic_load(&rune_test_blocks[i].constants) == address)
            return atomic_load(&rune_test_blocks[i].frame) + RUNE_TEST_FRAMES >= now;
    }
    return false;
}

RT_ON_RESET(pc_vr_rune_test_reset) {
    atomic_store(&rune_test_frame, 0);
    for (unsigned i = 0; i < RUNE_TEST_BLOCKS; ++i) {
        atomic_store(&rune_test_blocks[i].constants, 0);
        atomic_store(&rune_test_blocks[i].frame, 0);
    }
}

// ------------------------------------------------------------------------------------------------- what a pane is
// The game names every UI pane (nn::ui2d: 24 characters at the pane + 0xb8) and groups them under parent panes. The
// names below are the game's own, read from its layouts in play (docs/evidence/pc-vr-aim-hooks-2026-11-10.md):
//   - MainScreen_00 keeps each aiming reticle under one parent: the bow's (Pa_ArrowPointer_00), the runes'
//     (Pa_ItemPointer_00) and the thrown weapon's (Pa_ThrowingPointer_00);
//   - the fade is the screen Fade's P_Black_00;
//   - a menu's picture of the world is a P_Capture_NN pane of the quick menu, the pause menu's background or the
//     shop (the map's: P_MapCapture_00), a list's end mask over it a P_CaptureMask_NN of those menus, and the boxes
//     that show the screen blurred behind them are window bases (W_Base_NN), the sliding pages' P_CaptureSlideL/R_NN
//     and three single panes.
#define PANE_ROLE_NAME 0xb8
#define PANE_ROLE_NAME_LENGTH 24
#define PANE_ROLE_PARENTS 24  // how far up the tree a reticle's parent is looked for
#define PANE_ROLE_CACHE 1024  // per thread: the last pane seen at each slot, by its address

// The pane's name, sanitised, into name (PANE_ROLE_NAME_LENGTH + 1 bytes). False when the pane is not readable.
static bool pane_name(uint64_t pane, char *name) {
    if (!mapped(pane, PANE_ROLE_NAME + PANE_ROLE_NAME_LENGTH)) return false;
    memcpy(name, (const void *)(uintptr_t)(pane + PANE_ROLE_NAME), PANE_ROLE_NAME_LENGTH);
    name[PANE_ROLE_NAME_LENGTH] = 0;
    return true;
}

static bool name_starts(const char *name, const char *prefix) { return !strncmp(name, prefix, strlen(prefix)); }

static PcVrPaneRole pane_role_find(uint64_t pane, const char *name, const char *screen) {
    if (!strcmp(screen, "Fade") && !strcmp(name, "P_Black_00")) return PC_VR_PANE_FADE;
    // (by screen as well as by name: MainScreen_00 has P_Capture_NN panes too, the notice box's own contents)
    if (name_starts(name, "P_CaptureMask_") &&
        (!strcmp(screen, "MainShortCut_00") || !strcmp(screen, "PauseMenu_00") ||
         !strcmp(screen, "ShopBtnList5_00") || !strcmp(screen, "ShopBtnList20_00")))
        return PC_VR_PANE_EDGE_MASK;
    if ((name_starts(name, "P_Capture_") &&
         (!strcmp(screen, "MainShortCut_00") || !strcmp(screen, "PauseMenuBG_00") || !strcmp(screen, "ShopBG_00"))) ||
        (!strcmp(screen, "AppMap_00") && !strcmp(name, "P_MapCapture_00")))
        return PC_VR_PANE_WORLD_PICTURE;
    if (name_starts(name, "W_Base_") || name_starts(name, "P_CaptureSlide") ||
        (!strcmp(screen, "AppHome_00") && !strcmp(name, "P_ScanningLine_00")) ||
        (!strcmp(screen, "MainScreen3D_00") && !strcmp(name, "FBLayout_00")) ||
        (!strcmp(screen, "MessageSp_00") && !strcmp(name, "P_Sh_01")))
        return PC_VR_PANE_BOX;
    if (!strcmp(screen, "MainScreen_00")) {
        char parent_name[PANE_ROLE_NAME_LENGTH + 1];
        memcpy(parent_name, name, sizeof parent_name);
        for (unsigned level = 0; level < PANE_ROLE_PARENTS; ++level) {
            if (!strcmp(parent_name, "Pa_ArrowPointer_00") || !strcmp(parent_name, "Pa_ItemPointer_00") ||
                !strcmp(parent_name, "Pa_ThrowingPointer_00"))
                return PC_VR_PANE_RETICLE;
            if (!mapped(pane, PANE_SIZE)) break;
            pane = *(const uint64_t *)(uintptr_t)(pane + PANE_PARENT);
            if (!pane || !pane_name(pane, parent_name)) break;
        }
    }
    return PC_VR_PANE_NONE;
}

PcVrPaneRole pc_vr_pane_role(uint32_t screen_tag) {
    char name[PANE_ROLE_NAME_LENGTH + 1];
    if (!pane_drawing || !screen_tag || !pane_name(pane_drawing, name)) return PC_VR_PANE_NONE;
    // The same pane as last time at this address, in the same screen: its role without looking again.
    static _Thread_local struct {
        uint64_t pane;
        uint32_t screen_tag;
        char name[PANE_ROLE_NAME_LENGTH + 1];
        uint8_t role;
    } cache[PANE_ROLE_CACHE];
    const unsigned slot = (unsigned)((pane_drawing >> 4) % PANE_ROLE_CACHE);
    if (cache[slot].pane == pane_drawing && cache[slot].screen_tag == screen_tag &&
        !memcmp(cache[slot].name, name, sizeof name))
        return (PcVrPaneRole)cache[slot].role;
    const char *screen = pc_vr_screen_name(screen_tag);
    const PcVrPaneRole role = screen ? pane_role_find(pane_drawing, name, screen) : PC_VR_PANE_NONE;
    cache[slot].pane = pane_drawing;
    cache[slot].screen_tag = screen_tag;
    memcpy(cache[slot].name, name, sizeof name);
    cache[slot].role = (uint8_t)role;
    return role;
}

// --------------------------------------------------------------------------------- [pc-vr-audit] the pane behind a draw
// SAVAGEWIND_LOG_PANES=1: logs, once for each UI pane and each way the renderer treats its draws (on the HUD panel,
// placed as the crosshair, a fade, a backdrop drawn black, left out or erasing), the pane's name and its screen's:
// "[pc-vr-panes] kind=... screen=... pane=...". The names are nn::ui2d's own (the pane + 0xb8). For replacing the
// renderer's rules about a draw's shape by the pane that drew it. A draw without a marker link carries the pane's
// number in the marker's place, with PC_VR_PANE_LOG_TAG set; markers keep their links.
#define PANE_NAME 0xb8          // char[24]: the pane's name
#define PANE_NAME_LENGTH 24
#define PANE_LOG_ENTRIES 4096   // distinct (screen, pane name) pairs kept
#define PANE_LOG_CACHE 1024     // per thread: the last pane seen at each slot, by its address
typedef struct {
    char name[PANE_NAME_LENGTH + 1];
    uint32_t screen_tag;
    _Atomic uint32_t logged_kinds;  // a bit for each kind already logged
} PaneLogEntry;
static PaneLogEntry pane_log_entries[PANE_LOG_ENTRIES];
static _Atomic unsigned pane_log_count;
static pthread_mutex_t pane_log_lock = PTHREAD_MUTEX_INITIALIZER;

bool pc_vr_pane_log_on(void) { return pc_vr_enabled() && pc_vr_config()->diagnostics.log_panes; }

// The pane's name as the logs print it (a character outside printable ASCII as '?'), into name (PANE_NAME_LENGTH + 1
// bytes). False when the pane is not readable.
static bool pane_log_name_read(uint64_t pane, char *name) {
    if (!mapped(pane, PANE_NAME + PANE_NAME_LENGTH)) return false;
    memcpy(name, (const void *)(uintptr_t)(pane + PANE_NAME), PANE_NAME_LENGTH);
    name[PANE_NAME_LENGTH] = 0;
    for (unsigned i = 0; i < PANE_NAME_LENGTH && name[i]; ++i)
        if (name[i] < 0x20 || name[i] >= 0x7f) name[i] = '?';
    return true;
}

// A screen's whole pane tree, once per screen: "[pc-vr-panes] tree screen=... depth=N pane=..." in drawing order, so a
// pane's group (its parents) is known. nn::ui2d keeps a pane's children in a list whose head is at the pane + 0x20
// (next at + 8), each child linked through its own node at the child + 8 (as upstream's pc_cursor.c walks it).
#define PANE_CHILDREN 0x20
#define PANE_LINK 0x8
#define PANE_TREE_NODES 1500
static void pane_tree_log(uint64_t pane, unsigned depth, const char *screen, unsigned *budget) {
    char name[PANE_NAME_LENGTH + 1];
    if (!*budget || depth > 24 || !pane_log_name_read(pane, name)) return;
    --*budget;
    LOG("[pc-vr-panes] tree screen=%s depth=%u pane=%s at %llx\n", screen, depth, name[0] ? name : "-",
        (unsigned long long)pane);
    const uint64_t head = pane + PANE_CHILDREN;
    uint64_t link = *(const uint64_t *)(uintptr_t)(head + 8);
    while (*budget && link != head && mapped(link, 16)) {
        pane_tree_log(link - PANE_LINK, depth + 1, screen, budget);
        link = *(const uint64_t *)(uintptr_t)(link + 8);
    }
}

// Dumps the tree of the screen the pane being drawn belongs to, the first time that screen is seen.
static void pane_tree_once(uint32_t screen_tag) {
    static _Atomic uint8_t dumped[257];
    if (!screen_tag || screen_tag > 256 || atomic_exchange(&dumped[screen_tag], 1)) return;
    const char *screen = pc_vr_screen_name(screen_tag);
    uint64_t root = pane_drawing;
    for (unsigned i = 0; i < 32 && mapped(root, PANE_SIZE); ++i) {
        const uint64_t parent = *(const uint64_t *)(uintptr_t)(root + PANE_PARENT);
        if (!parent || !mapped(parent, PANE_SIZE)) break;
        root = parent;
    }
    unsigned budget = PANE_TREE_NODES;
    pane_tree_log(root, 0, screen ? screen : "-", &budget);
}

uint32_t pc_vr_pane_log_drawing(uint32_t screen_tag) {
    char name[PANE_NAME_LENGTH + 1];
    if (!pane_drawing || !pane_log_name_read(pane_drawing, name)) return 0;
    pane_tree_once(screen_tag);
    // The same pane as last time at this address: its number without a search.
    static _Thread_local struct { uint64_t pane; uint32_t id; } cache[PANE_LOG_CACHE];
    const unsigned slot = (unsigned)((pane_drawing >> 4) % PANE_LOG_CACHE);
    if (cache[slot].pane == pane_drawing && cache[slot].id) {
        const PaneLogEntry *entry = &pane_log_entries[cache[slot].id - 1];
        if (entry->screen_tag == screen_tag && !strcmp(entry->name, name)) return cache[slot].id;
    }
    uint32_t id = 0;
    pthread_mutex_lock(&pane_log_lock);
    const unsigned count = atomic_load(&pane_log_count);
    for (unsigned i = 0; i < count && !id; ++i)
        if (pane_log_entries[i].screen_tag == screen_tag && !strcmp(pane_log_entries[i].name, name)) id = i + 1;
    if (!id && count < PANE_LOG_ENTRIES) {
        memcpy(pane_log_entries[count].name, name, sizeof name);
        pane_log_entries[count].screen_tag = screen_tag;
        atomic_store(&pane_log_count, count + 1);
        id = count + 1;
    }
    pthread_mutex_unlock(&pane_log_lock);
    cache[slot].pane = pane_drawing;
    cache[slot].id = id;
    return id;
}

const char *pc_vr_pane_log_name(uint32_t id) {
    return id && id <= atomic_load(&pane_log_count) && pane_log_entries[id - 1].name[0] ?
        pane_log_entries[id - 1].name : "-";
}

void pc_vr_pane_log_kind(uint32_t id, unsigned kind, const char *kind_name) {
    if (!id || id > atomic_load(&pane_log_count) || kind >= 32) return;
    PaneLogEntry *entry = &pane_log_entries[id - 1];
    if (atomic_fetch_or(&entry->logged_kinds, 1u << kind) & (1u << kind)) return;
    const char *screen = pc_vr_screen_name(entry->screen_tag);
    LOG("[pc-vr-panes] kind=%s screen=%s pane=%s present=%llu\n", kind_name, screen ? screen : "-",
        entry->name[0] ? entry->name : "-", (unsigned long long)renderer_present_count());
}

void pc_vr_screens_log(void) {
    if (pc_vr_config()->diagnostics.log_screens && pc_vr_enabled()) screens_each(log_change, NULL);
}

// --------------------------------------------------------------------- [pc-vr-marker-link] markers by the game's store
// The link between a draw and its marker, known from where the game stores the helper's result into the piece's pane.
// The stores (1.9.0, all in the world screen's own code; tools/game_sites.py pcvr_marker_*, tools/patches.py
// _marker_*), by the site number each passes:
//   1 to 8   eight of the helper's callers copy its result into the pane themselves, right after the call;
//   9        one hands it to a component's set-position function, which writes the pane at [[component + 8] + 0x18]
//            when both pointers are set;
//   10, 11   a second wheel-like piece follows Link at the helper's result (10), or is given a fixed HUD place (11);
//   12       a speech balloon's store of its pane's x (rt_balloon_point).
// A store binds the pane to the report the helper made last on that thread, when the store reads the very buffer the
// helper wrote: that is the report's provenance, and no position is compared to decide it. A store that cannot bind
// (no report, a report dropped by report(), another buffer) leaves the pane's world point unknown: the game moved the
// piece, but where in the world is not known (a refusal, counted and logged). Store 11 makes the pane flat: the game's
// own fixed HUD place. The table keeps each pane's last state with a copy of its report (and of the report before
// it), so a binding depends neither on the ring nor on any window of presents: the game draws a piece where it last
// put it, and the renderer gets that report however old (pc_vr_hud_point_by_seq). The bindings belong to the world
// screens' objects: when one of those screens comes with a new object, the whole table is cleared
// (marker_link_screen_seen); a pane does not outlive its screen.
//
// Written on the game thread, read on the thread that records the UI's draws: the nearest pane up a draw's parents
// that has an entry answers (marker_link_answer). Bound: its report's number. Flat: PC_VR_MARKER_FLAT. Unknown, or no
// entry: 0, no marker (exec.c leaves a world screen's such draws out, and logs them).
#define MARKER_LINK_SLOTS 2048           // the table's slots: open addressing by the pane's address
#define MARKER_LINK_PANES 1536           // the most panes it keeps (beyond: "table full")
#define MARKER_LINK_LOG_LINES 8          // the lines refusals and "table full" may log
#define MARKER_LINK_NAMED_PANES 64       // the panes whose first binding is logged with the pane's name
#define MARKER_LINK_CLEAR_LINES 16       // the table's clears (a world screen's new object) logged
#define MARKER_LINK_TOTALS_PRESENTS 600  // the presents between two totals lines (logged by a draw)
#define COMPONENT_PANE_HOLDER 0x08       // a HUD component: guest pointer to the object that holds its pane
#define HOLDER_PANE 0x18                 // that object: guest pointer to the pane

typedef enum {
    MARKER_PANE_FREE,     // (a free slot)
    MARKER_PANE_BOUND,    // bound to a report: drawn at its world point
    MARKER_PANE_FLAT,     // at the game's own fixed HUD place: drawn flat
    MARKER_PANE_UNKNOWN,  // moved by a store that could not bind: no world point known, no marker
} MarkerPaneState;
typedef struct {
    uint64_t pane;          // the pane's guest address (0: a free slot)
    PcVrHudPoint report;    // a copy of the report it is bound to (not bound: the last it was bound to)
    PcVrHudPoint previous;  // the report it was bound to before that (seq 0: none), for draws recorded meanwhile
    uint8_t state;          // MarkerPaneState
    bool named;             // its first binding has been logged
} MarkerLink;

typedef struct {
    uint64_t bound, released, unknown;  // the stores kept: bound, released to a fixed HUD place (flat), unknown
    uint64_t no_pane;                   // the stores into no readable pane (a component without one)
    uint64_t full;                      // the stores not kept: the table was full
    uint64_t draws_bound, draws_flat;   // the draws answered with a report, and flat
    uint64_t clears;                    // the table cleared: a world screen's new object
} MarkerLinkCounts;

static pthread_mutex_t marker_link_lock = PTHREAD_MUTEX_INITIALIZER;
static MarkerLink marker_links[MARKER_LINK_SLOTS];
static unsigned marker_link_panes;           // the slots in use
static uint16_t marker_link_used[MARKER_LINK_PANES];  // which slots, in the order taken (for a search by report)
static MarkerLinkCounts marker_link_counts;
static uint64_t marker_link_totals_present;  // the present of the last totals line

// The pane's slot, under marker_link_lock: its own, or (add) a free one taken for it. NULL when it has none, or when
// the table is full. (Slots are freed only all at once, by a clear or a reboot, so a free slot ends every search.)
static MarkerLink *marker_link_slot(uint64_t pane, bool add) {
    unsigned slot = (unsigned)(((pane >> 3) * 0x9e3779b97f4a7c15ull) >> 32) % MARKER_LINK_SLOTS;
    for (unsigned probe = 0; probe < MARKER_LINK_SLOTS; ++probe) {
        MarkerLink *link = &marker_links[slot];
        if (link->pane == pane) return link;
        if (!link->pane) {
            if (!add || marker_link_panes >= MARKER_LINK_PANES) return NULL;
            marker_link_used[marker_link_panes++] = (uint16_t)slot;
            link->pane = pane;
            return link;
        }
        slot = (slot + 1) % MARKER_LINK_SLOTS;
    }
    return NULL;
}

// The store at site into the pane gives it a state: bound to the report point, flat or unknown. On the game thread.
static void marker_link_set(uint64_t pane, const PcVrHudPoint *point, MarkerPaneState state, uint32_t site) {
    static unsigned full_logged, named_logged;  // (under the lock)
    const uint64_t now = renderer_present_count();
    const bool readable = pane && mapped(pane, PANE_SIZE);
    bool log_full = false, log_name = false;
    pthread_mutex_lock(&marker_link_lock);
    MarkerLink *link = readable ? marker_link_slot(pane, true) : NULL;
    if (!readable) {
        ++marker_link_counts.no_pane;
    } else if (!link) {
        ++marker_link_counts.full;
        if (full_logged < MARKER_LINK_LOG_LINES) {
            ++full_logged;
            log_full = true;
        }
    } else {
        if (state == MARKER_PANE_BOUND && link->report.seq != point->seq) {
            if (link->report.seq) link->previous = link->report;
            link->report = *point;
        }
        link->state = (uint8_t)state;
        if (state == MARKER_PANE_BOUND) ++marker_link_counts.bound;
        else if (state == MARKER_PANE_FLAT) ++marker_link_counts.released;
        else ++marker_link_counts.unknown;
        // (the first binding of each pane, with its name: only with the pane log on, SAVAGEWIND_LOG_PANES)
        if (state == MARKER_PANE_BOUND && !link->named && pc_vr_pane_log_on()) {
            link->named = true;
            if (named_logged < MARKER_LINK_NAMED_PANES) {
                ++named_logged;
                log_name = true;
            }
        }
    }
    const unsigned panes_kept = marker_link_panes;
    pthread_mutex_unlock(&marker_link_lock);
    if (log_full)
        LOG("[pc-vr-marker-link] table full (%u panes): site %u's store into pane %llx not kept present=%llu\n",
            panes_kept, site, (unsigned long long)pane, (unsigned long long)now);
    char name[PANE_NAME_LENGTH + 1];
    if (log_name && pane_log_name_read(pane, name))
        LOG("[pc-vr-marker-link] site %u binds pane %s at %llx (its first binding: report %u) present=%llu\n", site,
            name[0] ? name : "-", (unsigned long long)pane, point->seq, (unsigned long long)now);
}

// The store at site into the pane: binds it to the report point, or (point NULL) gives it the game's fixed HUD place.
static void marker_link_store(uint64_t pane, const PcVrHudPoint *point, uint32_t site) {
    marker_link_set(pane, point, point ? MARKER_PANE_BOUND : MARKER_PANE_FLAT, site);
}

// A store that should have bound its pane and cannot (why): the game moved the piece, its world point is unknown. The
// refusal is counted (as an unknown store) and logged.
static void marker_link_refuse(uint64_t pane, uint64_t source, uint32_t site, const char *why) {
    static unsigned logged;  // (under the lock)
    bool log_refusal = false;
    pthread_mutex_lock(&marker_link_lock);
    if (logged < MARKER_LINK_LOG_LINES) {
        ++logged;
        log_refusal = true;
    }
    pthread_mutex_unlock(&marker_link_lock);
    marker_link_set(pane, NULL, MARKER_PANE_UNKNOWN, site);
    if (log_refusal)
        LOG("[pc-vr-marker-link] refused: site %u's store into pane %llx (reading %llx) binds nothing: %s; the "
            "pane's world point is unknown present=%llu\n", site, (unsigned long long)pane, (unsigned long long)source,
            why, (unsigned long long)renderer_present_count());
}

// A store at site into the pane of the two floats at source (a guest address): binds the pane to the helper's last
// report on this thread when the helper wrote that report to source. The report is used up either way.
static void marker_link_from_helper(uint64_t pane, uint64_t source, uint32_t site) {
    const bool this_boot = helper_report.boot == atomic_load_explicit(&marker_link_boot, memory_order_relaxed);
    const HelperReportState state = this_boot ? (HelperReportState)helper_report.state : HELPER_REPORT_NONE;
    if (state == HELPER_REPORT_READY) helper_report.state = HELPER_REPORT_USED;
    if (state == HELPER_REPORT_READY && helper_report.output == source) {
        marker_link_store(pane, &helper_report.point, site);
        return;
    }
    char why[128];
    if (state == HELPER_REPORT_READY)
        snprintf(why, sizeof why, "the helper wrote its last report (%u) to %llx", helper_report.point.seq,
                 (unsigned long long)helper_report.output);
    else if (state == HELPER_REPORT_USED)
        snprintf(why, sizeof why, "the helper's last report (%u) was stored already", helper_report.point.seq);
    else snprintf(why, sizeof why, "no report from the helper on this thread (none made, or dropped)");
    marker_link_refuse(pane, source, site, why);
}

void rt_marker_pane_bind(uint64_t pane, uint64_t source, uint32_t site) {
    if (pc_vr_enabled()) marker_link_from_helper(pane, source, site);
}

// The pane a HUD component's set-position function (1.9.0 0x7000d727f4) writes: [[component + 8] + 0x18]. 0 when
// either pointer is null (the game's function then writes nothing) or not readable.
static uint64_t marker_component_pane(uint64_t component) {
    if (!mapped(component, COMPONENT_PANE_HOLDER + 8)) return 0;
    const uint64_t holder = *(const uint64_t *)(uintptr_t)(component + COMPONENT_PANE_HOLDER);
    if (!holder || !mapped(holder, HOLDER_PANE + 8)) return 0;
    return *(const uint64_t *)(uintptr_t)(holder + HOLDER_PANE);
}

void rt_marker_component_bind(uint64_t component, uint64_t source, uint32_t site) {
    if (!pc_vr_enabled()) return;
    const uint64_t pane = marker_component_pane(component);
    if (pane) {
        marker_link_from_helper(pane, source, site);
        return;
    }
    // (no pane: the game's function writes nothing and nothing is bound; the helper's report is used up all the same)
    if (helper_report.state == HELPER_REPORT_READY) helper_report.state = HELPER_REPORT_USED;
    marker_link_store(0, NULL, site);  // (counted as a store into no pane)
}

void rt_marker_component_release(uint64_t component, uint32_t site) {
    if (pc_vr_enabled()) marker_link_store(marker_component_pane(component), NULL, site);
}

// The report numbered seq among the bindings' copies (pc_vr_hud_point_by_seq, when the ring no longer has it).
static bool marker_link_report_by_seq(uint32_t seq, PcVrHudPoint *out) {
    bool found = false;
    pthread_mutex_lock(&marker_link_lock);
    for (unsigned index = 0; index < marker_link_panes && !found; ++index) {
        const MarkerLink *link = &marker_links[marker_link_used[index]];
        const PcVrHudPoint *copy = link->report.seq == seq ? &link->report :
            link->previous.seq == seq ? &link->previous : NULL;
        if (copy) {
            *out = *copy;
            found = true;
        }
    }
    pthread_mutex_unlock(&marker_link_lock);
    return found;
}

// Lifetime: the bound panes belong to the world screens' objects (all the stores are in their code). The screen loop
// calls this for each screen it draws (rt_screen_draw_begin, on the drawing thread): when a world screen's id comes
// with another object than before, the screens were made anew and every binding is cleared (the counts stay). A
// screen's layout name is read once per object.
static struct {
    uint64_t object;  // the screen object last seen with this id (a guest address; reset per boot)
    bool world;       // whether it is a world screen's
} marker_link_screens[256];  // (by id; the drawing thread's)

static void marker_link_screen_seen(uint32_t id, uint64_t screen) {
    static unsigned clears_logged;
    if (marker_link_screens[id].object == screen) return;
    const char *name = screen ? screen_name(screen) : NULL;
    const bool world = name && (!strcmp(name, "MainScreen3D_00") || !strcmp(name, "Message3D_00"));
    const uint64_t before = marker_link_screens[id].object;
    const bool replaced = world && before && marker_link_screens[id].world;
    marker_link_screens[id].object = screen;
    marker_link_screens[id].world = world;
    if (!replaced) return;
    pthread_mutex_lock(&marker_link_lock);
    const unsigned panes_cleared = marker_link_panes;
    memset(marker_links, 0, sizeof marker_links);
    marker_link_panes = 0;
    ++marker_link_counts.clears;
    pthread_mutex_unlock(&marker_link_lock);
    if (clears_logged < MARKER_LINK_CLEAR_LINES) {
        ++clears_logged;
        LOG("[pc-vr-marker-link] cleared: the world screen %s (id %#x) is a new object (%llx, was %llx); %u panes' "
            "bindings dropped present=%llu\n", name, id, (unsigned long long)screen, (unsigned long long)before,
            panes_cleared, (unsigned long long)renderer_present_count());
    }
}

// For one draw (pc_vr_marker_drawing), its pane and parents (panes, nearest first): the state of the nearest that has a
// table entry. Bound: its report's number. Flat: PC_VR_MARKER_FLAT. Unknown, or no entry: 0. Counts the draws answered
// with a report or flat, and logs the totals every MARKER_LINK_TOTALS_PRESENTS presents.
static uint32_t marker_link_answer(const uint64_t *panes, unsigned depth) {
    const uint64_t now = renderer_present_count();
    uint32_t answer = 0;
    pthread_mutex_lock(&marker_link_lock);
    const MarkerLink *found = NULL;
    for (unsigned level = 0; level < depth && marker_link_panes && !found; ++level)  // (an empty table: no lookups)
        found = marker_link_slot(panes[level], false);
    if (found && found->state == MARKER_PANE_BOUND) {
        answer = found->report.seq;
        ++marker_link_counts.draws_bound;
    } else if (found && found->state == MARKER_PANE_FLAT) {
        answer = PC_VR_MARKER_FLAT;
        ++marker_link_counts.draws_flat;
    }
    const bool log_totals = now >= marker_link_totals_present + MARKER_LINK_TOTALS_PRESENTS;
    MarkerLinkCounts counts = {0};
    unsigned panes_kept = 0;
    if (log_totals) {
        marker_link_totals_present = now;
        counts = marker_link_counts;
        panes_kept = marker_link_panes;
    }
    pthread_mutex_unlock(&marker_link_lock);
    if (log_totals)
        LOG("[pc-vr-marker-link] totals present=%llu: stores bound=%llu released=%llu unknown=%llu no_pane=%llu "
            "full=%llu; draws bound=%llu flat=%llu; panes=%u clears=%llu\n", (unsigned long long)now,
            (unsigned long long)counts.bound, (unsigned long long)counts.released, (unsigned long long)counts.unknown,
            (unsigned long long)counts.no_pane, (unsigned long long)counts.full,
            (unsigned long long)counts.draws_bound, (unsigned long long)counts.draws_flat, panes_kept,
            (unsigned long long)counts.clears);
    return answer;
}

RT_ON_RESET(pc_vr_marker_link_reset) {
    pthread_mutex_lock(&marker_link_lock);
    memset(marker_links, 0, sizeof marker_links);
    marker_link_panes = 0;
    memset(&marker_link_counts, 0, sizeof marker_link_counts);
    marker_link_totals_present = 0;
    pthread_mutex_unlock(&marker_link_lock);
    memset(marker_link_screens, 0, sizeof marker_link_screens);  // (guest addresses: the next boot's are its own)
    atomic_fetch_add(&marker_link_boot, 1);  // (each thread's last helper report is from before the reboot: none)
}
