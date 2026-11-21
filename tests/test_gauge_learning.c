/* Capacity learning tracker, load hold and learned-FCC record
 * (components/roundie_board/gauge_learning.c). Scenarios follow TI SLUUBD4A
 * 1.1.3: a qualified discharge sets VDQ, EDV2 ends it, FCC is updated within
 * 4 s; charge, low temperature, low current or a gauge reset disqualify. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "roundie_gauge_learning.h"

#define VDQ ROUNDIE_GAUGE_OP_VDQ
#define EDV2 ROUNDIE_GAUGE_OP_EDV2
#define INIT ROUNDIE_GAUGE_OP_INITCOMP
#define SECOND INT64_C(1000000)

static const roundie_gauge_learning_config_t config = {.learning_low_temp_deci_c = 119, .overload_ma = 1500};

typedef struct {
    roundie_gauge_learning_t tracker;
    roundie_gauge_hold_t hold;
    roundie_gauge_learning_sample_t sample;
    bool held;
    bool usb; /* a USB host link in the next sample */
} rig_t;

static void rig_init(rig_t *rig)
{
    memset(rig, 0, sizeof(*rig));
    roundie_gauge_learning_init(&rig->tracker);
    rig->sample.valid = true;
    rig->sample.initialized = true;
    rig->sample.operation_flags = INIT;
    rig->sample.battery_flags = 0x0008;
    rig->sample.voltage_mv = 4150;
    rig->sample.current_ma = 0;
    rig->sample.temperature_deci_c = 250;
    rig->sample.remaining_mah = 590;
    rig->sample.full_mah = 600;
    rig->sample.design_mah = 600;
}

/* One sample 5 s after the previous one. */
static roundie_gauge_learn_result_t step(rig_t *rig)
{
    rig->sample.time_us += 5 * SECOND;
    roundie_gauge_learn_result_t result = roundie_gauge_learning_update(&rig->tracker, &config, &rig->sample);
    roundie_gauge_hold_supply(&rig->hold, &rig->sample, rig->usb, rig->sample.time_us);
    rig->held = roundie_gauge_hold_update(&rig->hold, &rig->sample, result, rig->sample.time_us);
    return result;
}

/* Charged and armed, then a qualified discharge down to EDV2. */
static void discharge_to_edv2(rig_t *rig, int16_t current_ma)
{
    assert(step(rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    assert(roundie_gauge_hold_arm(&rig->hold, rig->sample.time_us, 10 * 3600 * SECOND));
    rig->sample.current_ma = current_ma;
    rig->sample.operation_flags = INIT | VDQ;
    assert(step(rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    assert(rig->tracker.phase == ROUNDIE_GAUGE_LEARN_QUALIFIED && !rig->tracker.resumed);
    assert(rig->tracker.start_remaining_mah == 590 && rig->tracker.qualified_count == 1);
    for (int i = 0; i < 20; i++) {
        rig->sample.voltage_mv = (uint16_t)(rig->sample.voltage_mv - 30);
        rig->sample.remaining_mah = (uint16_t)(rig->sample.remaining_mah - 25);
        assert(step(rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
        assert(rig->held);
    }
    rig->sample.voltage_mv = 3560;
    rig->sample.remaining_mah = 42;
    rig->sample.operation_flags = INIT | VDQ | EDV2;
}

static void test_min_current(void)
{
    /* 3C/32: 56.25 mA for 600 mAh, met from 57 mA; 281.25 for 3000. */
    assert(roundie_gauge_learning_min_current_ma(600) == 57);
    assert(roundie_gauge_learning_min_current_ma(3000) == 282);
    assert(roundie_gauge_learning_min_current_ma(32) == 3);
}

static void test_learning_updates_fcc(void)
{
    rig_t rig;
    rig_init(&rig);
    discharge_to_edv2(&rig, -90);
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    assert(rig.tracker.phase == ROUNDIE_GAUGE_LEARN_EDV2 && rig.tracker.edv2_qualified);
    assert(rig.tracker.edv2_voltage_mv == 3560 && rig.tracker.edv2_current_ma == -90);
    /* EDV2 releases the load at once and keeps a snapshot. */
    assert(!rig.held && rig.hold.state == ROUNDIE_GAUGE_HOLD_EDV2);
    assert(rig.hold.edv2_seen && rig.hold.edv2.voltage_mv == 3560 && rig.hold.edv2.current_ma == -90);
    assert(rig.hold.edv2.remaining_mah == 42 && rig.hold.edv2.full_mah == 600);
    assert(rig.hold.protect && rig.hold.alert == ROUNDIE_GAUGE_ALERT_FINISHED && !rig.hold.learned);
    /* No protective sleep while the result is awaited. */
    assert(!roundie_gauge_hold_sleep_wanted(&rig.hold));
    rig.sample.full_mah = 572;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_UPDATED);
    assert(rig.tracker.result_old_full_mah == 600 && rig.tracker.result_new_full_mah == 572);
    assert(rig.tracker.cause == ROUNDIE_GAUGE_LEARN_CAUSE_NONE && rig.tracker.updated_count == 1);
    assert(rig.tracker.phase == ROUNDIE_GAUGE_LEARN_IDLE);
    assert(!rig.held && rig.hold.state == ROUNDIE_GAUGE_HOLD_DONE);
    assert(rig.hold.learned && rig.hold.result == ROUNDIE_GAUGE_LEARN_RESULT_UPDATED);
    assert(rig.hold.protect && rig.hold.alert == ROUNDIE_GAUGE_ALERT_FINISHED);
    assert(rig.hold.edv2.full_mah == 600); /* the snapshot is the EDV2 sample */
    assert(roundie_gauge_hold_sleep_wanted(&rig.hold));
    /* VDQ still set afterwards does not start a new discharge. */
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    assert(rig.tracker.phase == ROUNDIE_GAUGE_LEARN_IDLE && rig.tracker.qualified_count == 1);
    /* Discharge statistics: 21 samples at -90 mA, the EDV2 ones included. */
    assert(rig.tracker.discharge_samples >= 21);
    assert(rig.tracker.discharge_smallest_ma == -90 && rig.tracker.discharge_largest_ma == -90);
    /* Protection ends only when the battery is seen charging. */
    rig.sample.current_ma = 0;
    step(&rig);
    assert(rig.hold.protect);
    rig.sample.current_ma = 250;
    step(&rig);
    assert(!rig.hold.protect && rig.hold.alert == ROUNDIE_GAUGE_ALERT_NONE);
    assert(rig.hold.learned && rig.hold.state == ROUNDIE_GAUGE_HOLD_DONE);
    assert(!roundie_gauge_hold_sleep_wanted(&rig.hold));
}

static void test_charge_aborts(void)
{
    rig_t rig;
    rig_init(&rig);
    discharge_to_edv2(&rig, -90);
    /* USB reconnected before EDV2: the gauge clears VDQ after 10 mAh. */
    rig.sample.operation_flags = INIT;
    rig.sample.current_ma = 250;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_ABORTED);
    assert(rig.tracker.cause == ROUNDIE_GAUGE_LEARN_CAUSE_CHARGE && rig.tracker.aborted_count == 1);
    assert(!rig.held && rig.hold.state == ROUNDIE_GAUGE_HOLD_ABORTED);
    assert(!rig.hold.learned && rig.hold.alert == ROUNDIE_GAUGE_ALERT_STOPPED);
}

static void test_low_temperature_aborts(void)
{
    rig_t rig;
    rig_init(&rig);
    discharge_to_edv2(&rig, -90);
    rig.sample.operation_flags = INIT;
    rig.sample.temperature_deci_c = 100;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_ABORTED);
    assert(rig.tracker.cause == ROUNDIE_GAUGE_LEARN_CAUSE_LOW_TEMP);
}

static void test_config_update_and_unknown(void)
{
    rig_t rig;
    rig_init(&rig);
    discharge_to_edv2(&rig, -90);
    rig.sample.operation_flags = INIT | ROUNDIE_GAUGE_OP_CFGUPDATE;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_ABORTED);
    assert(rig.tracker.cause == ROUNDIE_GAUGE_LEARN_CAUSE_CONFIG_UPDATE);

    rig_init(&rig);
    discharge_to_edv2(&rig, -90);
    rig.sample.operation_flags = INIT;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_ABORTED);
    assert(rig.tracker.cause == ROUNDIE_GAUGE_LEARN_CAUSE_UNKNOWN);
}

/* Screen off and light sleep: too little current at EDV2. */
static void test_low_current_at_edv2(void)
{
    rig_t rig;
    rig_init(&rig);
    discharge_to_edv2(&rig, -30);
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    assert(rig.tracker.phase == ROUNDIE_GAUGE_LEARN_EDV2);
    /* No FCC change within the window: unchanged, low current reported. */
    for (int i = 0; i < 11; i++) {
        assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    }
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_UNCHANGED);
    assert(rig.tracker.cause == ROUNDIE_GAUGE_LEARN_CAUSE_LOW_CURRENT && rig.tracker.unchanged_count == 1);
    assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_DONE && !rig.hold.learned);
    assert(rig.hold.result == ROUNDIE_GAUGE_LEARN_RESULT_UNCHANGED && rig.hold.protect);

    /* VDQ already cleared when EDV2 is seen: aborted, same cause. */
    rig_init(&rig);
    discharge_to_edv2(&rig, -30);
    rig.sample.operation_flags = INIT | EDV2;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_ABORTED);
    assert(rig.tracker.cause == ROUNDIE_GAUGE_LEARN_CAUSE_LOW_CURRENT);

    /* Overload at EDV2. */
    rig_init(&rig);
    discharge_to_edv2(&rig, -1600);
    rig.sample.operation_flags = INIT | EDV2;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_ABORTED);
    assert(rig.tracker.cause == ROUNDIE_GAUGE_LEARN_CAUSE_OVERLOAD);
}

