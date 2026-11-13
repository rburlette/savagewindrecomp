#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../runtime/hle/input_stick.h"

static void filtered(int32_t raw_x, int32_t raw_y, int32_t *x, int32_t *y) {
    *x = raw_x;
    *y = raw_y;
    input_stick_apply_deadzone(x, y);
    assert(*x >= -32767 && *x <= 32767);
    assert(*y >= -32767 && *y <= 32767);
}

int main(void) {
    int32_t x, y;

    // Measured at-rest Link pad axes must not create walking or camera motion.
    filtered(2182, 0, &x, &y); assert(x == 0 && y == 0);
    filtered(0, 1532, &x, &y); assert(x == 0 && y == 0);
    filtered(2182, 1532, &x, &y); assert(x == 0 && y == 0);

    filtered(8000, 0, &x, &y); assert(x == 0 && y == 0);
    filtered(8001, 0, &x, &y); assert(x > 0 && x <= 2 && y == 0);
    filtered(5656, 5656, &x, &y); assert(x == 0 && y == 0);
    filtered(5657, 5657, &x, &y); assert(x >= 0 && x <= 2 && x == y);

    filtered(32767, 0, &x, &y); assert(x == 32767 && y == 0);
    filtered(-32768, 0, &x, &y); assert(x == -32767 && y == 0);
    filtered(0, -32768, &x, &y); assert(x == 0 && y == -32767);
    filtered(32767, 32767, &x, &y); assert(x == 32767 && y == 32767);
    filtered(-32768, -32768, &x, &y); assert(x == -32767 && y == -32767);

    filtered(20000, 10000, &x, &y);
    assert(x > 0 && y > 0 && x >= 2 * y - 1 && x <= 2 * y + 1);
    int32_t dx = x, dy = y;
    filtered(-20000, 10000, &x, &y); assert(x == -dx && y == dy);
    filtered(20000, -10000, &x, &y); assert(x == dx && y == -dy);
    filtered(-20000, -10000, &x, &y); assert(x == -dx && y == -dy);

    int32_t previous_axis = 0, previous_diagonal = 0;
    for (int32_t raw = 0; raw <= 32767; ++raw) {
        filtered(raw, 0, &x, &y);
        assert(x >= previous_axis && y == 0);
        previous_axis = x;
        filtered(raw, raw, &x, &y);
        assert(x == y && x >= previous_diagonal);
        previous_diagonal = x;
    }
    assert(previous_axis == 32767 && previous_diagonal == 32767);

    const int32_t samples[] = {-32768, -32767, -30000, -8001, -8000,
                                -2182, -1532, 0, 1532, 2182, 8000, 8001,
                                30000, 32767, 32768};
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; ++i)
        for (size_t j = 0; j < sizeof samples / sizeof samples[0]; ++j)
            filtered(samples[i], samples[j], &x, &y);

    puts("physical stick radial deadzone fixture passed");
    return 0;
}
