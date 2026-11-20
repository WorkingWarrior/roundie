/* M8-B power state model and sleep inhibitors: the actual pure modules
 * (components/roundie_power/lifecycle.c, inhibit.c). No IDF, no hardware.
 * Scenario numbers refer to docs/power-management-m8-b.md. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "roundie_power_inhibit.h"
#include "roundie_power_lifecycle.h"

#define MS INT64_C(1000)
#define TIMER_WAKE 0x10U

static void assert_idle(const roundie_pm_t *pm, roundie_pm_state_t state)
{
    assert(pm->state == state);
    assert(!roundie_pm_sleeping(pm));
}

/* 1: ACTIVE -> SCREEN_OFF -> ACTIVE, with time accounting. */
static void screen_off_and_on(void)
{
    roundie_pm_t pm;
    roundie_pm_init(&pm, 0);
    roundie_pm_screen(&pm, false, 100 * MS);
    assert_idle(&pm, ROUNDIE_PM_SCREEN_OFF);
    assert(pm.last_from == ROUNDIE_PM_ACTIVE && pm.last_to == ROUNDIE_PM_SCREEN_OFF);
    roundie_pm_screen(&pm, false, 150 * MS); /* repeated: no transition */
    assert(pm.transitions == 1U);
    roundie_pm_screen(&pm, true, 400 * MS);
    assert_idle(&pm, ROUNDIE_PM_ACTIVE);
    assert(roundie_pm_time_in(&pm, ROUNDIE_PM_ACTIVE, 500 * MS) == 200 * MS);
    assert(roundie_pm_time_in(&pm, ROUNDIE_PM_SCREEN_OFF, 500 * MS) == 300 * MS);
    assert(pm.rejected == 0U);
}

/* 2, 12: a full light sleep with a timer wake keeps the screen off; a user
 * wake turns it on. Suspend and resume durations are measured. */
static void light_sleep_cycle(void)
{
    roundie_pm_t pm;
    roundie_pm_init(&pm, 0);
    assert(roundie_pm_request_light_sleep(&pm, 10 * MS) == ROUNDIE_POWER_ALLOWED);
    assert(pm.state == ROUNDIE_PM_LIGHT_SLEEP_PENDING && roundie_pm_sleeping(&pm));
    roundie_pm_screen(&pm, false, 20 * MS); /* the GUI parks: flag only */
    assert(pm.state == ROUNDIE_PM_LIGHT_SLEEP_PENDING);
    assert(roundie_pm_entered(&pm, 410 * MS));
    assert(pm.state == ROUNDIE_PM_LIGHT_SLEEP && pm.last_suspend_us == 400 * MS);
    assert(roundie_pm_woke(&pm, TIMER_WAKE, false, false, 5000 * MS, 5410 * MS));
    assert(pm.state == ROUNDIE_PM_RESUMING && pm.timer_wakes == 1U && pm.light_sleeps == 1U);
    assert(roundie_pm_resumed(&pm, 5450 * MS));
    assert_idle(&pm, ROUNDIE_PM_SCREEN_OFF);
    assert(pm.last_resume_us == 40 * MS);
    assert(roundie_pm_time_in(&pm, ROUNDIE_PM_LIGHT_SLEEP, 6000 * MS) == 5000 * MS);

    assert(roundie_pm_request_light_sleep(&pm, 6000 * MS) == ROUNDIE_POWER_ALLOWED);
    assert(roundie_pm_entered(&pm, 6100 * MS));
    assert(roundie_pm_woke(&pm, 0x100U, true, false, 700 * MS, 6800 * MS));
    roundie_pm_screen(&pm, true, 6820 * MS); /* user wake: GUI turns the screen on */
    assert(pm.state == ROUNDIE_PM_RESUMING);
    assert(roundie_pm_resumed(&pm, 6830 * MS));
    assert_idle(&pm, ROUNDIE_PM_ACTIVE);
    assert(pm.user_wakes == 1U && pm.light_sleeps == 2U && pm.rejected == 0U);
}

