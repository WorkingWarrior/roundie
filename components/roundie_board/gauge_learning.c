#include "roundie_gauge_learning.h"

#include <string.h>

_Static_assert(sizeof(roundie_gauge_record_t) == 24, "record layout is stored in NVS");

static uint32_t crc32_bytes(const uint8_t *bytes, size_t length)
{
    uint32_t value = UINT32_MAX;
    for (size_t i = 0; i < length; i++) {
        value ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            value = (value >> 1) ^ (UINT32_C(0xedb88320) & (0U - (value & 1U)));
        }
    }
    return ~value;
}

static size_t put16(uint8_t *out, size_t at, uint16_t value)
{
    out[at] = (uint8_t)(value & 0xff);
    out[at + 1] = (uint8_t)(value >> 8);
    return at + 2;
}

static size_t put32(uint8_t *out, size_t at, uint32_t value)
{
    at = put16(out, at, (uint16_t)(value & 0xffff));
    return put16(out, at, (uint16_t)(value >> 16));
}

/* ---- tracker ---- */

uint16_t roundie_gauge_learning_min_current_ma(uint16_t design_mah)
{
    /* 3C/32, rounded up: a current equal to the result meets it. */
    return (uint16_t)(((uint32_t)design_mah * 3U + 31U) / 32U);
}

void roundie_gauge_learning_init(roundie_gauge_learning_t *tracker)
{
    memset(tracker, 0, sizeof(*tracker));
}

static int16_t magnitude(int16_t current_ma)
{
    return current_ma < 0 ? (int16_t)-current_ma : current_ma;
}

static void begin_qualified(roundie_gauge_learning_t *tracker, const roundie_gauge_learning_sample_t *sample)
{
    tracker->phase = ROUNDIE_GAUGE_LEARN_QUALIFIED;
    tracker->started_us = sample->time_us;
    tracker->start_remaining_mah = sample->remaining_mah;
    tracker->start_full_mah = sample->full_mah;
    tracker->min_temperature_deci_c = sample->temperature_deci_c;
    tracker->edv2_us = 0;
    tracker->qualified_count++;
}

static void finish(roundie_gauge_learning_t *tracker, const roundie_gauge_learning_sample_t *sample,
                   roundie_gauge_learn_result_t result, roundie_gauge_learn_cause_t cause)
{
    tracker->phase = ROUNDIE_GAUGE_LEARN_IDLE;
    tracker->result = result;
    tracker->cause = cause;
    tracker->result_us = sample->time_us;
    tracker->result_old_full_mah = tracker->last_full_mah;
    tracker->result_new_full_mah = sample->full_mah;
    if (result == ROUNDIE_GAUGE_LEARN_RESULT_UPDATED) {
        tracker->updated_count++;
    } else if (result == ROUNDIE_GAUGE_LEARN_RESULT_UNCHANGED) {
        tracker->unchanged_count++;
    } else {
        tracker->aborted_count++;
    }
}

/* VDQ cleared before EDV2: the first visible disqualifying condition. */
static roundie_gauge_learn_cause_t abort_cause(const roundie_gauge_learning_config_t *config,
                                               const roundie_gauge_learning_sample_t *sample)
{
    if ((sample->operation_flags & ROUNDIE_GAUGE_OP_CFGUPDATE) != 0) {
        return ROUNDIE_GAUGE_LEARN_CAUSE_CONFIG_UPDATE;
    }
    if (sample->current_ma > 0) {
        return ROUNDIE_GAUGE_LEARN_CAUSE_CHARGE;
    }
    if (sample->temperature_deci_c < config->learning_low_temp_deci_c) {
        return ROUNDIE_GAUGE_LEARN_CAUSE_LOW_TEMP;
    }
    return ROUNDIE_GAUGE_LEARN_CAUSE_UNKNOWN;
}

