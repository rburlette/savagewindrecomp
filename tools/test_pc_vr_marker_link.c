// Asset-free check of PC VR's marker links (runtime/pc_vr_hud.c, [pc-vr-marker-link]): the game's stores of the HUD
// helper's result into a pane bind that pane to the helper's report, by where the helper wrote it and never by
// position, and the binding answers each draw (pc_vr_marker_drawing). Covers a bind, a rebind, the lookup through a
// parent pane, no binding (0), refused stores (no report left, another buffer, a dropped report, a balloon's dropped
// report, a report from before a reboot) leaving the pane's world point unknown (0) until a good store binds it again,
// the game's fixed HUD place (flat), a component's pane, a speech balloon, a bound report older than the ring, the
// table cleared when a world screen comes with a new object, the totals' cadence and counts, the reset, a full table
// and the report numbers' wrap. The guest memory is a block allocated at the guest heap's address; no game, GPU or
// headset is involved. --log keeps the runtime's log lines.
#include "../runtime/pc_vr_hud.c"
#include "test_pc_vr_settings.h"  // (the play configuration's defaults: no pane or screen log)

#include <math.h>
#include <windows.h>

GameSites g_site;
static uint64_t fixture_present = 100;
static bool fixture_pc_vr = true;
uint64_t renderer_present_count(void) { return fixture_present; }
bool pc_vr_enabled(void) { return fixture_pc_vr; }
void rt_fatal_trace(void) { abort(); }

#define FIXTURE_HEAP_BYTES 0x400000ull
uint64_t kernel_heap_size(void) { return FIXTURE_HEAP_BYTES; }

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; if (failures < 10) printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

// Where things are in the fixture's guest memory.
#define CHILD_PANE (HEAP_BASE + 0x1000)       // a pane drawn under PARENT_PANE
#define PARENT_PANE (HEAP_BASE + 0x2000)      // the pane the stores write
#define BALLOON_PANE (HEAP_BASE + 0x3000)
#define COMPONENT (HEAP_BASE + 0x4000)        // a HUD component, its pane at [[+8] + 0x18]
#define PANE_HOLDER (HEAP_BASE + 0x4100)
#define COMPONENT_PANE (HEAP_BASE + 0x5000)
#define EMPTY_COMPONENT (HEAP_BASE + 0x6000)  // a component without a pane holder
#define LONE_PANE (HEAP_BASE + 0x7000)        // a pane no store writes
#define SPARE_PANE (HEAP_BASE + 0x7800)
#define HELPER_OUTPUT (HEAP_BASE + 0x8000)    // the buffer the helper writes (a caller's stack)
#define HELPER_CAMERA (HEAP_BASE + 0x8010)
#define OTHER_BUFFER (HEAP_BASE + 0x8040)
#define BALLOON_STACK (HEAP_BASE + 0x9000)
#define BALLOON_FRAME (HEAP_BASE + 0x9100)
#define WORLD_SCREEN (HEAP_BASE + 0xa000)     // a world screen's object (MainScreen3D_00)
#define NEW_WORLD_SCREEN (HEAP_BASE + 0xa400) // the same screen made anew
#define FLAT_SCREEN (HEAP_BASE + 0xa800)      // another screen's object (MainScreen_00)
#define NEW_FLAT_SCREEN (HEAP_BASE + 0xac00)
#define SCREEN_NAMES (HEAP_BASE + 0xb000)     // their layout names, 0x40 apart
#define MANY_PANES (HEAP_BASE + 0x100000)     // the panes that fill the table, 0x80 apart
#define SCREEN_TAG 1

static Cpu cpu;

static void put_float(uint64_t address, float value) { memcpy((void *)(uintptr_t)address, &value, 4); }
static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    return bits;
}
static void put_pointer(uint64_t address, uint64_t value) { memcpy((void *)(uintptr_t)address, &value, 8); }

