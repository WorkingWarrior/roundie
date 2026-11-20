#include "roundie_power_lifecycle.h"

#include <string.h>

static roundie_pm_state_t idle_state(const roundie_pm_t *pm)
{
    return pm->screen_on ? ROUNDIE_PM_ACTIVE : ROUNDIE_PM_SCREEN_OFF;
}

static bool idle(const roundie_pm_t *pm)
{
    return pm->state == ROUNDIE_PM_ACTIVE || pm->state == ROUNDIE_PM_SCREEN_OFF;
}

static void move(roundie_pm_t *pm, roundie_pm_state_t to, int64_t now_us)
{
    pm->time_in_us[pm->state] += now_us - pm->entered_us;
    pm->last_from = pm->state;
    pm->last_to = to;
    pm->last_transition_us = now_us;
    pm->state = to;
    pm->entered_us = now_us;
    pm->transitions++;
}

void roundie_pm_init(roundie_pm_t *pm, int64_t now_us)
{
    memset(pm, 0, sizeof(*pm));
    pm->state = ROUNDIE_PM_ACTIVE;
    pm->screen_on = true;
    pm->entered_us = now_us;
    pm->last_transition_us = now_us;
    pm->last_refusal = ROUNDIE_POWER_ALLOWED;
}

void roundie_pm_screen(roundie_pm_t *pm, bool on, int64_t now_us)
{
    pm->screen_on = on;
    if (idle(pm) && pm->state != idle_state(pm)) {
        move(pm, idle_state(pm), now_us);
    }
}

static roundie_power_refusal_t request(roundie_pm_t *pm, roundie_pm_state_t pending, int64_t now_us)
{
    if (!idle(pm)) {
        pm->rejected++;
        pm->refused++;
        pm->last_refusal = ROUNDIE_POWER_REFUSED_BUSY;
        return ROUNDIE_POWER_REFUSED_BUSY;
    }
    move(pm, pending, now_us);
    pm->attempts++;
    pm->suspend_started_us = now_us;
    return ROUNDIE_POWER_ALLOWED;
}

roundie_power_refusal_t roundie_pm_request_light_sleep(roundie_pm_t *pm, int64_t now_us)
{
    return request(pm, ROUNDIE_PM_LIGHT_SLEEP_PENDING, now_us);
}

roundie_power_refusal_t roundie_pm_request_deep_sleep(roundie_pm_t *pm, int64_t now_us)
{
    return request(pm, ROUNDIE_PM_DEEP_SLEEP_PENDING, now_us);
}

bool roundie_pm_cancel(roundie_pm_t *pm, roundie_power_refusal_t refusal, int64_t now_us)
{
    if (pm->state != ROUNDIE_PM_LIGHT_SLEEP_PENDING && pm->state != ROUNDIE_PM_DEEP_SLEEP_PENDING) {
        pm->rejected++;
        return false;
    }
    pm->refused++;
    pm->last_refusal = refusal;
    move(pm, idle_state(pm), now_us);
    return true;
}

bool roundie_pm_entered(roundie_pm_t *pm, int64_t now_us)
{
    roundie_pm_state_t to;
    if (pm->state == ROUNDIE_PM_LIGHT_SLEEP_PENDING) {
        to = ROUNDIE_PM_LIGHT_SLEEP;
    } else if (pm->state == ROUNDIE_PM_DEEP_SLEEP_PENDING) {
        to = ROUNDIE_PM_DEEP_SLEEP;
    } else {
        pm->rejected++;
        return false;
    }
    pm->last_suspend_us = now_us - pm->suspend_started_us;
    if (pm->last_suspend_us > pm->max_suspend_us) {
        pm->max_suspend_us = pm->last_suspend_us;
    }
    move(pm, to, now_us);
    return true;
}

bool roundie_pm_woke(roundie_pm_t *pm, uint32_t causes, bool user_wake, bool error, int64_t slept_us,
    int64_t now_us)
{
    if (pm->state != ROUNDIE_PM_LIGHT_SLEEP) {
        pm->rejected++;
        return false;
    }
    pm->last_wake_causes = causes;
    pm->last_slept_us = slept_us;
    if (error) {
        pm->sleep_errors++;
    } else {
        pm->light_sleeps++;
        if (user_wake) {
            pm->user_wakes++;
        } else {
            pm->timer_wakes++;
        }
    }
    pm->resume_started_us = now_us;
    move(pm, ROUNDIE_PM_RESUMING, now_us);
    return true;
}

bool roundie_pm_resumed(roundie_pm_t *pm, int64_t now_us)
{
    if (pm->state != ROUNDIE_PM_RESUMING) {
        pm->rejected++;
        return false;
    }
    pm->last_resume_us = now_us - pm->resume_started_us;
    if (pm->last_resume_us > pm->max_resume_us) {
        pm->max_resume_us = pm->last_resume_us;
    }
    move(pm, idle_state(pm), now_us);
    return true;
}

int64_t roundie_pm_time_in(const roundie_pm_t *pm, roundie_pm_state_t state, int64_t now_us)
{
    if (state >= ROUNDIE_PM_STATE_COUNT) {
        return 0;
    }
    int64_t total = pm->time_in_us[state];
    if (pm->state == state) {
        total += now_us - pm->entered_us;
    }
    return total;
}

bool roundie_pm_sleeping(const roundie_pm_t *pm)
{
    return !idle(pm);
}

const char *roundie_pm_state_name(roundie_pm_state_t state)
{
    switch (state) {
    case ROUNDIE_PM_ACTIVE: return "ACTIVE";
    case ROUNDIE_PM_SCREEN_OFF: return "SCREEN_OFF";
    case ROUNDIE_PM_LIGHT_SLEEP_PENDING: return "LIGHT_SLEEP_PENDING";
    case ROUNDIE_PM_LIGHT_SLEEP: return "LIGHT_SLEEP";
    case ROUNDIE_PM_RESUMING: return "RESUMING";
    case ROUNDIE_PM_DEEP_SLEEP_PENDING: return "DEEP_SLEEP_PENDING";
    case ROUNDIE_PM_DEEP_SLEEP: return "DEEP_SLEEP";
    case ROUNDIE_PM_STATE_COUNT: break;
    }
    return "UNKNOWN";
}
