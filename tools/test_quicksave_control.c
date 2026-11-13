#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../runtime/hle/quicksave_control.h"

#define SCREEN(id) (1ull << (id))

static QuicksaveView game(void) {
    return (QuicksaveView){.screens = SCREEN(2), .pause = 0, .fresh = true,
                           .world = true, .high_screens = true};
}
static QuicksaveView pause_menu(void) {
    return (QuicksaveView){.screens = SCREEN(2) | SCREEN(0x2f), .pause = 2,
                           .fresh = true, .world = true, .high_screens = true};
}
static QuicksaveView dialog(void) {
    QuicksaveView v = pause_menu();
    v.screens |= SCREEN(0x38);
    return v;
}

int main(void) {
    QuicksaveControl control = {0};
    assert(quicksave_advance(&control, game(), 0, 1000000000ull) == QUICKSAVE_PLUS);
    assert(quicksave_advance(&control, pause_menu(), 59, 2000000000ull) == QUICKSAVE_NONE);
    assert(quicksave_advance(&control, pause_menu(), 60, 3000000000ull) == QUICKSAVE_R);
    assert(quicksave_advance(&control, pause_menu(), 120, 4000000000ull) == QUICKSAVE_A);
    assert(quicksave_advance(&control, dialog(), 180, 5000000000ull) == QUICKSAVE_UP);
    assert(quicksave_advance(&control, dialog(), 240, 6000000000ull) == QUICKSAVE_A);
    // A Load confirmation shares screen 0x38: it must get no second A.
    assert(quicksave_advance(&control, dialog(), 300, 7000000000ull) == QUICKSAVE_NONE);
    assert(quicksave_advance(&control, dialog(), 360, 16000000000ull) == QUICKSAVE_ABORT);
    assert(quicksave_advance(&control, dialog(), 420, 17000000000ull) == QUICKSAVE_NONE);

    control = (QuicksaveControl){0};
    assert(quicksave_advance(&control, game(), 0, 1000000000ull) == QUICKSAVE_PLUS);
    assert(quicksave_advance(&control, pause_menu(), 60, 2000000000ull) == QUICKSAVE_R);
    assert(quicksave_advance(&control, pause_menu(), 120, 3000000000ull) == QUICKSAVE_A);
    assert(quicksave_advance(&control, dialog(), 180, 4000000000ull) == QUICKSAVE_UP);
    assert(quicksave_advance(&control, dialog(), 240, 5000000000ull) == QUICKSAVE_A);
    assert(quicksave_advance(&control, pause_menu(), 300, 6000000000ull) == QUICKSAVE_B);
    assert(quicksave_advance(&control, game(), 360, 7000000000ull) == QUICKSAVE_DONE);

    control = (QuicksaveControl){0};
    QuicksaveView title = game(); title.title = true;
    assert(quicksave_advance(&control, title, 0, 1000000000ull) == QUICKSAVE_ABORT);
    QuicksaveView movie = game(); movie.movie = true;
    assert(quicksave_advance(&control, movie, 0, 1000000000ull) == QUICKSAVE_ABORT);
    QuicksaveView stale = game(); stale.fresh = false;
    assert(quicksave_advance(&control, stale, 0, 1000000000ull) == QUICKSAVE_ABORT);
    QuicksaveView event = game(); event.event = true;
    assert(quicksave_advance(&control, event, 0, 1000000000ull) == QUICKSAVE_ABORT);
    QuicksaveView wrong_menu = dialog(); wrong_menu.screens |= SCREEN(0x39);
    assert(quicksave_advance(&control, wrong_menu, 0, 1000000000ull) == QUICKSAVE_ABORT);

    control = (QuicksaveControl){0};
    assert(quicksave_advance(&control, game(), 0, 1000000000ull) == QUICKSAVE_PLUS);
    assert(quicksave_advance(&control, dialog(), 60, 2000000000ull) == QUICKSAVE_ABORT);
    control = (QuicksaveControl){0};
    assert(quicksave_advance(&control, game(), 0, 1000000000ull) == QUICKSAVE_PLUS);
    quicksave_control_cancel(&control);
    assert(quicksave_advance(&control, pause_menu(), 60, 2000000000ull) == QUICKSAVE_NONE);
    puts("quicksave control tests passed");
    return 0;
}