/* 3, 4: a request refused or cancelled returns to where it came from. */
static void cancel_returns_to_idle(void)
{
    roundie_pm_t pm;
    roundie_pm_init(&pm, 0);
    roundie_pm_screen(&pm, false, 1 * MS);
    assert(roundie_pm_request_light_sleep(&pm, 2 * MS) == ROUNDIE_POWER_ALLOWED);
    assert(roundie_pm_cancel(&pm, ROUNDIE_POWER_REFUSED_INHIBITED, 3 * MS));
    assert_idle(&pm, ROUNDIE_PM_SCREEN_OFF);
    assert(pm.refused == 1U && pm.last_refusal == ROUNDIE_POWER_REFUSED_INHIBITED);
    /* Cancel without a pending request is rejected and changes nothing. */
    assert(!roundie_pm_cancel(&pm, ROUNDIE_POWER_REFUSED_APP, 4 * MS));
    assert_idle(&pm, ROUNDIE_PM_SCREEN_OFF);
    assert(pm.rejected == 1U && pm.refused == 1U);
    /* Cancel after the sleep started is rejected: the wake path resumes. */
    assert(roundie_pm_request_light_sleep(&pm, 5 * MS) == ROUNDIE_POWER_ALLOWED);
    assert(roundie_pm_entered(&pm, 6 * MS));
    assert(!roundie_pm_cancel(&pm, ROUNDIE_POWER_REFUSED_APP, 7 * MS));
    assert(pm.state == ROUNDIE_PM_LIGHT_SLEEP);
}

/* 5, 6, 7: one suspend at a time; no resume without a suspend; a repeated
 * resume is rejected and has no effect. */
static void ordering(void)
{
    roundie_pm_t pm;
    roundie_pm_init(&pm, 0);
    assert(!roundie_pm_woke(&pm, TIMER_WAKE, false, false, 0, 1 * MS));
    assert(!roundie_pm_resumed(&pm, 1 * MS));
    assert(!roundie_pm_entered(&pm, 1 * MS));
    assert(pm.rejected == 3U && pm.state == ROUNDIE_PM_ACTIVE && pm.transitions == 0U);

    assert(roundie_pm_request_light_sleep(&pm, 2 * MS) == ROUNDIE_POWER_ALLOWED);
    assert(roundie_pm_request_light_sleep(&pm, 2 * MS) == ROUNDIE_POWER_REFUSED_BUSY);
    assert(roundie_pm_request_deep_sleep(&pm, 2 * MS) == ROUNDIE_POWER_REFUSED_BUSY);
    assert(pm.attempts == 1U && pm.last_refusal == ROUNDIE_POWER_REFUSED_BUSY);
    assert(roundie_pm_entered(&pm, 3 * MS));
    assert(roundie_pm_request_light_sleep(&pm, 3 * MS) == ROUNDIE_POWER_REFUSED_BUSY);
    assert(roundie_pm_woke(&pm, TIMER_WAKE, false, false, 1, 4 * MS));
    /* 9: an event (here a second wake report) during RESUMING is rejected. */
    assert(!roundie_pm_woke(&pm, TIMER_WAKE, false, false, 1, 4 * MS));
    assert(roundie_pm_request_light_sleep(&pm, 4 * MS) == ROUNDIE_POWER_REFUSED_BUSY);
    assert(roundie_pm_resumed(&pm, 5 * MS));
    uint32_t transitions = pm.transitions;
    assert(!roundie_pm_resumed(&pm, 6 * MS));
    assert(pm.transitions == transitions && pm.light_sleeps == 1U);
}

/* 15: the sleep call failed; the device resumes like after a wake and
 * counts an error, not a sleep. */
static void sleep_call_failure(void)
{
    roundie_pm_t pm;
    roundie_pm_init(&pm, 0);
    assert(roundie_pm_request_light_sleep(&pm, 1 * MS) == ROUNDIE_POWER_ALLOWED);
    assert(roundie_pm_entered(&pm, 2 * MS));
    assert(roundie_pm_woke(&pm, 0, true, true, 0, 3 * MS));
    assert(pm.sleep_errors == 1U && pm.light_sleeps == 0U && pm.user_wakes == 0U);
    roundie_pm_screen(&pm, true, 3 * MS);
    assert(roundie_pm_resumed(&pm, 4 * MS));
    assert_idle(&pm, ROUNDIE_PM_ACTIVE);
}

/* Deep sleep in M8-B: the request is cancelled as unsupported; the state
 * model still has the path for a later, validated implementation. */
