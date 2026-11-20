/* Actual PowerManager policy: screen timeout, wake touch, BOOT debounce and
 * light-sleep admission. Pure C, no IDF. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "roundie_power.h"

#define MS INT64_C(1000)

static roundie_power_action_t hold_button(roundie_power_t *power, int64_t *now, bool low)
{
    roundie_power_action_t last = ROUNDIE_POWER_NO_ACTION;
    for (int step = 0; step < 6; step++) {
        roundie_power_action_t action = roundie_power_tick(power, *now, low, false);
        if (action != ROUNDIE_POWER_NO_ACTION) {
            assert(last == ROUNDIE_POWER_NO_ACTION);
            last = action;
        }
        *now += 10 * MS;
    }
    return last;
}

static void timeout_and_touch_wake(void)
{
    roundie_power_t power;
    int64_t now = 1000 * MS;
    roundie_power_init(&power, 30000, now);
    assert(roundie_power_tick(&power, now + 29999 * MS, false, false) == ROUNDIE_POWER_NO_ACTION);
    /* Touch is activity and is delivered while active. */
    assert(roundie_power_touch(&power, now + 20000 * MS, true));
    assert(roundie_power_touch(&power, now + 20100 * MS, false));
    assert(roundie_power_tick(&power, now + 49000 * MS, false, false) == ROUNDIE_POWER_NO_ACTION);
    assert(roundie_power_tick(&power, now + 50100 * MS, false, false) == ROUNDIE_POWER_TURN_SCREEN_OFF);
    assert(power.state == ROUNDIE_POWER_SCREEN_OFF);
    assert(roundie_power_tick(&power, now + 90000 * MS, false, false) == ROUNDIE_POWER_NO_ACTION);
    /* Releases while dark are not delivered. The waking press and its whole
     * contact are withheld, then normal delivery resumes. */
    assert(!roundie_power_touch(&power, now + 91000 * MS, false));
    assert(!roundie_power_touch(&power, now + 92000 * MS, true));
    assert(power.state == ROUNDIE_POWER_ACTIVE);
    assert(roundie_power_tick(&power, now + 92010 * MS, false, false) == ROUNDIE_POWER_TURN_SCREEN_ON);
    assert(!roundie_power_touch(&power, now + 92100 * MS, true));
    assert(!roundie_power_touch(&power, now + 92200 * MS, false));
    assert(roundie_power_touch(&power, now + 92300 * MS, true));
    puts("timeout and touch wake: PASS");
}

static void keep_display_and_disabled_timeout(void)
{
    roundie_power_t power;
    roundie_power_init(&power, 30000, 0);
    assert(roundie_power_tick(&power, 60000 * MS, false, true) == ROUNDIE_POWER_NO_ACTION);
    /* The timeout restarts when the inhibiting condition ends. */
    assert(roundie_power_tick(&power, 89000 * MS, false, false) == ROUNDIE_POWER_NO_ACTION);
    assert(roundie_power_tick(&power, 90000 * MS, false, false) == ROUNDIE_POWER_TURN_SCREEN_OFF);

    roundie_power_init(&power, 0, 0);
    assert(roundie_power_tick(&power, 3600000 * MS, false, false) == ROUNDIE_POWER_NO_ACTION);
    /* An explicit request is honoured even when the timeout is disabled. */
    roundie_power_screen_off(&power);
    assert(roundie_power_tick(&power, 3600001 * MS, false, true) == ROUNDIE_POWER_TURN_SCREEN_OFF);
    roundie_power_screen_on(&power, 3600002 * MS);
    assert(roundie_power_tick(&power, 3600003 * MS, false, false) == ROUNDIE_POWER_TURN_SCREEN_ON);
    puts("keep display and disabled timeout: PASS");
}

static void boot_button(void)
{
    roundie_power_t power;
    int64_t now = 0;
    roundie_power_init(&power, 30000, now);
    /* A 10 ms glitch is shorter than the debounce and does nothing. */
    assert(roundie_power_tick(&power, now, true, false) == ROUNDIE_POWER_NO_ACTION);
    assert(roundie_power_tick(&power, now + 10 * MS, false, false) == ROUNDIE_POWER_NO_ACTION);
    now += 100 * MS;
    assert(hold_button(&power, &now, false) == ROUNDIE_POWER_NO_ACTION);
    /* Press opens the menu once; holding does not repeat. */
    assert(hold_button(&power, &now, true) == ROUNDIE_POWER_TOGGLE_MENU);
    assert(hold_button(&power, &now, true) == ROUNDIE_POWER_NO_ACTION);
    assert(hold_button(&power, &now, false) == ROUNDIE_POWER_NO_ACTION);
    assert(hold_button(&power, &now, true) == ROUNDIE_POWER_TOGGLE_MENU);
    assert(hold_button(&power, &now, false) == ROUNDIE_POWER_NO_ACTION);
    /* While dark the press only turns the screen on. */
    roundie_power_screen_off(&power);
    assert(roundie_power_tick(&power, now, false, false) == ROUNDIE_POWER_TURN_SCREEN_OFF);
    assert(hold_button(&power, &now, true) == ROUNDIE_POWER_TURN_SCREEN_ON);
    assert(power.state == ROUNDIE_POWER_ACTIVE);
    assert(hold_button(&power, &now, false) == ROUNDIE_POWER_NO_ACTION);
    puts("BOOT button: PASS");
}

