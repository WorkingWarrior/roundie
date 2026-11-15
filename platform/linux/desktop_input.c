#include "desktop_input.h"

#include <stdio.h>
#include <string.h>

static const char *const key_names[ROUNDIE_DESKTOP_KEY_COUNT] = {"boot", "pwr"};

static int32_t clamp(int32_t value, uint32_t size)
{
    if (value < 0) {
        return 0;
    }
    if ((uint32_t)value >= size) {
        return (int32_t)size - 1;
    }
    return value;
}

void roundie_desktop_input_init(roundie_desktop_input_t *input, uint32_t panel_width, uint32_t panel_height)
{
    memset(input, 0, sizeof(*input));
    input->panel_width = panel_width;
    input->panel_height = panel_height;
}

void roundie_desktop_touch_set(roundie_desktop_input_t *input, bool pressed, int32_t x, int32_t y)
{
    input->swiping = false;
    input->pressed = pressed;
    if (pressed) {
        input->x = clamp(x, input->panel_width);
        input->y = clamp(y, input->panel_height);
    }
}

bool roundie_desktop_touch_active(const roundie_desktop_input_t *input)
{
    return input->pressed;
}

void roundie_desktop_touch_swipe(roundie_desktop_input_t *input, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                 uint32_t duration_ms, int64_t now_us)
{
    roundie_desktop_touch_set(input, true, x1, y1);
    input->swiping = true;
    input->from_x = input->x;
    input->from_y = input->y;
    input->to_x = clamp(x2, input->panel_width);
    input->to_y = clamp(y2, input->panel_height);
    input->swipe_start_us = now_us;
    input->swipe_duration_us = (int64_t)duration_ms * 1000;
}

void roundie_desktop_input_poll(roundie_desktop_input_t *input, int64_t now_us)
{
    if (!input->swiping) {
        return;
    }
    int64_t elapsed = now_us - input->swipe_start_us;
    if (elapsed >= input->swipe_duration_us) {
        if (input->x != input->to_x || input->y != input->to_y) {
            /* Report the end point once before the release. */
            input->x = input->to_x;
            input->y = input->to_y;
            return;
        }
        input->swiping = false;
        input->pressed = false;
        return;
    }
    input->x = input->from_x + (int32_t)((input->to_x - input->from_x) * elapsed / input->swipe_duration_us);
    input->y = input->from_y + (int32_t)((input->to_y - input->from_y) * elapsed / input->swipe_duration_us);
}

void roundie_desktop_touch_read(const roundie_desktop_input_t *input, bool *pressed, uint16_t *x, uint16_t *y)
{
    *pressed = input->pressed;
    *x = (uint16_t)input->x;
    *y = (uint16_t)input->y;
}

void roundie_desktop_key(roundie_desktop_input_t *input, roundie_desktop_key_t key, bool pressed)
{
    if (input->keys[key] == pressed) {
        return;
    }
    input->keys[key] = pressed;
    if (pressed) {
        input->key_presses[key]++;
        /* Milestone M1: the buttons are emulated, but the power manager that
         * consumes them on the board is not part of Roundie Core yet (M2). */
        fprintf(stderr, "roundie-desktop: %s pressed (no handler before M2)\n", key_names[key]);
    }
}

bool roundie_desktop_key_name(roundie_desktop_input_t *input, const char *name, bool pressed)
{
    for (int key = 0; key < ROUNDIE_DESKTOP_KEY_COUNT; key++) {
        if (strcmp(name, key_names[key]) == 0) {
            roundie_desktop_key(input, (roundie_desktop_key_t)key, pressed);
            return true;
        }
    }
    return false;
}

bool roundie_desktop_key_pressed(const roundie_desktop_input_t *input, roundie_desktop_key_t key)
{
    return input->keys[key];
}
