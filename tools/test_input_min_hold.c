#include <assert.h>
#include <stdio.h>

#include "../runtime/hle/input_min_hold.h"

int main(void) {
    InputMinHoldKey key = {0};
    assert(input_min_hold_parse_ms(NULL) == 0);
    assert(input_min_hold_parse_ms("0") == 0);
    assert(input_min_hold_parse_ms("off") == 0);
    assert(input_min_hold_parse_ms("1") == 1);
    assert(input_min_hold_parse_ms("80") == 80);
    assert(input_min_hold_parse_ms("100") == 100);
    const char *invalid[] = {"", "00", "01", "-1", "+80", " 80", "101", "99999999999999999999", "80ms", "OFF"};
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
        assert(input_min_hold_parse_ms(invalid[i]) == 0);

    // Disabled/default behavior: release immediately, even when both edges precede a sample.
    input_min_hold_down(&key, 1000000, 0, false);
    input_min_hold_up(&key, 1000001);
    assert(!key.effective);

    // A 0.2 ms tap remains visible until an 80 ms deadline, then expires.
    input_min_hold_down(&key, 2000000, 80000000, false);
    input_min_hold_up(&key, 2200000);
    assert(!key.physical && key.effective);
    assert(!input_min_hold_expire(&key, 81999999) && key.effective);
    assert(input_min_hold_expire(&key, 82000000) && !key.effective);

    // Repeated down and unmatched up cannot arm or extend a hold.
    input_min_hold_up(&key, 90000000);
    assert(!key.effective);
    input_min_hold_down(&key, 90000000, 80000000, true);
    assert(!key.effective);
    input_min_hold_down(&key, 100000000, 80000000, false);
    input_min_hold_down(&key, 150000000, 80000000, true);
    assert(key.until_ns == 180000000);
    input_min_hold_up(&key, 180000001);
    assert(!key.effective);

    // A new physical down while the prior short press is pending starts a new deadline.
    input_min_hold_down(&key, 185000000, 80000000, false);
    input_min_hold_up(&key, 185200000);
    input_min_hold_down(&key, 190000000, 80000000, false);
    assert(key.physical && key.effective && key.until_ns == 270000000);
    input_min_hold_up(&key, 190200000);
    assert(!input_min_hold_expire(&key, 265000000));
    assert(input_min_hold_expire(&key, 270000000));

    // Physical hold outlives the deadline, but reset cancels every pending edge.
    input_min_hold_down(&key, 200000000, 80000000, false);
    assert(!input_min_hold_expire(&key, 300000000) && key.effective);
    input_min_hold_up(&key, 300000000);
    assert(!key.effective);
    input_min_hold_down(&key, 400000000, 80000000, false);
    input_min_hold_up(&key, 400200000);
    input_min_hold_cancel(&key);
    assert(!key.physical && !key.effective && !key.until_ns);
    assert(!input_min_hold_expire(&key, 500000000));
    input_min_hold_down(&key, 510000000, 80000000, true);
    assert(!key.effective);  // a repeat after reset cannot rearm a cancelled press

    // Two physical keys retain independent deadlines and cancellation.
    InputMinHoldKey second = {0};
    input_min_hold_down(&key, 600000000, 80000000, false);
    input_min_hold_down(&second, 610000000, 80000000, false);
    input_min_hold_up(&key, 600200000);
    input_min_hold_up(&second, 610200000);
    input_min_hold_cancel(&key);
    assert(!key.effective && second.effective);
    assert(input_min_hold_expire(&second, 690000000));

    puts("input minimum hold: passed");
    return 0;
}