static void deep_sleep_refused(void)
{
    roundie_pm_t pm;
    roundie_pm_init(&pm, 0);
    assert(roundie_pm_request_deep_sleep(&pm, 1 * MS) == ROUNDIE_POWER_ALLOWED);
    assert(pm.state == ROUNDIE_PM_DEEP_SLEEP_PENDING);
    assert(roundie_pm_cancel(&pm, ROUNDIE_POWER_REFUSED_UNSUPPORTED, 2 * MS));
    assert_idle(&pm, ROUNDIE_PM_ACTIVE);
    assert(pm.last_refusal == ROUNDIE_POWER_REFUSED_UNSUPPORTED);
    /* A deep sleep is never "woken" into RESUMING: the wake is a new boot. */
    assert(roundie_pm_request_deep_sleep(&pm, 3 * MS) == ROUNDIE_POWER_ALLOWED);
    assert(roundie_pm_entered(&pm, 4 * MS));
    assert(pm.state == ROUNDIE_PM_DEEP_SLEEP);
    assert(!roundie_pm_woke(&pm, TIMER_WAKE, false, false, 0, 5 * MS));
}

/* 21: many cycles keep every counter consistent. */
static void many_cycles(void)
{
    roundie_pm_t pm;
    roundie_pm_init(&pm, 0);
    int64_t now = 0;
    for (uint32_t cycle = 0; cycle < 10000U; cycle++) {
        roundie_pm_screen(&pm, false, now += MS);
        roundie_pm_screen(&pm, true, now += MS);
        assert(roundie_pm_request_light_sleep(&pm, now += MS) == ROUNDIE_POWER_ALLOWED);
        if (cycle % 7U == 0U) {
            assert(roundie_pm_cancel(&pm, ROUNDIE_POWER_REFUSED_WAKE_SOURCE, now += MS));
            continue;
        }
        roundie_pm_screen(&pm, false, now += MS);
        assert(roundie_pm_entered(&pm, now += MS));
        assert(roundie_pm_woke(&pm, TIMER_WAKE, false, false, 2 * MS, now += 2 * MS));
        assert(roundie_pm_resumed(&pm, now += MS));
        assert_idle(&pm, ROUNDIE_PM_SCREEN_OFF);
    }
    assert(pm.rejected == 0U);
    assert(pm.attempts == 10000U && pm.refused == 1429U && pm.light_sleeps == 8571U);
    int64_t total = 0;
    for (int state = 0; state < ROUNDIE_PM_STATE_COUNT; state++) {
        total += roundie_pm_time_in(&pm, (roundie_pm_state_t)state, now);
    }
    assert(total == now);
}

/* 3, 19: inhibitors have owners, kinds, handles and timeouts; double and
 * stale releases are rejected; leaks end by timeout or owner release. */