static void sleep_and_wake(void)
{
    roundie_power_t power;
    int64_t now = 0;
    roundie_power_init(&power, 30000, now);
    roundie_power_sleep_entered(&power);
    assert(!roundie_power_touch(&power, now, true));
    assert(roundie_power_tick(&power, now, true, false) == ROUNDIE_POWER_NO_ACTION);

    /* BOOT woke the device and is still held: no menu until released. */
    roundie_power_woke(&power, now, true);
    assert(power.state == ROUNDIE_POWER_ACTIVE);
    assert(hold_button(&power, &now, true) == ROUNDIE_POWER_NO_ACTION);
    assert(hold_button(&power, &now, false) == ROUNDIE_POWER_NO_ACTION);
    assert(hold_button(&power, &now, true) == ROUNDIE_POWER_TOGGLE_MENU);
    assert(hold_button(&power, &now, false) == ROUNDIE_POWER_NO_ACTION);
    /* The waking touch is withheld until release. */
    roundie_power_sleep_entered(&power);
    roundie_power_woke(&power, now, true);
    assert(!roundie_power_touch(&power, now, true));
    assert(!roundie_power_touch(&power, now + 50 * MS, false));
    assert(roundie_power_touch(&power, now + 60 * MS, true));
    /* The waking press is read late: releases before it do not end the hold. */
    roundie_power_sleep_entered(&power);
    roundie_power_woke(&power, now, true);
    assert(!roundie_power_touch(&power, now + 20 * MS, false));
    assert(!roundie_power_touch(&power, now + 80 * MS, true));
    assert(!roundie_power_touch(&power, now + 900 * MS, true));
    assert(!roundie_power_touch(&power, now + 950 * MS, false));
    assert(roundie_power_touch(&power, now + 960 * MS, true));
    /* A BOOT wake with no touch: the hold lapses after the window. */
    roundie_power_sleep_entered(&power);
    roundie_power_woke(&power, now, true);
    assert(!roundie_power_touch(&power, now + 100 * MS, false));
    assert(!roundie_power_touch(&power, now + 499 * MS, false));
    assert(!roundie_power_touch(&power, now + 500 * MS, false));
    assert(roundie_power_touch(&power, now + 510 * MS, true));

    /* A timer wake leaves the screen off; the next touch turns it on. */
    roundie_power_sleep_entered(&power);
    roundie_power_woke(&power, now, false);
    assert(power.state == ROUNDIE_POWER_SCREEN_OFF);
    assert(!roundie_power_touch(&power, now + 10 * MS, true));
    assert(roundie_power_tick(&power, now + 20 * MS, false, false) == ROUNDIE_POWER_TURN_SCREEN_ON);
    /* M8-B regression (found in the VHL): after a timer wake, with BOOT
     * released, the first BOOT press turns the screen on; before the fix it
     * only armed the button and a second press was needed. */
    roundie_power_sleep_entered(&power);
    roundie_power_woke(&power, now, false);
    assert(power.state == ROUNDIE_POWER_SCREEN_OFF);
    assert(hold_button(&power, &now, false) == ROUNDIE_POWER_NO_ACTION);
    assert(hold_button(&power, &now, true) == ROUNDIE_POWER_TURN_SCREEN_ON);
    assert(hold_button(&power, &now, false) == ROUNDIE_POWER_NO_ACTION);
    /* The same when the GUI parked before it saw BOOT's release: the press
     * that turned the screen on was still the debounced level at sleep. */
    assert(hold_button(&power, &now, true) == ROUNDIE_POWER_TOGGLE_MENU);
    roundie_power_sleep_entered(&power);
    roundie_power_woke(&power, now, false);
    assert(hold_button(&power, &now, false) == ROUNDIE_POWER_NO_ACTION);
    assert(hold_button(&power, &now, true) == ROUNDIE_POWER_TURN_SCREEN_ON);
    assert(hold_button(&power, &now, false) == ROUNDIE_POWER_NO_ACTION);
    puts("sleep and wake: PASS");
}

static void sleep_admission(void)
{
    roundie_power_conditions_t ready = {
        .safe = false, .stable = true, .ota_idle = true,
        .runtime_busy = false, .wake_source_active = false,
    };
    assert(roundie_power_sleep_refusal(&ready) == ROUNDIE_POWER_ALLOWED);
    roundie_power_conditions_t c = ready;
    c.safe = true;
    c.stable = false;
    assert(roundie_power_sleep_refusal(&c) == ROUNDIE_POWER_REFUSED_SAFE_MODE);
    c = ready;
    c.stable = false;
    assert(roundie_power_sleep_refusal(&c) == ROUNDIE_POWER_REFUSED_NOT_STABLE);
    c = ready;
    c.ota_idle = false;
    assert(roundie_power_sleep_refusal(&c) == ROUNDIE_POWER_REFUSED_OTA);
    c = ready;
    c.runtime_busy = true;
    assert(roundie_power_sleep_refusal(&c) == ROUNDIE_POWER_REFUSED_APP);
    c = ready;
    c.wake_source_active = true;
    assert(roundie_power_sleep_refusal(&c) == ROUNDIE_POWER_REFUSED_WAKE_SOURCE);
    for (int refusal = ROUNDIE_POWER_ALLOWED; refusal <= ROUNDIE_POWER_REFUSED_WAKE_SOURCE; refusal++) {
        assert(roundie_power_refusal_name((roundie_power_refusal_t)refusal)[0] != 'u');
    }
    puts("sleep admission: PASS");
}

int main(void)
{
    timeout_and_touch_wake();
    keep_display_and_disabled_timeout();
    boot_button();
    sleep_and_wake();
    sleep_admission();
    return 0;
}