static void test_charge_after_edv2(void)
{
    rig_t rig;
    rig_init(&rig);
    discharge_to_edv2(&rig, -90);
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    rig.sample.current_ma = 300;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_ABORTED);
    assert(rig.tracker.cause == ROUNDIE_GAUGE_LEARN_CAUSE_CHARGE);
}

/* Battery disconnected or gauge POR: INITCOMP lost or Design Capacity back
 * at the OTP default. */
static void test_gauge_reset_aborts(void)
{
    rig_t rig;
    rig_init(&rig);
    discharge_to_edv2(&rig, -90);
    rig.sample.operation_flags = VDQ;
    rig.sample.initialized = false;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_ABORTED);
    assert(rig.tracker.cause == ROUNDIE_GAUGE_LEARN_CAUSE_GAUGE_RESET);

    rig_init(&rig);
    discharge_to_edv2(&rig, -90);
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    rig.sample.design_mah = 3000;
    rig.sample.full_mah = 3000;
    rig.sample.operation_flags = INIT;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_ABORTED);
    assert(rig.tracker.cause == ROUNDIE_GAUGE_LEARN_CAUSE_GAUGE_RESET);
    /* A reset is not a learned FCC. */
    assert(rig.tracker.updated_count == 0);
}

/* MCU restart mid-discharge: the new tracker follows the gauge's VDQ; flags
 * already set are not transitions. */
static void test_mcu_restart(void)
{
    rig_t rig;
    rig_init(&rig);
    rig.sample.operation_flags = INIT | VDQ;
    rig.sample.current_ma = -90;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    assert(rig.tracker.phase == ROUNDIE_GAUGE_LEARN_QUALIFIED && rig.tracker.resumed);
    rig.sample.operation_flags = INIT | VDQ | EDV2;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    rig.sample.full_mah = 580;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_UPDATED);
    /* The hold is RAM only: after the restart nothing arms it, so the
     * resumed discharge gets no forced load, alert or protection. */
    assert(!rig.held && rig.hold.state == ROUNDIE_GAUGE_HOLD_OFF);
    assert(!rig.hold.protect && rig.hold.alert == ROUNDIE_GAUGE_ALERT_NONE && !rig.hold.learned);

    /* Restarted after EDV2 was already set: no transition, no result. */
    rig_init(&rig);
    rig.sample.operation_flags = INIT | VDQ | EDV2;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    rig.sample.full_mah = 580;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    assert(!rig.tracker.edv2_seen && rig.tracker.updated_count == 0);
    assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_OFF && !rig.hold.protect);
}

static void test_edv2_without_vdq(void)
{
    rig_t rig;
    rig_init(&rig);
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    rig.sample.current_ma = -90;
    rig.sample.operation_flags = INIT | EDV2;
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_ABORTED);
    assert(rig.tracker.cause == ROUNDIE_GAUGE_LEARN_CAUSE_NOT_QUALIFIED && !rig.tracker.edv2_qualified);
}

