// Pure filter for physical SDL stick pairs. Synthetic input and mouse camera
// bypass this helper so their existing test and control semantics stay intact.
#pragma once

#include <math.h>
#include <stdint.h>

#define INPUT_STICK_DEADZONE 8000
#define INPUT_STICK_MAX 32767

static inline int32_t input_stick_clamp_axis(int32_t value) {
    if (value < -INPUT_STICK_MAX) return -INPUT_STICK_MAX;
    if (value > INPUT_STICK_MAX) return INPUT_STICK_MAX;
    return value;
}

static inline int32_t input_stick_round_axis(double value) {
    if (value >= INPUT_STICK_MAX) return INPUT_STICK_MAX;
    if (value <= -INPUT_STICK_MAX) return -INPUT_STICK_MAX;
    return (int32_t)(value >= 0 ? value + 0.5 : value - 0.5);
}

// Remove a radial idle disk, then map the remaining distance to the full
// physical square boundary in the same direction. Axis and diagonal extrema
// therefore retain their full range; there is no step at the disk boundary.
static inline void input_stick_apply_deadzone(int32_t *x, int32_t *y) {
    int32_t clamped_x = input_stick_clamp_axis(*x);
    int32_t clamped_y = input_stick_clamp_axis(*y);
    double deflection_x = (double)clamped_x, deflection_y = (double)clamped_y;
    double radius = sqrt(deflection_x * deflection_x + deflection_y * deflection_y);
    if (radius <= INPUT_STICK_DEADZONE) {
        *x = *y = 0;
        return;
    }
    int32_t ax = clamped_x < 0 ? -clamped_x : clamped_x;
    int32_t ay = clamped_y < 0 ? -clamped_y : clamped_y;
    double max_component = (double)(ax > ay ? ax : ay);
    double max_radius = radius * ((double)INPUT_STICK_MAX / max_component);
    double mapped_radius = (radius - INPUT_STICK_DEADZONE) *
                           max_radius / (max_radius - INPUT_STICK_DEADZONE);
    double scale = mapped_radius / radius;
    *x = input_stick_round_axis(deflection_x * scale);
    *y = input_stick_round_axis(deflection_y * scale);
}