static void inhibitors(void)
{
    roundie_power_inhibitors_t table;
    roundie_power_inhibit_init(&table);
    roundie_power_inhibit_handle_t handle = 0;
    assert(roundie_power_inhibit_acquire(&table, ROUNDIE_POWER_OWNER_NONE, ROUNDIE_POWER_INHIBIT_CPU_AWAKE, 0,
                                         MS, &handle) == ROUNDIE_POWER_INHIBIT_INVALID && handle == 0U);
    assert(roundie_power_inhibit_acquire(&table, ROUNDIE_POWER_OWNER_OTA, 0, 0, MS, &handle) ==
           ROUNDIE_POWER_INHIBIT_INVALID);
    assert(roundie_power_inhibit_acquire(&table, ROUNDIE_POWER_OWNER_OTA, 0x80U, 0, MS, &handle) ==
           ROUNDIE_POWER_INHIBIT_INVALID);
    assert(roundie_power_inhibit_acquire(&table, ROUNDIE_POWER_OWNER_OTA, ROUNDIE_POWER_INHIBIT_CPU_AWAKE, 0, 0,
                                         &handle) == ROUNDIE_POWER_INHIBIT_INVALID);
    assert(roundie_power_inhibit_acquire(&table, ROUNDIE_POWER_OWNER_OTA, ROUNDIE_POWER_INHIBIT_CPU_AWAKE, 0,
                                         ROUNDIE_POWER_INHIBIT_MAX_TIMEOUT_US + 1, &handle) ==
           ROUNDIE_POWER_INHIBIT_INVALID);

    roundie_power_inhibit_handle_t ota = 0;
    roundie_power_inhibit_handle_t display = 0;
    assert(roundie_power_inhibit_acquire(&table, ROUNDIE_POWER_OWNER_OTA, ROUNDIE_POWER_INHIBIT_CPU_AWAKE,
                                         10 * MS, 1000 * MS, &ota) == ROUNDIE_POWER_INHIBIT_OK);
    assert(ota != 0U);
    assert(roundie_power_inhibit_acquire(&table, ROUNDIE_POWER_OWNER_GUI, ROUNDIE_POWER_INHIBIT_DISPLAY_AWAKE,
                                         10 * MS, 50 * MS, &display) == ROUNDIE_POWER_INHIBIT_OK);
    assert(roundie_power_inhibit_active(&table) ==
           (ROUNDIE_POWER_INHIBIT_CPU_AWAKE | ROUNDIE_POWER_INHIBIT_DISPLAY_AWAKE));
    uint32_t owners = 0;
    assert(roundie_power_inhibit_held(&table, &owners) == 2U);
    assert(owners == ((1U << ROUNDIE_POWER_OWNER_OTA) | (1U << ROUNDIE_POWER_OWNER_GUI)));

    /* Timeout: the display inhibitor leaked and expires; OTA stays. */
    assert(roundie_power_inhibit_expire(&table, 59 * MS) == 0U);
    assert(roundie_power_inhibit_expire(&table, 60 * MS) == 1U);
    assert(table.expired == 1U && roundie_power_inhibit_active(&table) == ROUNDIE_POWER_INHIBIT_CPU_AWAKE);
    /* Its handle is stale now and cannot release a later holder of the slot. */
    roundie_power_inhibit_handle_t reused = 0;
    assert(roundie_power_inhibit_acquire(&table, ROUNDIE_POWER_OWNER_AUDIO, ROUNDIE_POWER_INHIBIT_CPU_AWAKE,
                                         70 * MS, 1000 * MS, &reused) == ROUNDIE_POWER_INHIBIT_OK);
    assert((reused & 0xffU) == (display & 0xffU) && reused != display);
    assert(roundie_power_inhibit_release(&table, display, 80 * MS) == ROUNDIE_POWER_INHIBIT_STALE);
    assert(roundie_power_inhibit_held(&table, NULL) == 2U);

    /* Double release. */
    assert(roundie_power_inhibit_release(&table, ota, 90 * MS) == ROUNDIE_POWER_INHIBIT_OK);
    assert(roundie_power_inhibit_release(&table, ota, 91 * MS) == ROUNDIE_POWER_INHIBIT_STALE);
    assert(roundie_power_inhibit_release(&table, 0, 91 * MS) == ROUNDIE_POWER_INHIBIT_STALE);
    assert(roundie_power_inhibit_release(&table, 0x00010009U, 91 * MS) == ROUNDIE_POWER_INHIBIT_STALE);
    assert(roundie_power_inhibit_release(&table, 0x00010101U, 91 * MS) == ROUNDIE_POWER_INHIBIT_STALE);
    assert(table.stale_releases == 5U && table.released == 1U);
    assert(table.longest_hold_us == 80 * MS);

    /* Owner release ends everything an owner holds. */
    assert(roundie_power_inhibit_release_owner(&table, ROUNDIE_POWER_OWNER_AUDIO, 100 * MS) == 1U);
    assert(roundie_power_inhibit_held(&table, NULL) == 0U && roundie_power_inhibit_active(&table) == 0U);

    /* Full table: the caller learns it and must not start its operation. */
    roundie_power_inhibit_handle_t handles[ROUNDIE_POWER_INHIBIT_SLOTS];
    for (uint32_t index = 0; index < ROUNDIE_POWER_INHIBIT_SLOTS; index++) {
        assert(roundie_power_inhibit_acquire(&table, ROUNDIE_POWER_OWNER_DIAGNOSTICS,
                                             ROUNDIE_POWER_INHIBIT_NO_DEEP_SLEEP, 0, MS, &handles[index]) ==
               ROUNDIE_POWER_INHIBIT_OK);
    }
    assert(roundie_power_inhibit_acquire(&table, ROUNDIE_POWER_OWNER_OTA, ROUNDIE_POWER_INHIBIT_CPU_AWAKE, 0, MS,
                                         &handle) == ROUNDIE_POWER_INHIBIT_FULL && handle == 0U);
    assert(table.full == 1U);
    for (uint32_t index = 0; index < ROUNDIE_POWER_INHIBIT_SLOTS; index++) {
        assert(roundie_power_inhibit_release(&table, handles[index], 0) == ROUNDIE_POWER_INHIBIT_OK);
    }
    /* 19: after all of this, nothing is held. */
    assert(roundie_power_inhibit_held(&table, NULL) == 0U);
    assert(table.acquired == table.released + table.expired + table.owner_releases);
    assert(roundie_power_owner_name(ROUNDIE_POWER_OWNER_OTA)[0] == 'o');
}