/* The EDV2 sample against the TRM 1.1.3 conditions visible in it. */
static roundie_gauge_learn_cause_t edv2_cause(const roundie_gauge_learning_t *tracker,
                                              const roundie_gauge_learning_config_t *config)
{
    int16_t current = magnitude(tracker->edv2_current_ma);
    if (tracker->edv2_current_ma >= 0 ||
        (uint16_t)current < roundie_gauge_learning_min_current_ma(tracker->last_design_mah)) {
        return ROUNDIE_GAUGE_LEARN_CAUSE_LOW_CURRENT;
    }
    if ((uint16_t)current > config->overload_ma) {
        return ROUNDIE_GAUGE_LEARN_CAUSE_OVERLOAD;
    }
    if (tracker->edv2_temperature_deci_c < config->learning_low_temp_deci_c) {
        return ROUNDIE_GAUGE_LEARN_CAUSE_LOW_TEMP;
    }
    return ROUNDIE_GAUGE_LEARN_CAUSE_NONE;
}

static void remember(roundie_gauge_learning_t *tracker, const roundie_gauge_learning_sample_t *sample)
{
    tracker->last_operation_flags = sample->operation_flags;
    tracker->last_full_mah = sample->full_mah;
    tracker->last_design_mah = sample->design_mah;
}

