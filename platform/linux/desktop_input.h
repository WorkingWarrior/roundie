#pragma once

/* Emulated touch panel and buttons of the Linux desktop. Coordinates are
 * panel pixels, as the board's touch controller reports them. GUI thread
 * only: SDL events, scripts and the LVGL read callback all run there. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ROUNDIE_DESKTOP_KEY_BOOT,
    ROUNDIE_DESKTOP_KEY_PWR,
    ROUNDIE_DESKTOP_KEY_COUNT,
} roundie_desktop_key_t;

typedef struct {
    uint32_t panel_width;
    uint32_t panel_height;
    bool pressed;
    int32_t x;
    int32_t y;
    bool swiping;
    int32_t from_x;
    int32_t from_y;
    int32_t to_x;
    int32_t to_y;
    int64_t swipe_start_us;
    int64_t swipe_duration_us;
    bool keys[ROUNDIE_DESKTOP_KEY_COUNT];
    uint32_t key_presses[ROUNDIE_DESKTOP_KEY_COUNT];
    int64_t key_release_us[ROUNDIE_DESKTOP_KEY_COUNT]; /* timed hold; 0 when none */
} roundie_desktop_input_t;

void roundie_desktop_input_init(roundie_desktop_input_t *input, uint32_t panel_width, uint32_t panel_height);
/* Press (clamped to the panel) or release; cancels a scripted swipe. */
void roundie_desktop_touch_set(roundie_desktop_input_t *input, bool pressed, int32_t x, int32_t y);
bool roundie_desktop_touch_active(const roundie_desktop_input_t *input);
/* A press moving linearly from (x1, y1) to (x2, y2) over duration_ms, then
 * a release; roundie_desktop_input_poll advances it. */
void roundie_desktop_touch_swipe(roundie_desktop_input_t *input, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                 uint32_t duration_ms, int64_t now_us);
void roundie_desktop_input_poll(roundie_desktop_input_t *input, int64_t now_us);
void roundie_desktop_touch_read(const roundie_desktop_input_t *input, bool *pressed, uint16_t *x, uint16_t *y);

void roundie_desktop_key(roundie_desktop_input_t *input, roundie_desktop_key_t key, bool pressed);
/* "boot" or "pwr"; false for any other name. */
bool roundie_desktop_key_name(roundie_desktop_input_t *input, const char *name, bool pressed);
/* "boot" or "pwr" pressed now and released by roundie_desktop_input_poll()
 * at release_us; false for any other name. */
bool roundie_desktop_key_hold(roundie_desktop_input_t *input, const char *name, int64_t release_us);
bool roundie_desktop_key_pressed(const roundie_desktop_input_t *input, roundie_desktop_key_t key);