/* Handle generations survive many reuses of one slot (wrap skips 0). */
static void generation_wrap(void)
{
    roundie_power_inhibitors_t table;
    roundie_power_inhibit_init(&table);
    roundie_power_inhibit_handle_t first = 0;
    assert(roundie_power_inhibit_acquire(&table, ROUNDIE_POWER_OWNER_SYSTEM, ROUNDIE_POWER_INHIBIT_CPU_AWAKE, 0, MS,
                                         &first) == ROUNDIE_POWER_INHIBIT_OK);
    assert(roundie_power_inhibit_release(&table, first, 0) == ROUNDIE_POWER_INHIBIT_OK);
    for (uint32_t cycle = 0; cycle < 70000U; cycle++) {
        roundie_power_inhibit_handle_t handle = 0;
        assert(roundie_power_inhibit_acquire(&table, ROUNDIE_POWER_OWNER_SYSTEM, ROUNDIE_POWER_INHIBIT_CPU_AWAKE, 0,
                                             MS, &handle) == ROUNDIE_POWER_INHIBIT_OK);
        assert(handle != 0U && (handle >> 16) != 0U);
        assert(roundie_power_inhibit_release(&table, handle, 0) == ROUNDIE_POWER_INHIBIT_OK);
        assert(roundie_power_inhibit_release(&table, handle, 0) == ROUNDIE_POWER_INHIBIT_STALE);
    }
}

/* 5, 19: concurrent acquire/release and suspend requests under one lock, as
 * roundie_power_shared.lock serialises them; run under TSan by test_power.py. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static roundie_power_inhibitors_t shared_table;
static roundie_pm_t shared_pm;
static uint32_t accepted_requests;

static void *worker(void *argument)
{
    roundie_power_owner_t owner = (roundie_power_owner_t)(intptr_t)argument;
    for (uint32_t step = 0; step < 20000U; step++) {
        roundie_power_inhibit_handle_t handle = 0;
        pthread_mutex_lock(&lock);
        roundie_power_inhibit_result_t result = roundie_power_inhibit_acquire(&shared_table, owner,
            ROUNDIE_POWER_INHIBIT_CPU_AWAKE, step, 1000, &handle);
        if (roundie_pm_request_light_sleep(&shared_pm, step) == ROUNDIE_POWER_ALLOWED) {
            accepted_requests++;
            assert(roundie_pm_cancel(&shared_pm, ROUNDIE_POWER_REFUSED_INHIBITED, step));
        }
        pthread_mutex_unlock(&lock);
        pthread_mutex_lock(&lock);
        if (result == ROUNDIE_POWER_INHIBIT_OK) {
            assert(roundie_power_inhibit_release(&shared_table, handle, step) == ROUNDIE_POWER_INHIBIT_OK);
            assert(roundie_power_inhibit_release(&shared_table, handle, step) == ROUNDIE_POWER_INHIBIT_STALE);
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static void concurrency(void)
{
    roundie_power_inhibit_init(&shared_table);
    roundie_pm_init(&shared_pm, 0);
    pthread_t threads[4];
    for (int index = 0; index < 4; index++) {
        assert(pthread_create(&threads[index], NULL, worker,
                              (void *)(intptr_t)(ROUNDIE_POWER_OWNER_SYSTEM + index)) == 0);
    }
    for (int index = 0; index < 4; index++) {
        assert(pthread_join(threads[index], NULL) == 0);
    }
    assert(roundie_power_inhibit_held(&shared_table, NULL) == 0U);
    assert(shared_table.acquired == 80000U && shared_table.released == 80000U);
    assert(shared_table.stale_releases == 80000U && shared_table.full == 0U);
    assert(accepted_requests == 80000U && shared_pm.rejected == 0U);
    assert_idle(&shared_pm, ROUNDIE_PM_ACTIVE);
}

int main(void)
{
    screen_off_and_on();
    light_sleep_cycle();
    cancel_returns_to_idle();
    ordering();
    sleep_call_failure();
    deep_sleep_refused();
    many_cycles();
    inhibitors();
    generation_wrap();
    concurrency();
    puts("Power state model and inhibitors PASS");
    return 0;
}
