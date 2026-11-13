#include <assert.h>
#include <stdint.h>
#include "../runtime/hle/host_input_control.h"

int main(void) {
    uint64_t frame = 0;
    assert(host_input_parse_until("1", &frame) && frame == 1);
    assert(host_input_parse_until("100000", &frame) && frame == 100000);
    assert(!host_input_parse_until(NULL, &frame));
    assert(!host_input_parse_until("0", &frame));
    assert(!host_input_parse_until("01", &frame));
    assert(!host_input_parse_until("100001", &frame));
    assert(!host_input_parse_until("12 ", &frame));
    assert(!host_input_parse_until("+12", &frame));
    assert(!host_input_parse_until("18446744073709551616", &frame));
    assert(host_input_control_compatible("1:sdlkey:E:4", NULL, NULL, NULL));
    assert(!host_input_control_compatible(NULL, NULL, NULL, NULL));
    assert(!host_input_control_compatible("x", "x", NULL, NULL));
    assert(!host_input_control_compatible("x", NULL, "x", NULL));
    assert(!host_input_control_compatible("x", NULL, NULL, "0"));
    return 0;
}