roundie_gauge_learn_result_t roundie_gauge_learning_update(
    roundie_gauge_learning_t *tracker, const roundie_gauge_learning_config_t *config,
    const roundie_gauge_learning_sample_t *sample)
{
    if (!sample->valid) {
        return ROUNDIE_GAUGE_LEARN_RESULT_NONE;
    }
    if (sample->current_ma < 0) {
        if (tracker->discharge_samples == 0 || sample->current_ma > tracker->discharge_smallest_ma) {
            tracker->discharge_smallest_ma = sample->current_ma;
        }
        if (tracker->discharge_samples == 0 || sample->current_ma < tracker->discharge_largest_ma) {
            tracker->discharge_largest_ma = sample->current_ma;
        }
        tracker->discharge_samples++;
        tracker->discharge_sum_ma += sample->current_ma;
    }
    const bool vdq = (sample->operation_flags & ROUNDIE_GAUGE_OP_VDQ) != 0;
    if (!tracker->started) {
        /* After an MCU restart the gauge may be mid-discharge: follow it,
         * without an EDV2 transition for flags already set. */
        tracker->started = true;
        tracker->resumed = vdq;
        if (vdq) {
            begin_qualified(tracker, sample);
        }
        remember(tracker, sample);
        return ROUNDIE_GAUGE_LEARN_RESULT_NONE;
    }

    const bool reset = !sample->initialized || sample->design_mah != tracker->last_design_mah;
    const bool vdq_rise = vdq && (tracker->last_operation_flags & ROUNDIE_GAUGE_OP_VDQ) == 0;
    const bool edv2_rise = (sample->operation_flags & ROUNDIE_GAUGE_OP_EDV2) != 0 &&
                           (tracker->last_operation_flags & ROUNDIE_GAUGE_OP_EDV2) == 0;
    const bool full_changed = sample->full_mah != tracker->last_full_mah;
    roundie_gauge_learn_result_t completed = ROUNDIE_GAUGE_LEARN_RESULT_NONE;

    if (edv2_rise) {
        tracker->edv2_seen = true;
        tracker->edv2_voltage_mv = sample->voltage_mv;
        tracker->edv2_current_ma = sample->current_ma;
        tracker->edv2_temperature_deci_c = sample->temperature_deci_c;
        tracker->edv2_remaining_mah = sample->remaining_mah;
        tracker->edv2_qualified = tracker->phase == ROUNDIE_GAUGE_LEARN_QUALIFIED ||
                                  (tracker->phase == ROUNDIE_GAUGE_LEARN_IDLE && vdq_rise);
    }

    switch (tracker->phase) {
    case ROUNDIE_GAUGE_LEARN_IDLE:
        if (reset) {
            break;
        }
        if (vdq_rise) {
            tracker->resumed = false;
            begin_qualified(tracker, sample);
            if (edv2_rise) {
                tracker->phase = ROUNDIE_GAUGE_LEARN_EDV2;
                tracker->edv2_us = sample->time_us;
            }
        } else if (edv2_rise) {
            finish(tracker, sample, ROUNDIE_GAUGE_LEARN_RESULT_ABORTED,
                   ROUNDIE_GAUGE_LEARN_CAUSE_NOT_QUALIFIED);
            completed = ROUNDIE_GAUGE_LEARN_RESULT_ABORTED;
        }
        break;
    case ROUNDIE_GAUGE_LEARN_QUALIFIED:
        if (sample->temperature_deci_c < tracker->min_temperature_deci_c) {
            tracker->min_temperature_deci_c = sample->temperature_deci_c;
        }
        if (reset) {
            completed = ROUNDIE_GAUGE_LEARN_RESULT_ABORTED;
            finish(tracker, sample, completed, ROUNDIE_GAUGE_LEARN_CAUSE_GAUGE_RESET);
        } else if (edv2_rise && full_changed) {
            completed = ROUNDIE_GAUGE_LEARN_RESULT_UPDATED;
            finish(tracker, sample, completed, ROUNDIE_GAUGE_LEARN_CAUSE_NONE);
        } else if (edv2_rise && vdq) {
            tracker->phase = ROUNDIE_GAUGE_LEARN_EDV2;
            tracker->edv2_us = sample->time_us;
        } else if (edv2_rise) {
            /* VDQ cleared by the time EDV2 was seen. */
            roundie_gauge_learn_cause_t cause = edv2_cause(tracker, config);
            completed = ROUNDIE_GAUGE_LEARN_RESULT_ABORTED;
            finish(tracker, sample, completed,
                   cause == ROUNDIE_GAUGE_LEARN_CAUSE_NONE ? abort_cause(config, sample) : cause);
        } else if (!vdq) {
            completed = ROUNDIE_GAUGE_LEARN_RESULT_ABORTED;
            finish(tracker, sample, completed, abort_cause(config, sample));
        }
        break;
    case ROUNDIE_GAUGE_LEARN_EDV2:
        /* VDQ is not used here: whether the gauge clears it on completion is
         * not documented. The FCC change is the evidence. */
        if (reset) {
            completed = ROUNDIE_GAUGE_LEARN_RESULT_ABORTED;
            finish(tracker, sample, completed, ROUNDIE_GAUGE_LEARN_CAUSE_GAUGE_RESET);
        } else if (full_changed) {
            completed = ROUNDIE_GAUGE_LEARN_RESULT_UPDATED;
            finish(tracker, sample, completed, ROUNDIE_GAUGE_LEARN_CAUSE_NONE);
        } else if (sample->current_ma > 0) {
            completed = ROUNDIE_GAUGE_LEARN_RESULT_ABORTED;
            finish(tracker, sample, completed, ROUNDIE_GAUGE_LEARN_CAUSE_CHARGE);
        } else if (sample->time_us - tracker->edv2_us >= ROUNDIE_GAUGE_LEARN_UPDATE_WINDOW_US) {
            completed = ROUNDIE_GAUGE_LEARN_RESULT_UNCHANGED;
            finish(tracker, sample, completed, edv2_cause(tracker, config));
        }
        break;
    }
    remember(tracker, sample);
    return completed;
}

const char *roundie_gauge_learn_phase_name(roundie_gauge_learn_phase_t phase)
{
    switch (phase) {
    case ROUNDIE_GAUGE_LEARN_IDLE: return "idle";
    case ROUNDIE_GAUGE_LEARN_QUALIFIED: return "qualified";
    case ROUNDIE_GAUGE_LEARN_EDV2: return "edv2";
    }
    return "?";
}