static void test_invalid_samples(void)
{
    rig_t rig;
    rig_init(&rig);
    discharge_to_edv2(&rig, -90);
    rig.sample.valid = false;
    rig.sample.operation_flags = 0;
    for (int i = 0; i < ROUNDIE_GAUGE_HOLD_MAX_MISSED - 1; i++) {
        assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
        assert(rig.held);
    }
    /* Tracking is unaffected by unreadable samples. */
    assert(rig.tracker.phase == ROUNDIE_GAUGE_LEARN_QUALIFIED);
    assert(step(&rig) == ROUNDIE_GAUGE_LEARN_RESULT_NONE);
    assert(!rig.held && rig.hold.state == ROUNDIE_GAUGE_HOLD_NO_DATA);
    assert(rig.hold.protect && rig.hold.alert == ROUNDIE_GAUGE_ALERT_STOPPED && !rig.hold.learned);
}

static void test_hold_limits(void)
{
    rig_t rig;
    roundie_gauge_hold_t idle = {0};
    rig_init(&rig);
    /* Never armed: no hold. */
    assert(!roundie_gauge_hold_update(&idle, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE, 0));
    assert(!roundie_gauge_hold_arm(&idle, 0, 0));
    assert(!roundie_gauge_hold_arm(&idle, 0, ROUNDIE_GAUGE_HOLD_MAX_US + 1));
    assert(idle.state == ROUNDIE_GAUGE_HOLD_OFF);

    /* Low voltage counts only while discharging. */
    rig_init(&rig);
    assert(roundie_gauge_hold_arm(&rig.hold, 0, 3600 * SECOND));
    rig.sample.voltage_mv = ROUNDIE_GAUGE_HOLD_FLOOR_MV;
    rig.sample.current_ma = 200;
    step(&rig);
    assert(rig.held);
    rig.sample.current_ma = -90;
    step(&rig);
    assert(!rig.held && rig.hold.state == ROUNDIE_GAUGE_HOLD_LOW_VOLTAGE);
    /* Ended stays ended. */
    rig.sample.voltage_mv = 4000;
    step(&rig);
    assert(!rig.held);

    rig_init(&rig);
    assert(roundie_gauge_hold_arm(&rig.hold, 0, 3600 * SECOND));
    rig.sample.battery_flags |= ROUNDIE_GAUGE_BAT_SYSDWN;
    step(&rig);
    assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_SYSDOWN);

    rig_init(&rig);
    assert(roundie_gauge_hold_arm(&rig.hold, 0, 3600 * SECOND));
    rig.sample.battery_flags |= ROUNDIE_GAUGE_BAT_FD;
    step(&rig);
    assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_SYSDOWN);

    rig_init(&rig);
    assert(roundie_gauge_hold_arm(&rig.hold, 0, 3600 * SECOND));
    rig.sample.temperature_deci_c = ROUNDIE_GAUGE_HOLD_MAX_DECI_C;
    step(&rig);
    assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_HOT);

    rig_init(&rig);
    assert(roundie_gauge_hold_arm(&rig.hold, 0, 3600 * SECOND));
    rig.sample.temperature_deci_c = ROUNDIE_GAUGE_HOLD_MIN_DECI_C;
    step(&rig);
    assert(rig.held);
    rig.sample.temperature_deci_c = ROUNDIE_GAUGE_HOLD_MIN_DECI_C - 1;
    step(&rig);
    assert(!rig.held && rig.hold.state == ROUNDIE_GAUGE_HOLD_COLD && rig.hold.protect);

    rig_init(&rig);
    assert(roundie_gauge_hold_arm(&rig.hold, 0, 12 * SECOND));
    step(&rig);
    assert(rig.held);
    step(&rig);
    assert(rig.held);
    step(&rig);
    assert(!rig.held && rig.hold.state == ROUNDIE_GAUGE_HOLD_EXPIRED);
    /* A plain expiry far from the end needs no protection. */
    assert(!rig.hold.protect && rig.hold.alert == ROUNDIE_GAUGE_ALERT_NONE);

    rig_init(&rig);
    assert(roundie_gauge_hold_arm(&rig.hold, 0, 3600 * SECOND));
    roundie_gauge_hold_cancel(&rig.hold);
    step(&rig);
    assert(!rig.held && rig.hold.state == ROUNDIE_GAUGE_HOLD_CANCELLED);
    /* Cancelling an ended hold keeps its reason. */
    rig.hold.state = ROUNDIE_GAUGE_HOLD_DONE;
    roundie_gauge_hold_cancel(&rig.hold);
    assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_DONE);
}

/* The end of a cycle: warning, EDV2 without a result, safety stops after
 * EDV2, expiry near the end, cancel, re-arm. */
