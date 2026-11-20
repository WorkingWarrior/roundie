#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "roundie_power.h"

/* System power state model (M8-B, docs/power-management-m8-b.md). Pure C, no
 * IDF and no lock: the caller serialises every call (roundie_power_shared.lock).
 *
 *   ACTIVE <-> SCREEN_OFF                       GUI task (screen policy)
 *   ACTIVE|SCREEN_OFF -> LIGHT_SLEEP_PENDING    request accepted (one at a time)
 *   LIGHT_SLEEP_PENDING -> ACTIVE|SCREEN_OFF    cancel: refusal, inhibitor, wake source
 *   LIGHT_SLEEP_PENDING -> LIGHT_SLEEP          worker, right before the sleep call
 *   LIGHT_SLEEP -> RESUMING                     worker, after the sleep call returned
 *   RESUMING -> ACTIVE|SCREEN_OFF               GUI task, display restored
 *   ACTIVE|SCREEN_OFF -> DEEP_SLEEP_PENDING     request accepted
 *   DEEP_SLEEP_PENDING -> ACTIVE|SCREEN_OFF     cancel (always in M8-B: unsupported)
 *   DEEP_SLEEP_PENDING -> DEEP_SLEEP            last step before the chip sleeps;
 *                                               the wake is a new boot, not a resume
 *
 * Every other transition is rejected and counted: no second suspend while one
 * is in progress, no resume without a suspend, a repeated resume is a no-op. */

typedef enum {
    ROUNDIE_PM_ACTIVE,
    ROUNDIE_PM_SCREEN_OFF,
    ROUNDIE_PM_LIGHT_SLEEP_PENDING,
    ROUNDIE_PM_LIGHT_SLEEP,
    ROUNDIE_PM_RESUMING,
    ROUNDIE_PM_DEEP_SLEEP_PENDING,
    ROUNDIE_PM_DEEP_SLEEP,
    ROUNDIE_PM_STATE_COUNT,
} roundie_pm_state_t;

typedef struct {
    roundie_pm_state_t state;
    bool screen_on;
    int64_t entered_us;
    int64_t time_in_us[ROUNDIE_PM_STATE_COUNT]; /* completed stays only; see roundie_pm_time_in() */
    roundie_pm_state_t last_from;
    roundie_pm_state_t last_to;
    int64_t last_transition_us;
    uint32_t transitions;
    uint32_t rejected;       /* invalid transitions: concurrent suspend, resume without suspend, ... */
    uint32_t attempts;       /* suspend requests accepted into a *_PENDING state */
    uint32_t refused;        /* requests refused or cancelled */
    roundie_power_refusal_t last_refusal;
    uint32_t light_sleeps;   /* completed light sleeps (the sleep call returned OK) */
    uint32_t sleep_errors;   /* the sleep call failed */
    uint32_t user_wakes;
    uint32_t timer_wakes;
    uint32_t last_wake_causes;
    int64_t last_slept_us;
    int64_t suspend_started_us;
    int64_t resume_started_us;
    int64_t last_suspend_us; /* request accepted -> sleep entered */
    int64_t max_suspend_us;
    int64_t last_resume_us;  /* sleep call returned -> GUI restored */
    int64_t max_resume_us;
} roundie_pm_t;

void roundie_pm_init(roundie_pm_t *pm, int64_t now_us);
/* GUI: the screen went on or off. Changes the state only from ACTIVE or
 * SCREEN_OFF; during a sleep sequence only the flag is recorded. */
void roundie_pm_screen(roundie_pm_t *pm, bool on, int64_t now_us);
/* ROUNDIE_POWER_ALLOWED: now LIGHT_SLEEP_PENDING. REFUSED_BUSY otherwise. */
roundie_power_refusal_t roundie_pm_request_light_sleep(roundie_pm_t *pm, int64_t now_us);
roundie_power_refusal_t roundie_pm_request_deep_sleep(roundie_pm_t *pm, int64_t now_us);
/* From a *_PENDING state only; the refusal is recorded. */
bool roundie_pm_cancel(roundie_pm_t *pm, roundie_power_refusal_t refusal, int64_t now_us);
bool roundie_pm_entered(roundie_pm_t *pm, int64_t now_us);
/* error: the sleep call failed (the chip did not sleep). */
bool roundie_pm_woke(roundie_pm_t *pm, uint32_t causes, bool user_wake, bool error, int64_t slept_us,
    int64_t now_us);
bool roundie_pm_resumed(roundie_pm_t *pm, int64_t now_us);
int64_t roundie_pm_time_in(const roundie_pm_t *pm, roundie_pm_state_t state, int64_t now_us);
bool roundie_pm_sleeping(const roundie_pm_t *pm); /* any state past ACTIVE/SCREEN_OFF */
const char *roundie_pm_state_name(roundie_pm_state_t state);
