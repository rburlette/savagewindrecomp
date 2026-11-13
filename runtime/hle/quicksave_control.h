// Pure, bounded controller for the opt-in F5 pause-menu save sequence.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum QuicksaveAction {
    QUICKSAVE_NONE, QUICKSAVE_PLUS, QUICKSAVE_R, QUICKSAVE_A, QUICKSAVE_UP, QUICKSAVE_B, QUICKSAVE_DONE, QUICKSAVE_ABORT
} QuicksaveAction;

typedef enum QuicksaveStage {
    QUICKSAVE_IDLE, QUICKSAVE_WAIT_PAUSE, QUICKSAVE_WAIT_SYSTEM, QUICKSAVE_WAIT_DIALOG, QUICKSAVE_WAIT_SELECTION,
    QUICKSAVE_WAIT_CLOSE, QUICKSAVE_WAIT_GAMEPLAY, QUICKSAVE_FINISHED
} QuicksaveStage;

typedef struct QuicksaveControl {
    QuicksaveStage stage;
    uint64_t started_ns, next_frame;
} QuicksaveControl;

typedef struct QuicksaveView {
    uint64_t screens;
    uint32_t pause;
    bool fresh, world, title, event, movie, high_screens;
} QuicksaveView;

static inline void quicksave_control_cancel(QuicksaveControl *control) { control->stage = QUICKSAVE_FINISHED; }

static inline bool quicksave_screen_shown(QuicksaveView view, unsigned id) { return (view.screens >> id) & 1u; }
static inline bool quicksave_safe(QuicksaveView view) {
    // High screen IDs are present in observed ordinary gameplay and pause menus;
    // they are metadata, not a standalone unsafe-state signal.
    return view.fresh && view.world && !view.title && !view.event && !view.movie &&
           !quicksave_screen_shown(view, 0x34) && !quicksave_screen_shown(view, 0x39);
}
static inline bool quicksave_in_gameplay(QuicksaveView view) {
    return quicksave_safe(view) && view.pause == 0 && quicksave_screen_shown(view, 2) &&
           !quicksave_screen_shown(view, 0x2f) && !quicksave_screen_shown(view, 0x38);
}
static inline bool quicksave_in_pause_menu(QuicksaveView view) {
    return quicksave_safe(view) && view.pause == 2 && quicksave_screen_shown(view, 0x2f) &&
        !quicksave_screen_shown(view, 0x38);
}
static inline bool quicksave_in_save_dialog(QuicksaveView view) {
    return quicksave_safe(view) && view.pause == 2 && quicksave_screen_shown(view, 0x2f) &&
        quicksave_screen_shown(view, 0x38);
}

// Call once per presented frame. Every navigation action is separated by >=60 presents.
// Only one A is ever sent while the 0x38 prompt is open; if it was Load, no second
// confirmation can be sent by this controller.
static inline QuicksaveAction quicksave_advance(QuicksaveControl *control, QuicksaveView view,
                                         uint64_t frame, uint64_t now_ns) {
    if (control->stage == QUICKSAVE_IDLE) {
        if (!quicksave_in_gameplay(view)) return QUICKSAVE_ABORT;
        control->started_ns = now_ns;
        control->next_frame = frame + 60;
        control->stage = QUICKSAVE_WAIT_PAUSE;
        return QUICKSAVE_PLUS;
    }
    if (control->stage == QUICKSAVE_FINISHED) return QUICKSAVE_NONE;
    if (now_ns < control->started_ns || now_ns - control->started_ns >= 15000000000ull || !quicksave_safe(view)) {
        quicksave_control_cancel(control);
        return QUICKSAVE_ABORT;
    }
    if ((control->stage == QUICKSAVE_WAIT_PAUSE || control->stage == QUICKSAVE_WAIT_SYSTEM) &&
        quicksave_screen_shown(view, 0x38)) {
        quicksave_control_cancel(control);
        return QUICKSAVE_ABORT;
    }
    if (control->stage == QUICKSAVE_WAIT_SELECTION && !quicksave_in_save_dialog(view)) {
        quicksave_control_cancel(control);
        return QUICKSAVE_ABORT;
    }
    if (control->stage == QUICKSAVE_WAIT_GAMEPLAY && quicksave_screen_shown(view, 0x38)) {
        quicksave_control_cancel(control);
        return QUICKSAVE_ABORT;
    }
    if (frame < control->next_frame) return QUICKSAVE_NONE;
    switch (control->stage) {
    case QUICKSAVE_WAIT_PAUSE:
        if (!quicksave_in_pause_menu(view)) break;
        control->stage = QUICKSAVE_WAIT_SYSTEM;
        control->next_frame = frame + 60;
        return QUICKSAVE_R;
    case QUICKSAVE_WAIT_SYSTEM:
        if (!quicksave_in_pause_menu(view)) break;
        control->stage = QUICKSAVE_WAIT_DIALOG;
        control->next_frame = frame + 60;
        return QUICKSAVE_A;
    case QUICKSAVE_WAIT_DIALOG:
        if (!quicksave_in_save_dialog(view)) break;
        control->stage = QUICKSAVE_WAIT_SELECTION;
        control->next_frame = frame + 60;
        return QUICKSAVE_UP;
    case QUICKSAVE_WAIT_SELECTION:
        if (!quicksave_in_save_dialog(view)) break;
        control->stage = QUICKSAVE_WAIT_CLOSE;
        control->next_frame = frame + 60;
        return QUICKSAVE_A;
    case QUICKSAVE_WAIT_CLOSE:
        if (quicksave_in_gameplay(view)) {
            control->stage = QUICKSAVE_FINISHED;
            return QUICKSAVE_DONE;
        }
        if (!quicksave_in_pause_menu(view)) break;
        control->stage = QUICKSAVE_WAIT_GAMEPLAY;
        control->next_frame = frame + 60;
        return QUICKSAVE_B;
    case QUICKSAVE_WAIT_GAMEPLAY:
        if (!quicksave_in_gameplay(view)) break;
        quicksave_control_cancel(control);
        return QUICKSAVE_DONE;
    default: break;
    }
    return QUICKSAVE_NONE;  // wait for a transition, bounded by the real-time deadline
}