static void test_end_of_cycle(void)
{
    rig_t rig;

    /* Warning by remaining capacity (15 % of FCC), only while discharging. */
    rig_init(&rig);
    assert(roundie_gauge_hold_arm(&rig.hold, 0, 3600 * SECOND));
    rig.sample.remaining_mah = 90;
    rig.sample.current_ma = 100;
    step(&rig);
    assert(!rig.hold.warned && rig.hold.alert == ROUNDIE_GAUGE_ALERT_NONE);
    rig.sample.current_ma = -100;
    rig.sample.remaining_mah = 91;
    step(&rig);
    assert(!rig.hold.warned);
    rig.sample.remaining_mah = 90;
    step(&rig);
    assert(rig.held && rig.hold.warned && rig.hold.alert == ROUNDIE_GAUGE_ALERT_NEAR_END);
    assert(rig.hold.warned_us == rig.sample.time_us && !rig.hold.protect);
    assert(!roundie_gauge_hold_sleep_wanted(&rig.hold));
    /* The warning keeps the load: it is not an end. */
    step(&rig);
    assert(rig.held && rig.hold.state == ROUNDIE_GAUGE_HOLD_RUNNING);

    /* Warning by voltage. */
    rig_init(&rig);
    assert(roundie_gauge_hold_arm(&rig.hold, 0, 3600 * SECOND));
    rig.sample.current_ma = -100;
    rig.sample.voltage_mv = ROUNDIE_GAUGE_HOLD_WARN_MV + 1;
    step(&rig);
    assert(!rig.hold.warned);
    rig.sample.voltage_mv = ROUNDIE_GAUGE_HOLD_WARN_MV;
    step(&rig);
    assert(rig.hold.warned && rig.held);
    /* Expiry after the warning protects and alerts. */
    rig.sample.time_us = 3600 * SECOND;
    step(&rig);
    assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_EXPIRED && rig.hold.protect);
    assert(rig.hold.alert == ROUNDIE_GAUGE_ALERT_STOPPED && !rig.hold.learned);

    /* EDV2 with no result (tracker unqualified): NO_RESULT after the wait. */
    rig_init(&rig);
    roundie_gauge_hold_t *hold = &rig.hold;
    assert(roundie_gauge_hold_arm(hold, 0, 3600 * SECOND));
    rig.sample.current_ma = -100;
    rig.sample.operation_flags = INIT | EDV2;
    assert(!roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE, 10 * SECOND));
    assert(hold->state == ROUNDIE_GAUGE_HOLD_EDV2 && hold->edv2_seen);
    assert(!roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE,
                                      10 * SECOND + ROUNDIE_GAUGE_HOLD_EDV2_WAIT_US - 1));
    assert(hold->state == ROUNDIE_GAUGE_HOLD_EDV2);
    assert(!roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE,
                                      10 * SECOND + ROUNDIE_GAUGE_HOLD_EDV2_WAIT_US));
    assert(hold->state == ROUNDIE_GAUGE_HOLD_NO_RESULT && hold->protect && !hold->learned);
    assert(roundie_gauge_hold_sleep_wanted(hold));
    assert(hold->alert == ROUNDIE_GAUGE_ALERT_STOPPED);
    /* The hold deadline does not end the EDV2 wait early, nor does EDV2
     * while charging arm anything. */
    rig_init(&rig);
    assert(roundie_gauge_hold_arm(hold, 0, 3600 * SECOND));
    rig.sample.current_ma = 50;
    rig.sample.operation_flags = INIT | EDV2;
    assert(roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE, SECOND));
    assert(hold->state == ROUNDIE_GAUGE_HOLD_RUNNING && !hold->edv2_seen);

    /* A safety stop after EDV2 keeps the snapshot; a later UPDATED is not
     * declared as learned. */
    rig_init(&rig);
    assert(roundie_gauge_hold_arm(hold, 0, 3600 * SECOND));
    rig.sample.current_ma = -100;
    rig.sample.voltage_mv = 3400;
    rig.sample.operation_flags = INIT | EDV2;
    roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE, SECOND);
    assert(hold->state == ROUNDIE_GAUGE_HOLD_EDV2);
    rig.sample.voltage_mv = 3300;
    roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE, 2 * SECOND);
    assert(hold->state == ROUNDIE_GAUGE_HOLD_LOW_VOLTAGE && hold->edv2.voltage_mv == 3400);
    roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_UPDATED, 3 * SECOND);
    assert(hold->state == ROUNDIE_GAUGE_HOLD_LOW_VOLTAGE && !hold->learned);
    assert(hold->alert == ROUNDIE_GAUGE_ALERT_STOPPED && hold->protect);
    /* Each safety stop: SYSDWN, FD, hot, cold, missing samples. */
    static const uint16_t battery[] = {ROUNDIE_GAUGE_BAT_SYSDWN, ROUNDIE_GAUGE_BAT_FD};
    for (unsigned i = 0; i < 2; i++) {
        rig_init(&rig);
        assert(roundie_gauge_hold_arm(hold, 0, 3600 * SECOND));
        rig.sample.current_ma = -100;
        rig.sample.operation_flags = INIT | EDV2;
        roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE, SECOND);
        rig.sample.battery_flags |= battery[i];
        roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_UPDATED, 2 * SECOND);
        /* The stop wins over a result in the same sample: no success. */
        assert(hold->state == ROUNDIE_GAUGE_HOLD_SYSDOWN && !hold->learned);
        assert(hold->result == ROUNDIE_GAUGE_LEARN_RESULT_NONE && hold->edv2_seen);
        rig_init(&rig);
        assert(roundie_gauge_hold_arm(hold, 0, 3600 * SECOND));
        rig.sample.current_ma = -100;
        rig.sample.operation_flags = INIT | EDV2;
        roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE, SECOND);
        rig.sample.battery_flags |= battery[i];
        roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE, 2 * SECOND);
        assert(hold->state == ROUNDIE_GAUGE_HOLD_SYSDOWN && !hold->learned && hold->protect);
        roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_UPDATED, 3 * SECOND);
        assert(!hold->learned);
    }

    /* Cancel ends protection and the alert. */
    rig_init(&rig);
    assert(roundie_gauge_hold_arm(hold, 0, 3600 * SECOND));
    rig.sample.current_ma = -100;
    rig.sample.operation_flags = INIT | EDV2;
    roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE, SECOND);
    roundie_gauge_hold_cancel(hold);
    assert(hold->state == ROUNDIE_GAUGE_HOLD_CANCELLED && !hold->protect);
    assert(hold->alert == ROUNDIE_GAUGE_ALERT_NONE && hold->edv2_seen);
    /* Re-arming starts clean. */
    assert(roundie_gauge_hold_arm(hold, 5 * SECOND, 3600 * SECOND));
    assert(!hold->edv2_seen && !hold->warned && !hold->learned && hold->missed == 0);
    assert(hold->result == ROUNDIE_GAUGE_LEARN_RESULT_NONE);

    /* After an MCU reset the hold is OFF: an EDV2 or low-voltage sample
     * neither arms a load nor raises an alert or protection. */
    roundie_gauge_hold_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    rig_init(&rig);
    rig.sample.current_ma = -60;
    rig.sample.voltage_mv = 3290;
    rig.sample.operation_flags = INIT | VDQ | EDV2;
    assert(!roundie_gauge_hold_update(&fresh, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_UPDATED, SECOND));
    assert(fresh.state == ROUNDIE_GAUGE_HOLD_OFF && !fresh.protect);
    assert(fresh.alert == ROUNDIE_GAUGE_ALERT_NONE && !fresh.learned && !fresh.edv2_seen);
    assert(!roundie_gauge_hold_sleep_wanted(&fresh));
}

/* Arm a hold and a test on USB, then unplug: two discharging samples
 * without a link start the countdown. */
static void start_test(rig_t *rig, roundie_gauge_test_t test, int64_t delay_us)
{
    rig_init(rig);
    rig->usb = true;
    assert(roundie_gauge_hold_arm(&rig->hold, 0, 3600 * SECOND));
    assert(roundie_gauge_hold_test(&rig->hold, test, delay_us));
    rig->sample.current_ma = -105;
    rig->usb = false;
    step(rig);
    assert(!rig->hold.test_started && rig->hold.test_battery == 1);
    step(rig);
    assert(rig->hold.test_started && rig->hold.test_started_us == rig->sample.time_us);
}

/* Development test trigger: the end path without discharging. Samples stay
 * real (full charge, no VDQ/EDV2), the tracker sees nothing, nothing is
 * learned. The countdown starts only on battery power. */