// The helper reports a position written to HELPER_OUTPUT; returns the report's number (0: dropped).
static uint32_t helper_reports(float x, float y) {
    const uint32_t before = last_seq;
    put_float(HELPER_OUTPUT, x);
    put_float(HELPER_OUTPUT + 4, y);
    put_float(HELPER_CAMERA, 1);
    put_float(HELPER_CAMERA + 4, 2);
    put_float(HELPER_CAMERA + 8, -30);
    rt_hud_point(&cpu, HELPER_OUTPUT, HELPER_CAMERA);
    return last_seq != before ? last_seq : 0;
}

// The marker a draw under the pane gets (pc_vr_marker_drawing).
static uint32_t draw_under(uint64_t pane) {
    const uint64_t previous = rt_pane_draw_begin(pane);
    const uint32_t marker = pc_vr_marker_drawing(SCREEN_TAG);
    rt_pane_draw_end(previous);
    return marker;
}

// The table's entry for the pane (NULL: none).
static const MarkerLink *entry(uint64_t pane) { return marker_link_slot(pane, false); }

static bool in_state(uint64_t pane, MarkerPaneState state) {
    const MarkerLink *link = entry(pane);
    return link && link->state == state;
}

static bool bound_to(uint64_t pane, uint32_t seq) {
    const MarkerLink *link = entry(pane);
    return link && link->state == MARKER_PANE_BOUND && link->report.seq == seq;
}

static void reboot(void) {
    pc_vr_hud_reset();
    pc_vr_marker_link_reset();
}

