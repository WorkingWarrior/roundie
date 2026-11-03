#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Hardware-independent PowerManager policy (REP §17). The GUI task owns one
 * roundie_power_t; the system worker evaluates light-sleep admission. No
 * locks, allocation, logging or IDF calls. POWER_OFF is not represented: the
 * documented PWR latch has no MCU control, and deep sleep is not offered. */

typedef enum {
    ROUNDIE_POWER_ACTIVE,
    ROUNDIE_POWER_SCREEN_OFF,
    ROUNDIE_POWER_LIGHT_SLEEP,
} roundie_power_state_t;

typedef enum {
    ROUNDIE_POWER_NO_ACTION,
    ROUNDIE_POWER_TURN_SCREEN_OFF,
    ROUNDIE_POWER_TURN_SCREEN_ON,
    ROUNDIE_POWER_TOGGLE_MENU,
} roundie_power_action_t;

typedef enum {
    ROUNDIE_POWER_ALLOWED,
    ROUNDIE_POWER_REFUSED_SAFE_MODE,
    ROUNDIE_POWER_REFUSED_NOT_STABLE,
    ROUNDIE_POWER_REFUSED_OTA,
    ROUNDIE_POWER_REFUSED_APP,
    ROUNDIE_POWER_REFUSED_WAKE_SOURCE,
} roundie_power_refusal_t;

/* System conditions sampled by the worker immediately before sleeping. */
typedef struct {
    bool safe;
    bool stable;
    bool ota_idle;
    bool runtime_busy;
    bool wake_source_active;
} roundie_power_conditions_t;

#define ROUNDIE_POWER_DEBOUNCE_US 30000
#define ROUNDIE_POWER_WAKE_TOUCH_WINDOW_US 500000

typedef struct {
    roundie_power_state_t state;
    roundie_power_action_t pending;
    int64_t last_activity_us;
    uint32_t screen_timeout_ms;
    /* The touch that woke the screen is withheld from LVGL until release.
     * After a sleep wake the press may not have been read yet: the hold
     * lasts until a seen press is released, or until the window passes
     * without a press (a BOOT or timer wake). */
    bool touch_held;
    bool touch_seen;
    int64_t touch_hold_until_us;
    /* Debounced BOOT level, the latest raw sample and when it last changed. */
    bool button_low;
    bool button_sample;
    int64_t button_changed_us;
    /* A press counts only after a release; a press that woke the device
     * must not also open the menu. */
    bool button_armed;
} roundie_power_t;

/* screen_timeout_ms 0 disables automatic screen off. */
void roundie_power_init(roundie_power_t *power, uint32_t screen_timeout_ms, int64_t now_us);
/* Touch read callback. Returns whether LVGL may receive this sample. */
bool roundie_power_touch(roundie_power_t *power, int64_t now_us, bool pressed);
/* Once per GUI iteration, after touch. keep_display postpones the timeout.
 * Returns at most one action; the caller applies it to the panel or menu. */
roundie_power_action_t roundie_power_tick(roundie_power_t *power, int64_t now_us,
    bool button_low, bool keep_display);
/* Explicit screen-off request (menu or diagnostics), not subject to keep_display. */
void roundie_power_screen_off(roundie_power_t *power);
void roundie_power_screen_on(roundie_power_t *power, int64_t now_us);
void roundie_power_sleep_entered(roundie_power_t *power);
/* user_wake: a touch or BOOT woke the device. A timer wake keeps the screen off. */
void roundie_power_woke(roundie_power_t *power, int64_t now_us, bool user_wake);

roundie_power_refusal_t roundie_power_sleep_refusal(const roundie_power_conditions_t *conditions);
const char *roundie_power_state_name(roundie_power_state_t state);
const char *roundie_power_refusal_name(roundie_power_refusal_t refusal);