static void test_simulated_end(void)
{
    rig_t rig;
    rig_init(&rig);
    /* Only on a running hold, one test at a time, bounded delay. */
    assert(!roundie_gauge_hold_test(&rig.hold, ROUNDIE_GAUGE_TEST_END, 0));
    assert(roundie_gauge_hold_arm(&rig.hold, 0, 3600 * SECOND));
    assert(!roundie_gauge_hold_test(&rig.hold, ROUNDIE_GAUGE_TEST_NONE, 0));
    assert(!roundie_gauge_hold_test(&rig.hold, ROUNDIE_GAUGE_TEST_END, -1));
    assert(!roundie_gauge_hold_test(&rig.hold, ROUNDIE_GAUGE_TEST_END, ROUNDIE_GAUGE_TEST_MAX_DELAY_US + 1));
    assert(roundie_gauge_hold_test(&rig.hold, ROUNDIE_GAUGE_TEST_END, 10 * SECOND));
    assert(!roundie_gauge_hold_test(&rig.hold, ROUNDIE_GAUGE_TEST_STOP, 0));
    /* Armed on USB: no countdown, however long, with or without current. */
    rig.usb = true;
    for (int i = 0; i < 100; i++) {
        rig.sample.current_ma = (int16_t)(i % 2 == 0 ? 0 : -105);
        step(&rig);
        assert(rig.held && !rig.hold.test_started && !rig.hold.warned && !rig.hold.simulated);
    }
    /* No link but no discharge (or an invalid sample): not battery yet. */
    rig.usb = false;
    rig.sample.current_ma = 0;
    step(&rig);
    assert(!rig.hold.test_started && rig.hold.test_battery == 0);
    rig.sample.current_ma = -105;
    step(&rig);
    assert(rig.hold.test_battery == 1);
    rig.sample.valid = false;
    step(&rig);
    assert(!rig.hold.test_started && rig.hold.test_battery == 0);
    rig.sample.valid = true;
    rig.usb = true;
    step(&rig);
    rig.usb = false;
    step(&rig);
    assert(!rig.hold.test_started && rig.hold.test_battery == 1);
    step(&rig); /* the second consecutive battery sample starts it */
    assert(rig.hold.test_started && !rig.hold.warned);
    const int64_t started = rig.sample.time_us;
    assert(rig.hold.test_warn_us == started + 10 * SECOND);
    assert(rig.hold.test_end_us == started + 10 * SECOND + ROUNDIE_GAUGE_TEST_GAP_US);
    step(&rig); /* +5 s */
    assert(rig.held && !rig.hold.warned && !rig.hold.simulated);
    step(&rig); /* +10 s: warning */
    assert(rig.held && rig.hold.warned && rig.hold.simulated);
    assert(rig.hold.alert == ROUNDIE_GAUGE_ALERT_NEAR_END && !rig.hold.protect);
    for (int i = 0; i < 11; i++) {
        step(&rig);
        assert(rig.held);
    }
    step(&rig); /* +70 s: simulated EDV2, load released */
    assert(!rig.held && rig.hold.state == ROUNDIE_GAUGE_HOLD_EDV2);
    assert(rig.hold.edv2_seen && rig.hold.edv2.voltage_mv == 4150 && rig.hold.edv2.full_mah == 600);
    assert((rig.hold.edv2.operation_flags & EDV2) == 0); /* the real flags, not faked */
    assert(rig.hold.alert == ROUNDIE_GAUGE_ALERT_FINISHED && rig.hold.protect);
    assert(!roundie_gauge_hold_sleep_wanted(&rig.hold));
    assert(rig.tracker.phase == ROUNDIE_GAUGE_LEARN_IDLE && rig.tracker.updated_count == 0);
    for (int i = 0; i < 23; i++) {
        step(&rig);
        assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_EDV2);
    }
    step(&rig); /* 120 s later: a test end, not NO_RESULT, not learned */
    assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_TEST_END && !rig.hold.learned);
    assert(rig.hold.result == ROUNDIE_GAUGE_LEARN_RESULT_NONE && rig.hold.alert == ROUNDIE_GAUGE_ALERT_FINISHED);
    assert(roundie_gauge_hold_sleep_wanted(&rig.hold));
    /* A charging sample ends the protection. */
    roundie_gauge_hold_t copy = rig.hold;
    rig.sample.current_ma = 300;
    step(&rig);
    assert(!rig.hold.protect && rig.hold.alert == ROUNDIE_GAUGE_ALERT_NONE);
    assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_TEST_END && rig.tracker.updated_count == 0);
    /* So does a USB link with a full battery (no charging current); a later
     * unplug does not restart anything. */
    rig.hold = copy;
    rig.usb = true;
    rig.sample.current_ma = 0;
    step(&rig);
    assert(!rig.hold.protect && rig.hold.alert == ROUNDIE_GAUGE_ALERT_NONE);
    rig.usb = false;
    rig.sample.current_ma = -60;
    for (int i = 0; i < 100; i++) {
        step(&rig);
        assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_TEST_END && !rig.hold.protect);
        assert(!roundie_gauge_hold_sleep_wanted(&rig.hold));
    }
    /* external_power alone leaves a running hold and the EDV2 wait alone. */
    roundie_gauge_hold_t waiting = {.state = ROUNDIE_GAUGE_HOLD_EDV2, .protect = true,
                                    .alert = ROUNDIE_GAUGE_ALERT_FINISHED};
    roundie_gauge_hold_external_power(&waiting);
    assert(waiting.protect && waiting.alert == ROUNDIE_GAUGE_ALERT_FINISHED);
    roundie_gauge_hold_t running = {.state = ROUNDIE_GAUGE_HOLD_RUNNING, .alert = ROUNDIE_GAUGE_ALERT_NEAR_END};
    roundie_gauge_hold_external_power(&running);
    assert(running.alert == ROUNDIE_GAUGE_ALERT_NEAR_END);

    /* USB back before the warning, after it, or in the simulated EDV2 wait:
     * the test ends as TEST_USB (load released, no protection or alert) and
     * a later unplug does not resume it. A charging sample alone counts. */
    for (int when = 0; when < 4; when++) {
        start_test(&rig, ROUNDIE_GAUGE_TEST_END, 10 * SECOND);
        const int steps[] = {1, 3, 15, 15};
        for (int i = 0; i < steps[when]; i++) {
            step(&rig);
        }
        if (when == 3) {
            rig.sample.current_ma = 250; /* charger without a host */
        } else {
            rig.usb = true;
            rig.sample.current_ma = 0;
        }
        step(&rig);
        assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_TEST_USB && !rig.held);
        assert(!rig.hold.protect && rig.hold.alert == ROUNDIE_GAUGE_ALERT_NONE && !rig.hold.learned);
        assert(rig.hold.ended_us == rig.sample.time_us);
        rig.usb = false;
        rig.sample.current_ma = -105;
        for (int i = 0; i < 100; i++) {
            step(&rig);
            assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_TEST_USB && !rig.held && !rig.hold.protect);
            assert(rig.hold.alert == ROUNDIE_GAUGE_ALERT_NONE);
        }
        assert(!roundie_gauge_hold_test(&rig.hold, ROUNDIE_GAUGE_TEST_END, 0)); /* one-shot */
    }

    /* Timeout: armed but never on battery, the hold expires unsimulated. */
    rig_init(&rig);
    rig.usb = true;
    assert(roundie_gauge_hold_arm(&rig.hold, 0, 60 * SECOND));
    assert(roundie_gauge_hold_test(&rig.hold, ROUNDIE_GAUGE_TEST_END, 0));
    for (int i = 0; i < 12; i++) {
        step(&rig);
    }
    assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_EXPIRED && !rig.hold.test_started);
    assert(!rig.hold.simulated && !rig.hold.protect && rig.hold.alert == ROUNDIE_GAUGE_ALERT_NONE);
    rig.usb = false;
    rig.sample.current_ma = -105;
    for (int i = 0; i < 40; i++) {
        step(&rig);
        assert(rig.hold.state == ROUNDIE_GAUGE_HOLD_EXPIRED && !rig.hold.test_started);
    }

    /* A real UPDATED during a simulated wait is not credited either. */
    start_test(&rig, ROUNDIE_GAUGE_TEST_END, 0);
    roundie_gauge_hold_t *hold = &rig.hold;
    int64_t t0 = rig.sample.time_us;
    roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE, t0 + ROUNDIE_GAUGE_TEST_GAP_US);
    assert(hold->state == ROUNDIE_GAUGE_HOLD_EDV2 && hold->simulated);
    roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_UPDATED,
                              t0 + ROUNDIE_GAUGE_TEST_GAP_US + SECOND);
    assert(hold->state == ROUNDIE_GAUGE_HOLD_DONE && !hold->learned);

    /* Simulated safety stop: protection and the stop alert, no snapshot. */
    start_test(&rig, ROUNDIE_GAUGE_TEST_STOP, 0);
    assert(hold->alert == ROUNDIE_GAUGE_ALERT_NEAR_END); /* warned in the start sample */
    t0 = rig.sample.time_us;
    assert(!roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE,
                                      t0 + ROUNDIE_GAUGE_TEST_GAP_US));
    assert(hold->state == ROUNDIE_GAUGE_HOLD_TEST_STOP && hold->protect && !hold->learned);
    assert(hold->alert == ROUNDIE_GAUGE_ALERT_STOPPED && !hold->edv2_seen);
    assert(roundie_gauge_hold_sleep_wanted(hold));

    /* A real safety condition still wins over a started simulation. */
    start_test(&rig, ROUNDIE_GAUGE_TEST_END, 30 * SECOND);
    rig.sample.temperature_deci_c = ROUNDIE_GAUGE_HOLD_MAX_DECI_C;
    roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE,
                              rig.sample.time_us + ROUNDIE_GAUGE_TEST_GAP_US + 30 * SECOND);
    assert(hold->state == ROUNDIE_GAUGE_HOLD_HOT && !hold->simulated);

    /* Cancel and re-arm clear the test; an MCU reset starts from OFF, where
     * nothing can fire. */
    start_test(&rig, ROUNDIE_GAUGE_TEST_END, 0);
    roundie_gauge_hold_cancel(hold);
    assert(!roundie_gauge_hold_update(hold, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE,
                                      rig.sample.time_us + 2 * ROUNDIE_GAUGE_TEST_GAP_US));
    assert(hold->state == ROUNDIE_GAUGE_HOLD_CANCELLED && hold->alert == ROUNDIE_GAUGE_ALERT_NONE);
    assert(roundie_gauge_hold_arm(hold, 0, 3600 * SECOND));
    assert(hold->test == ROUNDIE_GAUGE_TEST_NONE && !hold->simulated && !hold->test_started);
    roundie_gauge_hold_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    rig.sample.current_ma = -105;
    for (int i = 0; i < 4; i++) {
        roundie_gauge_hold_supply(&fresh, &rig.sample, false, INT64_C(1) << 40);
        assert(!roundie_gauge_hold_update(&fresh, &rig.sample, ROUNDIE_GAUGE_LEARN_RESULT_NONE, INT64_C(1) << 40));
    }
    assert(fresh.state == ROUNDIE_GAUGE_HOLD_OFF && fresh.alert == ROUNDIE_GAUGE_ALERT_NONE && !fresh.protect);
    assert(!fresh.test_started);
}