const char *roundie_gauge_learn_result_name(roundie_gauge_learn_result_t result)
{
    switch (result) {
    case ROUNDIE_GAUGE_LEARN_RESULT_NONE: return "none";
    case ROUNDIE_GAUGE_LEARN_RESULT_UPDATED: return "updated";
    case ROUNDIE_GAUGE_LEARN_RESULT_UNCHANGED: return "unchanged";
    case ROUNDIE_GAUGE_LEARN_RESULT_ABORTED: return "aborted";
    }
    return "?";
}

const char *roundie_gauge_learn_cause_name(roundie_gauge_learn_cause_t cause)
{
    switch (cause) {
    case ROUNDIE_GAUGE_LEARN_CAUSE_NONE: return "none";
    case ROUNDIE_GAUGE_LEARN_CAUSE_CHARGE: return "charge";
    case ROUNDIE_GAUGE_LEARN_CAUSE_LOW_TEMP: return "low-temp";
    case ROUNDIE_GAUGE_LEARN_CAUSE_LOW_CURRENT: return "low-current";
    case ROUNDIE_GAUGE_LEARN_CAUSE_OVERLOAD: return "overload";
    case ROUNDIE_GAUGE_LEARN_CAUSE_GAUGE_RESET: return "gauge-reset";
    case ROUNDIE_GAUGE_LEARN_CAUSE_CONFIG_UPDATE: return "config-update";
    case ROUNDIE_GAUGE_LEARN_CAUSE_NOT_QUALIFIED: return "not-qualified";
    case ROUNDIE_GAUGE_LEARN_CAUSE_UNKNOWN: return "unknown";
    }
    return "?";
}

/* ---- hold ---- */

bool roundie_gauge_hold_arm(roundie_gauge_hold_t *hold, int64_t now_us, int64_t duration_us)
{
    if (duration_us <= 0 || duration_us > ROUNDIE_GAUGE_HOLD_MAX_US) {
        return false;
    }
    memset(hold, 0, sizeof(*hold));
    hold->state = ROUNDIE_GAUGE_HOLD_RUNNING;
    hold->deadline_us = now_us + duration_us;
    return true;
}

void roundie_gauge_hold_cancel(roundie_gauge_hold_t *hold)
{
    if (hold->state == ROUNDIE_GAUGE_HOLD_RUNNING || hold->state == ROUNDIE_GAUGE_HOLD_EDV2) {
        hold->state = ROUNDIE_GAUGE_HOLD_CANCELLED;
        hold->protect = false;
        hold->alert = ROUNDIE_GAUGE_ALERT_NONE;
    }
}

bool roundie_gauge_hold_test(roundie_gauge_hold_t *hold, roundie_gauge_test_t test, int64_t now_us,
                             int64_t delay_us)
{
    if (hold->state != ROUNDIE_GAUGE_HOLD_RUNNING || hold->test != ROUNDIE_GAUGE_TEST_NONE) {
        return false;
    }
    if ((test != ROUNDIE_GAUGE_TEST_END && test != ROUNDIE_GAUGE_TEST_STOP) || delay_us < 0 ||
        delay_us > ROUNDIE_GAUGE_TEST_MAX_DELAY_US) {
        return false;
    }
    hold->test = test;
    hold->test_warn_us = now_us + delay_us;
    hold->test_end_us = hold->test_warn_us + ROUNDIE_GAUGE_TEST_GAP_US;
    return true;
}

static void hold_stop(roundie_gauge_hold_t *hold, roundie_gauge_hold_state_t state, int64_t now_us)
{
    hold->state = state;
    hold->ended_us = now_us;
    hold->protect = true;
    hold->alert = ROUNDIE_GAUGE_ALERT_STOPPED;
}

static void hold_snapshot(roundie_gauge_hold_t *hold, const roundie_gauge_learning_sample_t *sample)
{
    if (!hold->edv2_seen) {
        hold->edv2_seen = true;
        hold->edv2 = *sample;
    }
}