int main(int argc, char **argv) {
    if (!(argc > 1 && !strcmp(argv[1], "--log"))) (void)!freopen("NUL", "w", stderr);
    test_pc_vr_configure((const char *const[]){"SAVAGEWIND_PC_VR", "1", NULL});
    void *heap = VirtualAlloc((void *)(uintptr_t)HEAP_BASE, FIXTURE_HEAP_BYTES, MEM_RESERVE | MEM_COMMIT,
                              PAGE_READWRITE);
    if (heap != (void *)(uintptr_t)HEAP_BASE) {
        printf("marker link fixture: the guest heap's address is taken\n");
        return 1;
    }
    put_pointer(CHILD_PANE + PANE_PARENT, PARENT_PANE);
    memcpy((void *)(uintptr_t)(PARENT_PANE + PANE_NAME), "P_Fixture_00", 13);
    put_pointer(COMPONENT + COMPONENT_PANE_HOLDER, PANE_HOLDER);
    put_pointer(PANE_HOLDER + HOLDER_PANE, COMPONENT_PANE);

    // A bind: the store reads the buffer the helper just wrote. The report is used up.
    const uint32_t first = helper_reports(100, 200);
    CHECK(first);
    rt_marker_pane_bind(PARENT_PANE, HELPER_OUTPUT, 3);
    CHECK(bound_to(PARENT_PANE, first));
    CHECK(marker_link_counts.bound == 1 && marker_link_panes == 1);
    CHECK(helper_report.state == HELPER_REPORT_USED);
    // A draw under the bound pane's child: the binding found through the parent answers.
    CHECK(draw_under(CHILD_PANE) == first);
    CHECK(draw_under(PARENT_PANE) == first);
    // A pane no store wrote: 0, no marker.
    CHECK(draw_under(LONE_PANE) == 0);
    CHECK(marker_link_counts.draws_bound == 2 && !marker_link_counts.draws_flat);

    // A second store without a new report binds nothing: refused, the pane's world point unknown, its draws 0 (the
    // renderer leaves a world screen's such draws out), never flat.
    rt_marker_pane_bind(PARENT_PANE, HELPER_OUTPUT, 3);
    CHECK(in_state(PARENT_PANE, MARKER_PANE_UNKNOWN));
    CHECK(marker_link_counts.unknown == 1 && !marker_link_counts.released);
    CHECK(draw_under(CHILD_PANE) == 0);
    // A store that reads another buffer than the helper wrote: refused too.
    CHECK(helper_reports(100, 200));
    rt_marker_pane_bind(PARENT_PANE, OTHER_BUFFER, 4);
    CHECK(in_state(PARENT_PANE, MARKER_PANE_UNKNOWN) && marker_link_counts.unknown == 2);
    CHECK(helper_report.state == HELPER_REPORT_USED);
    CHECK(draw_under(CHILD_PANE) == 0);

    // A good store binds the pane again; a rebind replaces its report.
    const uint32_t rebound = helper_reports(100, 200);
    rt_marker_pane_bind(PARENT_PANE, HELPER_OUTPUT, 5);
    CHECK(bound_to(PARENT_PANE, rebound) && marker_link_panes == 1);
    CHECK(draw_under(CHILD_PANE) == rebound);
    // A newer report at the same position, stored into another pane, does not change this pane's answer.
    CHECK(helper_reports(100, 200));
    rt_marker_pane_bind(SPARE_PANE, HELPER_OUTPUT, 6);
    CHECK(draw_under(CHILD_PANE) == rebound);
    // Presents later, without new reports: no age limits a binding.
    fixture_present = 110;
    CHECK(draw_under(CHILD_PANE) == rebound);

    // A component's pane, bound, then given the game's fixed HUD place: flat.
    const uint32_t wheel = helper_reports(7, 8);
    rt_marker_component_bind(COMPONENT, HELPER_OUTPUT, 9);
    CHECK(bound_to(COMPONENT_PANE, wheel));
    CHECK(draw_under(COMPONENT_PANE) == wheel);
    rt_marker_component_release(COMPONENT, 11);
    CHECK(in_state(COMPONENT_PANE, MARKER_PANE_FLAT) && marker_link_counts.released == 1);
    CHECK(draw_under(COMPONENT_PANE) == PC_VR_MARKER_FLAT);
    CHECK(marker_link_counts.draws_flat == 1);
    // ... and bound again when it follows Link again.
    const uint32_t wheel_again = helper_reports(9, 8);
    rt_marker_component_bind(COMPONENT, HELPER_OUTPUT, 10);
    CHECK(draw_under(COMPONENT_PANE) == wheel_again);
    // A component without a pane: nothing kept, the report used up, so the next store is refused.
    CHECK(helper_reports(7, 8));
    rt_marker_component_bind(EMPTY_COMPONENT, HELPER_OUTPUT, 9);
    CHECK(marker_link_counts.no_pane == 1);
    CHECK(helper_report.state == HELPER_REPORT_USED);
    rt_marker_pane_bind(PARENT_PANE, HELPER_OUTPUT, 1);
    CHECK(in_state(PARENT_PANE, MARKER_PANE_UNKNOWN));

    // A report the ring drops (not a finite position) leaves none to bind: unknown, until a good store.
    CHECK(!helper_reports(NAN, 8));
    CHECK(helper_report.state == HELPER_REPORT_NONE);
    rt_marker_pane_bind(PARENT_PANE, HELPER_OUTPUT, 2);
    CHECK(in_state(PARENT_PANE, MARKER_PANE_UNKNOWN) && draw_under(CHILD_PANE) == 0);
    const uint32_t recovered = helper_reports(100, 201);
    rt_marker_pane_bind(PARENT_PANE, HELPER_OUTPUT, 2);
    CHECK(draw_under(CHILD_PANE) == recovered);

    // A speech balloon binds its own pane (x8) to the report it makes; a balloon whose report is dropped: unknown.
    cpu.x[8] = BALLOON_PANE;
    cpu.sp = BALLOON_STACK;
    cpu.x[29] = BALLOON_FRAME;
    cpu.v[0] = float_bits(660);  // (the pane's x)
    cpu.v[8] = float_bits(300);  // (its y)
    cpu.v[9] = float_bits(640);  // (its x before the horizontal offset)
    put_float(BALLOON_FRAME - 0x18, 640);
    put_float(BALLOON_STACK + 0x1c, 0);
    put_float(BALLOON_STACK + 0x20, 1);
    put_float(BALLOON_STACK + 0x24, -40);
    rt_balloon_point(&cpu);
    CHECK(bound_to(BALLOON_PANE, last_seq));
    CHECK(entry(BALLOON_PANE) && fabsf(entry(BALLOON_PANE)->report.offset - 20.0f / 640) < 1e-6f);
    CHECK(draw_under(BALLOON_PANE) == last_seq);
    cpu.v[8] = 0x7fc00000;  // (NaN: the report is dropped)
    rt_balloon_point(&cpu);
    CHECK(in_state(BALLOON_PANE, MARKER_PANE_UNKNOWN) && draw_under(BALLOON_PANE) == 0);
    cpu.v[0] = cpu.v[8] = cpu.v[9] = 0;

    // Outside PC VR nothing is bound, released or counted.
    fixture_pc_vr = false;
    const MarkerLinkCounts before_off = marker_link_counts;
    rt_marker_pane_bind(PARENT_PANE, HELPER_OUTPUT, 1);
    rt_marker_component_release(COMPONENT, 11);
    CHECK(!memcmp(&marker_link_counts, &before_off, sizeof before_off) && draw_under(CHILD_PANE) == 0);
    fixture_pc_vr = true;

    // A bound report older than the ring (many newer reports since): the binding still answers with it, and the
    // renderer still gets it (pc_vr_hud_point_by_seq). After a rebind, the report before it too (a draw recorded
    // before the rebind). A report no pane keeps is gone with the ring.
    const uint32_t unkept = helper_reports(300, 400);
    const uint32_t old_report = helper_reports(50, 60);
    rt_marker_pane_bind(PARENT_PANE, HELPER_OUTPUT, 8);
    for (unsigned index = 0; index < 2 * HUD_POINTS; ++index) helper_reports(1000.0f + (float)index, 1);
    PcVrHudPoint copy = {0};
    CHECK(draw_under(CHILD_PANE) == old_report);
    CHECK(pc_vr_hud_point_by_seq(old_report, &copy) && copy.seq == old_report && copy.layout[0] == 50 &&
          copy.layout[1] == 60 && copy.camera[2] == -30);
    const uint32_t next_report = helper_reports(51, 61);
    rt_marker_pane_bind(PARENT_PANE, HELPER_OUTPUT, 8);
    for (unsigned index = 0; index < 2 * HUD_POINTS; ++index) helper_reports(2000.0f + (float)index, 1);
    CHECK(pc_vr_hud_point_by_seq(next_report, &copy) && copy.layout[0] == 51);
    CHECK(pc_vr_hud_point_by_seq(old_report, &copy) && copy.layout[0] == 50);
    CHECK(unkept && !pc_vr_hud_point_by_seq(unkept, &copy));
    CHECK(!pc_vr_hud_point_by_seq(0, &copy) && !pc_vr_hud_point_by_seq(PC_VR_MARKER_FLAT, &copy));

    // The table is cleared when a world screen comes with a new object, and only then: not for the same object again,
    // not when another screen does.
    memcpy((void *)(uintptr_t)SCREEN_NAMES, "MainScreen3D_00", 16);
    memcpy((void *)(uintptr_t)(SCREEN_NAMES + 0x40), "MainScreen_00", 14);
    put_pointer(WORLD_SCREEN + SCREEN_LAYOUT_NAME, SCREEN_NAMES);
    put_pointer(NEW_WORLD_SCREEN + SCREEN_LAYOUT_NAME, SCREEN_NAMES);
    put_pointer(FLAT_SCREEN + SCREEN_LAYOUT_NAME, SCREEN_NAMES + 0x40);
    put_pointer(NEW_FLAT_SCREEN + SCREEN_LAYOUT_NAME, SCREEN_NAMES + 0x40);
    rt_screen_draw_end(rt_screen_draw_begin(WORLD_SCREEN, 7));
    rt_screen_draw_end(rt_screen_draw_begin(FLAT_SCREEN, 8));
    rt_screen_draw_end(rt_screen_draw_begin(WORLD_SCREEN, 7));
    rt_screen_draw_end(rt_screen_draw_begin(NEW_FLAT_SCREEN, 8));
    CHECK(marker_link_panes && !marker_link_counts.clears && entry(PARENT_PANE));
    rt_screen_draw_end(rt_screen_draw_begin(NEW_WORLD_SCREEN, 7));
    CHECK(!marker_link_panes && marker_link_counts.clears == 1 && !entry(PARENT_PANE));
    CHECK(draw_under(CHILD_PANE) == 0 && !pc_vr_screen_drawing());
    const uint32_t after_clear = helper_reports(100, 200);
    rt_marker_pane_bind(PARENT_PANE, HELPER_OUTPUT, 5);
    CHECK(draw_under(CHILD_PANE) == after_clear);

    // The totals line: logged by a draw, at most once every MARKER_LINK_TOTALS_PRESENTS presents.
    fixture_present = MARKER_LINK_TOTALS_PRESENTS + 5;
    draw_under(LONE_PANE);
    CHECK(marker_link_totals_present == MARKER_LINK_TOTALS_PRESENTS + 5);
    fixture_present += MARKER_LINK_TOTALS_PRESENTS - 1;
    draw_under(LONE_PANE);
    CHECK(marker_link_totals_present == MARKER_LINK_TOTALS_PRESENTS + 5);

    // A reboot empties the table, and a report from before it binds nothing after it.
    CHECK(helper_reports(1, 2));
    reboot();
    CHECK(!marker_link_panes && !entry(PARENT_PANE) && !marker_link_counts.bound);
    CHECK(draw_under(CHILD_PANE) == 0);
    rt_marker_pane_bind(PARENT_PANE, HELPER_OUTPUT, 1);
    CHECK(marker_link_counts.unknown == 1 && in_state(PARENT_PANE, MARKER_PANE_UNKNOWN));

    // A full table keeps what it has: another pane is not kept, a pane it has is still rebound.
    reboot();
    unsigned bound_panes = 0;
    for (unsigned index = 0; index < MARKER_LINK_PANES; ++index) {
        helper_reports((float)index, 1);
        rt_marker_pane_bind(MANY_PANES + 0x80ull * index, HELPER_OUTPUT, 7);
        bound_panes += entry(MANY_PANES + 0x80ull * index) != NULL;
    }
    CHECK(bound_panes == MARKER_LINK_PANES && marker_link_panes == MARKER_LINK_PANES && !marker_link_counts.full);
    helper_reports(-1, 1);
    rt_marker_pane_bind(MANY_PANES + 0x80ull * MARKER_LINK_PANES, HELPER_OUTPUT, 7);
    CHECK(marker_link_counts.full == 1 && !entry(MANY_PANES + 0x80ull * MARKER_LINK_PANES));
    const uint32_t again = helper_reports(5, 5);
    rt_marker_pane_bind(MANY_PANES, HELPER_OUTPUT, 8);
    CHECK(bound_to(MANY_PANES, again));
    for (unsigned index = 0; index < MARKER_LINK_PANES; index += 97)
        CHECK(entry(MANY_PANES + 0x80ull * index) && entry(MANY_PANES + 0x80ull * index)->pane ==
              MANY_PANES + 0x80ull * index);

    // The report numbers run from 1 to PC_VR_MARKER_SEQ_MAX and start again at 1: never 0 or the flat answer.
    last_seq = PC_VR_MARKER_SEQ_MAX - 1;
    CHECK(helper_reports(1, 1) == PC_VR_MARKER_SEQ_MAX);
    CHECK(helper_reports(1, 1) == 1);

    printf("marker link fixture: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