/* The battery test window: opens with the countdown, keeps the protect
 * point and the last battery sample, closes on external power for good. */
static void test_battery_window(void)
{
    rig_t rig;
    roundie_gauge_window_t window;
    memset(&window, 0, sizeof(window));
    roundie_gauge_power_counters_t power = {.sleeps = 7, .slept_us = 9 * SECOND, .timer_wakes = 6,
                                            .other_wakes = 1, .awake_us = 50 * SECOND, .awake_gaps = 6};
    rig_init(&rig);
    rig.usb = true;
    assert(roundie_gauge_hold_arm(&rig.hold, 0, 3600 * SECOND));
    assert(roundie_gauge_hold_test(&rig.hold, ROUNDIE_GAUGE_TEST_END, 0));
    step(&rig);
    roundie_gauge_window_update(&window, &rig.hold, &rig.sample, &power, rig.usb, rig.sample.time_us);
    assert(window.state == ROUNDIE_GAUGE_WINDOW_NONE); /* armed is not started */
    rig.usb = false;
    rig.sample.current_ma = -62;
    step(&rig);
    roundie_gauge_window_update(&window, &rig.hold, &rig.sample, &power, rig.usb, rig.sample.time_us);
    assert(window.state == ROUNDIE_GAUGE_WINDOW_NONE);
    step(&rig); /* started; warned at once (delay 0) */
    roundie_gauge_window_update(&window, &rig.hold, &rig.sample, &power, rig.usb, rig.sample.time_us);
    assert(window.state == ROUNDIE_GAUGE_WINDOW_OPEN && window.samples == 1);
    assert(window.start.time_us == rig.sample.time_us && window.start.remaining_mah == 590);
    assert(window.start.power.sleeps == 7 && !window.protect_seen);
    /* Run to the test end; then protective sleeps (counters advance). */
    uint16_t mv = 4100;
    while (rig.hold.state != ROUNDIE_GAUGE_HOLD_TEST_END) {
        rig.sample.voltage_mv = mv--;
        step(&rig);
        roundie_gauge_window_update(&window, &rig.hold, &rig.sample, &power, rig.usb, rig.sample.time_us);
    }
    assert(window.protect_seen && window.protect.time_us == rig.sample.time_us);
    assert(window.protect.power.sleeps == 7);
    for (int i = 0; i < 5; i++) {
        power.sleeps++;
        power.slept_us += 60 * SECOND;
        power.timer_wakes++;
        power.causes = 1U << 4;
        rig.sample.remaining_mah--;
        step(&rig);
        roundie_gauge_window_update(&window, &rig.hold, &rig.sample, &power, rig.usb, rig.sample.time_us);
    }
    power.sleeps++;
    power.other_wakes++;
    power.causes = 1U << 7;
    step(&rig);
    roundie_gauge_window_update(&window, &rig.hold, &rig.sample, &power, rig.usb, rig.sample.time_us);
    assert(window.causes_seen == ((1U << 4) | (1U << 7)));
    assert(window.voltage_max_mv == 4150 && window.voltage_min_mv == mv + 1);
    const roundie_gauge_window_point_t last = window.last;
    assert(last.power.sleeps == 13 && last.remaining_mah == 585);
    /* USB back: closed, the last point stays the last battery sample. */
    rig.usb = true;
    rig.sample.current_ma = 0;
    step(&rig);
    roundie_gauge_window_update(&window, &rig.hold, &rig.sample, &power, rig.usb, rig.sample.time_us);
    assert(window.state == ROUNDIE_GAUGE_WINDOW_CLOSED && window.last.time_us == last.time_us);
    rig.usb = false;
    rig.sample.current_ma = -60;
    for (int i = 0; i < 10; i++) {
        step(&rig);
        roundie_gauge_window_update(&window, &rig.hold, &rig.sample, &power, rig.usb, rig.sample.time_us);
    }
    assert(window.state == ROUNDIE_GAUGE_WINDOW_CLOSED && window.last.time_us == last.time_us);
    /* An invalid sample neither opens nor advances a window. */
    roundie_gauge_window_t other;
    memset(&other, 0, sizeof(other));
    start_test(&rig, ROUNDIE_GAUGE_TEST_END, 0);
    rig.sample.valid = false;
    roundie_gauge_window_update(&other, &rig.hold, &rig.sample, &power, false, rig.sample.time_us);
    assert(other.state == ROUNDIE_GAUGE_WINDOW_NONE);
}