static bool hold_near_end(const roundie_gauge_learning_sample_t *sample)
{
    if (sample->current_ma >= 0) {
        return false;
    }
    uint32_t remaining = (uint32_t)sample->remaining_mah * 100U;
    uint32_t limit = (uint32_t)sample->full_mah * ROUNDIE_GAUGE_HOLD_WARN_PERCENT;
    return remaining <= limit || sample->voltage_mv <= ROUNDIE_GAUGE_HOLD_WARN_MV;
}

bool roundie_gauge_hold_update(roundie_gauge_hold_t *hold, const roundie_gauge_learning_sample_t *sample,
                               roundie_gauge_learn_result_t result, int64_t now_us)
{
    const bool active = hold->state == ROUNDIE_GAUGE_HOLD_RUNNING || hold->state == ROUNDIE_GAUGE_HOLD_EDV2;
    if (!active) {
        /* Ended: protection lasts until the battery is seen charging. */
        if (hold->protect && sample->valid && sample->current_ma > 0) {
            hold->protect = false;
            hold->alert = ROUNDIE_GAUGE_ALERT_NONE;
        }
        return false;
    }
    if (hold->state == ROUNDIE_GAUGE_HOLD_RUNNING && now_us >= hold->deadline_us) {
        hold->state = ROUNDIE_GAUGE_HOLD_EXPIRED;
        hold->ended_us = now_us;
        /* Near the end of the battery the expiry needs the same protection. */
        hold->protect = hold->warned;
        hold->alert = hold->warned ? ROUNDIE_GAUGE_ALERT_STOPPED : ROUNDIE_GAUGE_ALERT_NONE;
        return false;
    }
    if (!sample->valid) {
        hold->missed++;
        if (hold->missed >= ROUNDIE_GAUGE_HOLD_MAX_MISSED) {
            hold_stop(hold, ROUNDIE_GAUGE_HOLD_NO_DATA, now_us);
        }
        return hold->state == ROUNDIE_GAUGE_HOLD_RUNNING;
    }
    hold->missed = 0;
    /* Safety stops first: a result in the same sample is not a success. */
    if ((sample->battery_flags & (ROUNDIE_GAUGE_BAT_SYSDWN | ROUNDIE_GAUGE_BAT_FD)) != 0) {
        hold_stop(hold, ROUNDIE_GAUGE_HOLD_SYSDOWN, now_us);
    } else if (sample->current_ma < 0 && sample->voltage_mv <= ROUNDIE_GAUGE_HOLD_FLOOR_MV) {
        hold_stop(hold, ROUNDIE_GAUGE_HOLD_LOW_VOLTAGE, now_us);
    } else if (sample->temperature_deci_c >= ROUNDIE_GAUGE_HOLD_MAX_DECI_C) {
        hold_stop(hold, ROUNDIE_GAUGE_HOLD_HOT, now_us);
    } else if (sample->temperature_deci_c < ROUNDIE_GAUGE_HOLD_MIN_DECI_C) {
        hold_stop(hold, ROUNDIE_GAUGE_HOLD_COLD, now_us);
    } else if (result == ROUNDIE_GAUGE_LEARN_RESULT_UPDATED || result == ROUNDIE_GAUGE_LEARN_RESULT_UNCHANGED) {
        hold_snapshot(hold, sample);
        hold->state = ROUNDIE_GAUGE_HOLD_DONE;
        hold->ended_us = now_us;
        hold->result = result;
        hold->learned = result == ROUNDIE_GAUGE_LEARN_RESULT_UPDATED && !hold->simulated;
        hold->protect = true;
        hold->alert = ROUNDIE_GAUGE_ALERT_FINISHED;
    } else if (result == ROUNDIE_GAUGE_LEARN_RESULT_ABORTED) {
        hold->result = result;
        hold_stop(hold, ROUNDIE_GAUGE_HOLD_ABORTED, now_us);
    } else if (hold->state == ROUNDIE_GAUGE_HOLD_RUNNING &&
               (sample->operation_flags & ROUNDIE_GAUGE_OP_EDV2) != 0 && sample->current_ma < 0) {
        hold_snapshot(hold, sample);
        hold->state = ROUNDIE_GAUGE_HOLD_EDV2;
        hold->edv2_deadline_us = now_us + ROUNDIE_GAUGE_HOLD_EDV2_WAIT_US;
        hold->protect = true;
        hold->alert = ROUNDIE_GAUGE_ALERT_FINISHED;
    } else if (hold->state == ROUNDIE_GAUGE_HOLD_EDV2 && now_us >= hold->edv2_deadline_us) {
        if (hold->simulated) {
            /* The simulated EDV2 had no real result to wait for. */
            hold->state = ROUNDIE_GAUGE_HOLD_TEST_END;
            hold->ended_us = now_us;
        } else {
            hold_stop(hold, ROUNDIE_GAUGE_HOLD_NO_RESULT, now_us);
        }
    } else if (hold->state == ROUNDIE_GAUGE_HOLD_RUNNING && hold->test != ROUNDIE_GAUGE_TEST_NONE &&
               now_us >= hold->test_end_us) {
        hold->simulated = true;
        if (hold->test == ROUNDIE_GAUGE_TEST_END) {
            hold_snapshot(hold, sample);
            hold->state = ROUNDIE_GAUGE_HOLD_EDV2;
            hold->edv2_deadline_us = now_us + ROUNDIE_GAUGE_HOLD_EDV2_WAIT_US;
            hold->protect = true;
            hold->alert = ROUNDIE_GAUGE_ALERT_FINISHED;
        } else {
            hold_stop(hold, ROUNDIE_GAUGE_HOLD_TEST_STOP, now_us);
        }
    } else if (hold->state == ROUNDIE_GAUGE_HOLD_RUNNING && hold->test != ROUNDIE_GAUGE_TEST_NONE &&
               !hold->warned && now_us >= hold->test_warn_us) {
        hold->simulated = true;
        hold->warned = true;
        hold->warned_us = now_us;
        hold->alert = ROUNDIE_GAUGE_ALERT_NEAR_END;
    } else if (hold->state == ROUNDIE_GAUGE_HOLD_RUNNING && !hold->warned && hold_near_end(sample)) {
        hold->warned = true;
        hold->warned_us = now_us;
        hold->alert = ROUNDIE_GAUGE_ALERT_NEAR_END;
    }
    return hold->state == ROUNDIE_GAUGE_HOLD_RUNNING;
}

