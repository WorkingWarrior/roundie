#include "roundie_power.h"

#include <string.h>

void roundie_power_init(roundie_power_t *power, uint32_t screen_timeout_ms, int64_t now_us)
{
    memset(power, 0, sizeof(*power));
    power->state = ROUNDIE_POWER_ACTIVE;
    power->pending = ROUNDIE_POWER_NO_ACTION;
    power->last_activity_us = now_us;
    power->screen_timeout_ms = screen_timeout_ms;
    power->button_armed = true;
    power->button_changed_us = now_us;
}

bool roundie_power_touch(roundie_power_t *power, int64_t now_us, bool pressed)
{
    if (power->touch_held) {
        if (pressed) {
            power->touch_seen = true;
        } else if (power->touch_seen || now_us >= power->touch_hold_until_us) {
            power->touch_held = false;
        }
        return false;
    }
    if (!pressed) {
        return power->state == ROUNDIE_POWER_ACTIVE;
    }
    if (power->state == ROUNDIE_POWER_SCREEN_OFF) {
        power->state = ROUNDIE_POWER_ACTIVE;
        power->pending = ROUNDIE_POWER_TURN_SCREEN_ON;
        power->touch_held = true;
        power->touch_seen = true;
        power->last_activity_us = now_us;
        return false;
    }
    if (power->state != ROUNDIE_POWER_ACTIVE) {
        return false;
    }
    power->last_activity_us = now_us;
    return true;
}

static bool button_pressed(roundie_power_t *power, int64_t now_us, bool button_low)
{
    if (button_low != power->button_sample) {
        power->button_sample = button_low;
        power->button_changed_us = now_us;
        return false;
    }
    if (power->button_low == power->button_sample) {
        return false;
    }
    if (now_us - power->button_changed_us < ROUNDIE_POWER_DEBOUNCE_US) {
        return false;
    }
    power->button_low = power->button_sample;
    if (!power->button_low) {
        power->button_armed = true;
        return false;
    }
    return power->button_armed;
}

roundie_power_action_t roundie_power_tick(roundie_power_t *power, int64_t now_us,
    bool button_low, bool keep_display)
{
    if (power->state == ROUNDIE_POWER_LIGHT_SLEEP) {
        return ROUNDIE_POWER_NO_ACTION;
    }
    if (power->pending != ROUNDIE_POWER_NO_ACTION) {
        /* The button is sampled again on the next iteration. */
        roundie_power_action_t action = power->pending;
        power->pending = ROUNDIE_POWER_NO_ACTION;
        return action;
    }
    if (button_pressed(power, now_us, button_low)) {
        power->last_activity_us = now_us;
        if (power->state == ROUNDIE_POWER_SCREEN_OFF) {
            power->state = ROUNDIE_POWER_ACTIVE;
            return ROUNDIE_POWER_TURN_SCREEN_ON;
        }
        return ROUNDIE_POWER_TOGGLE_MENU;
    }
    if (keep_display) {
        power->last_activity_us = now_us;
        return ROUNDIE_POWER_NO_ACTION;
    }
    if (power->state == ROUNDIE_POWER_ACTIVE && power->screen_timeout_ms > 0 &&
        now_us - power->last_activity_us >= (int64_t)power->screen_timeout_ms * 1000) {
        power->state = ROUNDIE_POWER_SCREEN_OFF;
        return ROUNDIE_POWER_TURN_SCREEN_OFF;
    }
    return ROUNDIE_POWER_NO_ACTION;
}

void roundie_power_screen_off(roundie_power_t *power)
{
    if (power->state == ROUNDIE_POWER_ACTIVE) {
        power->state = ROUNDIE_POWER_SCREEN_OFF;
        power->pending = ROUNDIE_POWER_TURN_SCREEN_OFF;
    }
}

void roundie_power_screen_on(roundie_power_t *power, int64_t now_us)
{
    power->last_activity_us = now_us;
    if (power->state == ROUNDIE_POWER_SCREEN_OFF) {
        power->state = ROUNDIE_POWER_ACTIVE;
        power->pending = ROUNDIE_POWER_TURN_SCREEN_ON;
    }
}

void roundie_power_sleep_entered(roundie_power_t *power)
{
    power->state = ROUNDIE_POWER_LIGHT_SLEEP;
    power->pending = ROUNDIE_POWER_NO_ACTION;
}

void roundie_power_woke(roundie_power_t *power, int64_t now_us, bool user_wake)
{
    power->state = user_wake ? ROUNDIE_POWER_ACTIVE : ROUNDIE_POWER_SCREEN_OFF;
    power->pending = ROUNDIE_POWER_NO_ACTION;
    power->last_activity_us = now_us;
    power->touch_held = user_wake;
    power->touch_seen = false;
    power->touch_hold_until_us = now_us + ROUNDIE_POWER_WAKE_TOUCH_WINDOW_US;
    power->button_armed = false;
}

roundie_power_refusal_t roundie_power_sleep_refusal(const roundie_power_conditions_t *conditions)
{
    if (conditions->safe) {
        return ROUNDIE_POWER_REFUSED_SAFE_MODE;
    }
    if (!conditions->stable) {
        return ROUNDIE_POWER_REFUSED_NOT_STABLE;
    }
    if (!conditions->ota_idle) {
        return ROUNDIE_POWER_REFUSED_OTA;
    }
    if (conditions->runtime_busy) {
        return ROUNDIE_POWER_REFUSED_APP;
    }
    if (conditions->wake_source_active) {
        return ROUNDIE_POWER_REFUSED_WAKE_SOURCE;
    }
    return ROUNDIE_POWER_ALLOWED;
}

const char *roundie_power_state_name(roundie_power_state_t state)
{
    switch (state) {
    case ROUNDIE_POWER_ACTIVE: return "active";
    case ROUNDIE_POWER_SCREEN_OFF: return "screen_off";
    case ROUNDIE_POWER_LIGHT_SLEEP: return "light_sleep";
    }
    return "unknown";
}

const char *roundie_power_refusal_name(roundie_power_refusal_t refusal)
{
    switch (refusal) {
    case ROUNDIE_POWER_ALLOWED: return "none";
    case ROUNDIE_POWER_REFUSED_SAFE_MODE: return "safe_mode";
    case ROUNDIE_POWER_REFUSED_NOT_STABLE: return "not_stable";
    case ROUNDIE_POWER_REFUSED_OTA: return "ota";
    case ROUNDIE_POWER_REFUSED_APP: return "app";
    case ROUNDIE_POWER_REFUSED_WAKE_SOURCE: return "wake_source";
    }
    return "unknown";
}