static roundie_gauge_capacity_t profile(void)
{
    roundie_gauge_capacity_t state;
    memset(&state, 0, sizeof(state));
    state.gauging_config = 0x102a;
    state.design_voltage_mv = 3700;
    state.charging_voltage_mv = 4200;
    state.taper_current_ma = 100;
    state.near_full_mah = 200;
    state.battery_low_centi = 700;
    state.learning_low_temp_deci_c = 119;
    state.overload_ma = 1500;
    state.reserve_mah = 0;
    state.flag_config_a = 0x0c8c;
    state.flag_config_b = 0x8c;
    return state;
}

static void test_profile_id(void)
{
    roundie_gauge_capacity_t base = profile();
    const uint32_t id = roundie_gauge_profile_id(&base);
    /* Capacities, flags and progress are not part of the identity. */
    roundie_gauge_capacity_t other = base;
    other.design_mah = 600;
    other.dm_full_mah = 560;
    other.dm_design_mah = 600;
    other.battery_flags = 0x5028;
    other.operation_flags = 0x00a4;
    other.step = 7;
    other.written = true;
    assert(roundie_gauge_profile_id(&other) == id);
    /* Every learning parameter is. */
    uint16_t *fields16[] = {&other.gauging_config, &other.design_voltage_mv, &other.charging_voltage_mv,
                            &other.taper_current_ma, &other.near_full_mah, &other.battery_low_centi,
                            &other.overload_ma, &other.reserve_mah, &other.flag_config_a};
    for (size_t i = 0; i < sizeof(fields16) / sizeof(fields16[0]); i++) {
        other = base;
        *fields16[i] ^= 1;
        assert(roundie_gauge_profile_id(&other) != id);
    }
    other = base;
    other.flag_config_b ^= 1;
    assert(roundie_gauge_profile_id(&other) != id);
    other = base;
    other.learning_low_temp_deci_c ^= 1;
    assert(roundie_gauge_profile_id(&other) != id);
}

static void test_record(void)
{
    roundie_gauge_capacity_t state = profile();
    const uint32_t id = roundie_gauge_profile_id(&state);
    roundie_gauge_record_t record;
    roundie_gauge_record_make(&record, 600, id, 572, 1);
    assert(roundie_gauge_record_valid(&record, sizeof(record), 600, id));
    assert(roundie_gauge_record_restore_full(&record, sizeof(record), 600, id) == 572);
    /* Every corrupted byte is detected. */
    for (size_t i = 0; i < sizeof(record); i++) {
        roundie_gauge_record_t broken = record;
        ((uint8_t *)&broken)[i] ^= 0x10;
        assert(!roundie_gauge_record_valid(&broken, sizeof(broken), 600, id));
        assert(roundie_gauge_record_restore_full(&broken, sizeof(broken), 600, id) == 600);
    }
    /* Size, design capacity and profile identity. */
    assert(!roundie_gauge_record_valid(&record, sizeof(record) - 1, 600, id));
    assert(!roundie_gauge_record_valid(&record, 0, 600, id));
    assert(!roundie_gauge_record_valid(&record, sizeof(record), 650, id));
    assert(!roundie_gauge_record_valid(&record, sizeof(record), 600, id ^ 1U));
    assert(!roundie_gauge_record_valid(NULL, sizeof(record), 600, id));
    assert(roundie_gauge_record_restore_full(NULL, 0, 600, id) == 600);
    /* A well-formed record with an implausible capacity. */
    roundie_gauge_record_make(&record, 600, id, 299, 1);
    assert(!roundie_gauge_record_valid(&record, sizeof(record), 600, id));
    roundie_gauge_record_make(&record, 600, id, 751, 1);
    assert(!roundie_gauge_record_valid(&record, sizeof(record), 600, id));
    /* Plausibility bounds: design/2 .. design*5/4. */
    assert(roundie_gauge_full_acceptable(600, 300) && roundie_gauge_full_acceptable(600, 750));
    assert(!roundie_gauge_full_acceptable(600, 299) && !roundie_gauge_full_acceptable(600, 751));
    assert(!roundie_gauge_full_acceptable(0, 0));
}