const char *roundie_gauge_hold_state_name(roundie_gauge_hold_state_t state)
{
    switch (state) {
    case ROUNDIE_GAUGE_HOLD_OFF: return "off";
    case ROUNDIE_GAUGE_HOLD_RUNNING: return "running";
    case ROUNDIE_GAUGE_HOLD_DONE: return "done";
    case ROUNDIE_GAUGE_HOLD_ABORTED: return "aborted";
    case ROUNDIE_GAUGE_HOLD_LOW_VOLTAGE: return "low-voltage";
    case ROUNDIE_GAUGE_HOLD_SYSDOWN: return "sysdown";
    case ROUNDIE_GAUGE_HOLD_HOT: return "hot";
    case ROUNDIE_GAUGE_HOLD_NO_DATA: return "no-data";
    case ROUNDIE_GAUGE_HOLD_EXPIRED: return "expired";
    case ROUNDIE_GAUGE_HOLD_CANCELLED: return "cancelled";
    case ROUNDIE_GAUGE_HOLD_EDV2: return "edv2";
    case ROUNDIE_GAUGE_HOLD_COLD: return "cold";
    case ROUNDIE_GAUGE_HOLD_NO_RESULT: return "no-result";
    case ROUNDIE_GAUGE_HOLD_TEST_END: return "test-end";
    case ROUNDIE_GAUGE_HOLD_TEST_STOP: return "test-stop";
    }
    return "?";
}

bool roundie_gauge_hold_sleep_wanted(const roundie_gauge_hold_t *hold)
{
    return hold->protect && hold->state != ROUNDIE_GAUGE_HOLD_RUNNING && hold->state != ROUNDIE_GAUGE_HOLD_EDV2;
}

void roundie_gauge_hold_external_power(roundie_gauge_hold_t *hold)
{
    if (hold->state != ROUNDIE_GAUGE_HOLD_RUNNING && hold->state != ROUNDIE_GAUGE_HOLD_EDV2) {
        hold->protect = false;
        hold->alert = ROUNDIE_GAUGE_ALERT_NONE;
    }
}

const char *roundie_gauge_alert_name(roundie_gauge_alert_t alert)
{
    switch (alert) {
    case ROUNDIE_GAUGE_ALERT_NONE: return "none";
    case ROUNDIE_GAUGE_ALERT_NEAR_END: return "near-end";
    case ROUNDIE_GAUGE_ALERT_FINISHED: return "finished";
    case ROUNDIE_GAUGE_ALERT_STOPPED: return "stopped";
    }
    return "?";
}

/* ---- record ---- */

uint32_t roundie_gauge_profile_id(const roundie_gauge_capacity_t *state)
{
    uint8_t bytes[20];
    size_t at = 0;
    at = put16(bytes, at, state->gauging_config);
    at = put16(bytes, at, state->design_voltage_mv);
    at = put16(bytes, at, state->charging_voltage_mv);
    at = put16(bytes, at, state->taper_current_ma);
    at = put16(bytes, at, state->near_full_mah);
    at = put16(bytes, at, state->battery_low_centi);
    at = put16(bytes, at, state->overload_ma);
    at = put16(bytes, at, state->reserve_mah);
    at = put16(bytes, at, state->flag_config_a);
    bytes[at++] = state->flag_config_b;
    bytes[at++] = state->learning_low_temp_deci_c;
    return crc32_bytes(bytes, at);
}

bool roundie_gauge_full_acceptable(uint16_t design_mah, uint16_t full_mah)
{
    if (design_mah == 0) {
        return false;
    }
    return (uint32_t)full_mah * 2U >= design_mah && (uint32_t)full_mah * 4U <= (uint32_t)design_mah * 5U;
}

static uint32_t record_crc(const roundie_gauge_record_t *record)
{
    uint8_t bytes[20];
    size_t at = 0;
    at = put32(bytes, at, record->magic);
    at = put16(bytes, at, record->version);
    at = put16(bytes, at, record->design_mah);
    at = put32(bytes, at, record->profile_id);
    at = put16(bytes, at, record->learned_full_mah);
    at = put16(bytes, at, record->reserved);
    at = put32(bytes, at, record->sequence);
    return crc32_bytes(bytes, at);
}

void roundie_gauge_record_make(roundie_gauge_record_t *record, uint16_t design_mah,
                               uint32_t profile_id, uint16_t learned_full_mah, uint32_t sequence)
{
    memset(record, 0, sizeof(*record));
    record->magic = ROUNDIE_GAUGE_RECORD_MAGIC;
    record->version = ROUNDIE_GAUGE_RECORD_VERSION;
    record->design_mah = design_mah;
    record->profile_id = profile_id;
    record->learned_full_mah = learned_full_mah;
    record->sequence = sequence;
    record->crc = record_crc(record);
}

bool roundie_gauge_record_valid(const roundie_gauge_record_t *record, size_t size,
                                uint16_t design_mah, uint32_t profile_id)
{
    return record != NULL && size == sizeof(*record) &&
           record->magic == ROUNDIE_GAUGE_RECORD_MAGIC &&
           record->version == ROUNDIE_GAUGE_RECORD_VERSION &&
           record->reserved == 0 &&
           record->crc == record_crc(record) &&
           record->design_mah == design_mah &&
           record->profile_id == profile_id &&
           roundie_gauge_full_acceptable(design_mah, record->learned_full_mah);
}