static void test_save_policy(void)
{
    roundie_gauge_capacity_t state = profile();
    const uint32_t id = roundie_gauge_profile_id(&state);
    roundie_gauge_record_t stored;
    roundie_gauge_record_make(&stored, 600, id, 572, 3);
    /* No record, or a record that is not valid here: save. */
    assert(roundie_gauge_record_save_due(NULL, 0, 600, id, 572, 0));
    assert(roundie_gauge_record_save_due(&stored, sizeof(stored), 600, id ^ 1U, 572, 0));
    /* Less than 1 % of design (6 mAh) from the stored value: no write. */
    assert(!roundie_gauge_record_save_due(&stored, sizeof(stored), 600, id, 572, 0));
    assert(!roundie_gauge_record_save_due(&stored, sizeof(stored), 600, id, 577, 0));
    assert(!roundie_gauge_record_save_due(&stored, sizeof(stored), 600, id, 567, 0));
    assert(roundie_gauge_record_save_due(&stored, sizeof(stored), 600, id, 578, 0));
    assert(roundie_gauge_record_save_due(&stored, sizeof(stored), 600, id, 566, 0));
    /* Implausible values are never stored. */
    assert(!roundie_gauge_record_save_due(NULL, 0, 600, id, 200, 0));
    /* Per-boot limit. */
    assert(roundie_gauge_record_save_due(NULL, 0, 600, id, 572, ROUNDIE_GAUGE_RECORD_MAX_SAVES_PER_BOOT - 1));
    assert(!roundie_gauge_record_save_due(NULL, 0, 600, id, 572, ROUNDIE_GAUGE_RECORD_MAX_SAVES_PER_BOOT));
}

static void test_names(void)
{
    for (int i = 0; i <= ROUNDIE_GAUGE_LEARN_CAUSE_UNKNOWN; i++) {
        assert(strcmp(roundie_gauge_learn_cause_name((roundie_gauge_learn_cause_t)i), "?") != 0);
    }
    for (int i = 0; i <= ROUNDIE_GAUGE_HOLD_TEST_USB; i++) {
        assert(strcmp(roundie_gauge_hold_state_name((roundie_gauge_hold_state_t)i), "?") != 0);
    }
    for (int i = 0; i <= ROUNDIE_GAUGE_ALERT_STOPPED; i++) {
        assert(strcmp(roundie_gauge_alert_name((roundie_gauge_alert_t)i), "?") != 0);
    }
    for (int i = 0; i <= ROUNDIE_GAUGE_LEARN_RESULT_ABORTED; i++) {
        assert(strcmp(roundie_gauge_learn_result_name((roundie_gauge_learn_result_t)i), "?") != 0);
    }
    for (int i = 0; i <= ROUNDIE_GAUGE_LEARN_EDV2; i++) {
        assert(strcmp(roundie_gauge_learn_phase_name((roundie_gauge_learn_phase_t)i), "?") != 0);
    }
}

static void test_telemetry_log(void)
{
    static roundie_gauge_log_t log;
    roundie_gauge_log_entry_t entry;
    roundie_gauge_learning_sample_t sample = {0};
    memset(&log, 0, sizeof(log));
    assert(roundie_gauge_log_first(&log) == 0);
    assert(!roundie_gauge_log_get(&log, 0, &entry));

    /* An invalid sample keeps only time, hold state and link flags. */
    sample.time_us = 5 * SECOND;
    sample.voltage_mv = 4000;
    roundie_gauge_log_push(&log, &sample, ROUNDIE_GAUGE_HOLD_RUNNING, true, true, true);
    assert(roundie_gauge_log_get(&log, 0, &entry));
    assert(entry.time_ms == 5000);
    assert(entry.flags == (ROUNDIE_GAUGE_LOG_USB | ROUNDIE_GAUGE_LOG_INHIBITOR | ROUNDIE_GAUGE_LOG_WAKE));
    assert(entry.voltage_mv == 0);
    assert(entry.hold_state == ROUNDIE_GAUGE_HOLD_RUNNING);

    /* A valid battery sample; temperature clamps to int16. */
    sample.valid = true;
    sample.current_ma = -85;
    sample.average_current_ma = -83;
    sample.temperature_deci_c = 70000;
    sample.remaining_mah = 570;
    sample.full_mah = 600;
    sample.operation_flags = VDQ;
    sample.battery_flags = 0x0001;
    roundie_gauge_log_push(&log, &sample, ROUNDIE_GAUGE_HOLD_EXPIRED, false, false, false);
    assert(roundie_gauge_log_get(&log, 1, &entry));
    assert(entry.flags == ROUNDIE_GAUGE_LOG_VALID);
    assert(entry.voltage_mv == 4000 && entry.current_ma == -85);
    assert(entry.average_current_ma == -83);
    assert(entry.temperature_deci_c == INT16_MAX);
    assert(entry.remaining_mah == 570 && entry.full_mah == 600);
    assert(entry.operation_flags == VDQ && entry.battery_flags == 0x0001);
    assert(entry.hold_state == ROUNDIE_GAUGE_HOLD_EXPIRED);
    sample.temperature_deci_c = -70000;
    roundie_gauge_log_push(&log, &sample, ROUNDIE_GAUGE_HOLD_OFF, false, false, false);
    assert(roundie_gauge_log_get(&log, 2, &entry));
    assert(entry.temperature_deci_c == INT16_MIN);
    assert(!roundie_gauge_log_get(&log, 3, &entry));

    /* Wrap: the oldest entries are overwritten, sequences stay monotonic. */
    for (uint32_t i = 3; i < ROUNDIE_GAUGE_LOG_CAPACITY + 10; i++) {
        sample.time_us = (int64_t)i * SECOND;
        roundie_gauge_log_push(&log, &sample, ROUNDIE_GAUGE_HOLD_OFF, false, false, false);
    }
    assert(log.total == ROUNDIE_GAUGE_LOG_CAPACITY + 10);
    assert(roundie_gauge_log_first(&log) == 10);
    assert(!roundie_gauge_log_get(&log, 9, &entry));
    assert(roundie_gauge_log_get(&log, 10, &entry));
    assert(entry.time_ms == 10000);
    assert(roundie_gauge_log_get(&log, ROUNDIE_GAUGE_LOG_CAPACITY + 9, &entry));
    assert(entry.time_ms == (ROUNDIE_GAUGE_LOG_CAPACITY + 9) * 1000);
}

int main(void)
{
    test_min_current();
    test_learning_updates_fcc();
    test_charge_aborts();
    test_low_temperature_aborts();
    test_config_update_and_unknown();
    test_low_current_at_edv2();
    test_charge_after_edv2();
    test_gauge_reset_aborts();
    test_mcu_restart();
    test_edv2_without_vdq();
    test_invalid_samples();
    test_hold_limits();
    test_end_of_cycle();
    test_simulated_end();
    test_battery_window();
    test_profile_id();
    test_record();
    test_save_policy();
    test_names();
    test_telemetry_log();
    printf("gauge learning: OK\n");
    return 0;
}