uint16_t roundie_gauge_record_restore_full(const roundie_gauge_record_t *record, size_t size,
                                           uint16_t design_mah, uint32_t profile_id)
{
    if (roundie_gauge_record_valid(record, size, design_mah, profile_id)) {
        return record->learned_full_mah;
    }
    return design_mah;
}

bool roundie_gauge_record_save_due(const roundie_gauge_record_t *stored, size_t stored_size,
                                   uint16_t design_mah, uint32_t profile_id,
                                   uint16_t learned_full_mah, uint32_t saves_this_boot)
{
    if (!roundie_gauge_full_acceptable(design_mah, learned_full_mah) ||
        saves_this_boot >= ROUNDIE_GAUGE_RECORD_MAX_SAVES_PER_BOOT) {
        return false;
    }
    if (!roundie_gauge_record_valid(stored, stored_size, design_mah, profile_id)) {
        return true;
    }
    uint16_t step = design_mah / 100U > 0 ? (uint16_t)(design_mah / 100U) : 1U;
    uint16_t difference = learned_full_mah > stored->learned_full_mah
                              ? (uint16_t)(learned_full_mah - stored->learned_full_mah)
                              : (uint16_t)(stored->learned_full_mah - learned_full_mah);
    return difference >= step;
}

void roundie_gauge_log_push(roundie_gauge_log_t *log, const roundie_gauge_learning_sample_t *sample,
                            roundie_gauge_hold_state_t hold_state, bool usb_connected, bool inhibitor_held,
                            bool after_wake)
{
    roundie_gauge_log_entry_t entry = {0};
    entry.time_ms = (uint32_t)(sample->time_us / 1000);
    entry.hold_state = (uint8_t)hold_state;
    if (usb_connected) {
        entry.flags |= ROUNDIE_GAUGE_LOG_USB;
    }
    if (inhibitor_held) {
        entry.flags |= ROUNDIE_GAUGE_LOG_INHIBITOR;
    }
    if (after_wake) {
        entry.flags |= ROUNDIE_GAUGE_LOG_WAKE;
    }
    if (sample->valid) {
        entry.flags |= ROUNDIE_GAUGE_LOG_VALID;
        entry.voltage_mv = sample->voltage_mv;
        entry.current_ma = sample->current_ma;
        entry.average_current_ma = sample->average_current_ma;
        int32_t temperature = sample->temperature_deci_c;
        if (temperature > INT16_MAX) {
            temperature = INT16_MAX;
        } else if (temperature < INT16_MIN) {
            temperature = INT16_MIN;
        }
        entry.temperature_deci_c = (int16_t)temperature;
        entry.remaining_mah = sample->remaining_mah;
        entry.full_mah = sample->full_mah;
        entry.operation_flags = sample->operation_flags;
        entry.battery_flags = sample->battery_flags;
    }
    log->entries[log->total % ROUNDIE_GAUGE_LOG_CAPACITY] = entry;
    log->total++;
}

uint32_t roundie_gauge_log_first(const roundie_gauge_log_t *log)
{
    if (log->total <= ROUNDIE_GAUGE_LOG_CAPACITY) {
        return 0;
    }
    return log->total - ROUNDIE_GAUGE_LOG_CAPACITY;
}

bool roundie_gauge_log_get(const roundie_gauge_log_t *log, uint32_t sequence, roundie_gauge_log_entry_t *entry)
{
    if (sequence >= log->total || sequence < roundie_gauge_log_first(log)) {
        return false;
    }
    *entry = log->entries[sequence % ROUNDIE_GAUGE_LOG_CAPACITY];
    return true;
}
