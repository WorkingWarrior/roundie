#pragma once

/* BQ27220 CEDV capacity learning (TI SLUUBD4A 1.1.3, 1.1.4, 1.1.10, 4.9.36).
 * Pure C, no driver types, used by the system worker and host tests:
 *   tracker  follows VDQ, EDV2 and FullChargeCapacity() in periodic samples
 *            and reports how a qualified discharge ended, with the abort
 *            cause where the samples show it
 *   hold     decides, for one controlled cycle, whether the system should
 *            keep its load on (screen and CPU awake) and when to stop
 *   record   the learned Full Charge Capacity kept across a gauge power-on
 *            reset: integrity, profile identity, write limiting
 * The tracker observes; the gauge alone decides whether a discharge
 * qualifies. Causes it infers are labelled as inferences. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "roundie_gauge.h"

/* OperationStatus() and BatteryStatus() bits (TRM tables 2-x). */
#define ROUNDIE_GAUGE_OP_EDV2 0x0008U
#define ROUNDIE_GAUGE_OP_VDQ 0x0010U
#define ROUNDIE_GAUGE_OP_INITCOMP 0x0020U
#define ROUNDIE_GAUGE_OP_CFGUPDATE 0x0400U
#define ROUNDIE_GAUGE_BAT_SYSDWN 0x0002U
#define ROUNDIE_GAUGE_BAT_FC 0x0200U
#define ROUNDIE_GAUGE_BAT_FD 0x8000U

/* After EDV2, how long the tracker waits for FullChargeCapacity() to change
 * (the TRM saves the new value within 4 s; samples are 5 s apart). */
#define ROUNDIE_GAUGE_LEARN_UPDATE_WINDOW_US INT64_C(60000000)

/* One gauge sample. valid: the standard commands were read. */
typedef struct {
    bool valid;
    bool initialized; /* OperationStatus()[INITCOMP] */
    int64_t time_us;
    uint16_t operation_flags;
    uint16_t battery_flags;
    uint16_t voltage_mv;
    int16_t current_ma; /* positive = charging */
    int32_t temperature_deci_c;
    uint16_t remaining_mah;
    uint16_t full_mah;   /* FullChargeCapacity() */
    uint16_t design_mah; /* DesignCapacity() */
} roundie_gauge_learning_sample_t;

/* Gauge thresholds the inference compares with (Data Memory values). */
typedef struct {
    int32_t learning_low_temp_deci_c; /* Learning Low Temp, default 119 */
    uint16_t overload_ma;             /* OverLoad Current, default 1500 */
} roundie_gauge_learning_config_t;

typedef enum {
    ROUNDIE_GAUGE_LEARN_IDLE,      /* VDQ clear */
    ROUNDIE_GAUGE_LEARN_QUALIFIED, /* VDQ set, EDV2 not reached */
    ROUNDIE_GAUGE_LEARN_EDV2,      /* VDQ was set at EDV2: waiting for the FCC update */
} roundie_gauge_learn_phase_t;

typedef enum {
    ROUNDIE_GAUGE_LEARN_RESULT_NONE,
    ROUNDIE_GAUGE_LEARN_RESULT_UPDATED,   /* FCC changed after a qualified EDV2 */
    ROUNDIE_GAUGE_LEARN_RESULT_UNCHANGED, /* qualified EDV2, FCC the same after the window */
    ROUNDIE_GAUGE_LEARN_RESULT_ABORTED,   /* VDQ cleared before an update, or EDV2 unqualified */
} roundie_gauge_learn_result_t;

typedef enum {
    ROUNDIE_GAUGE_LEARN_CAUSE_NONE,
    ROUNDIE_GAUGE_LEARN_CAUSE_CHARGE,        /* current > 0 when VDQ cleared */
    ROUNDIE_GAUGE_LEARN_CAUSE_LOW_TEMP,      /* temperature below Learning Low Temp */
    ROUNDIE_GAUGE_LEARN_CAUSE_LOW_CURRENT,   /* |current| < 3C/32 at EDV2 */
    ROUNDIE_GAUGE_LEARN_CAUSE_OVERLOAD,      /* |current| > OverLoad Current at EDV2 */
    ROUNDIE_GAUGE_LEARN_CAUSE_GAUGE_RESET,   /* INITCOMP lost or DesignCapacity() changed */
    ROUNDIE_GAUGE_LEARN_CAUSE_CONFIG_UPDATE, /* CFGUPDATE set */
    ROUNDIE_GAUGE_LEARN_CAUSE_NOT_QUALIFIED, /* EDV2 reached with VDQ clear */
    ROUNDIE_GAUGE_LEARN_CAUSE_UNKNOWN,       /* not visible in the samples */
} roundie_gauge_learn_cause_t;

typedef struct {
    roundie_gauge_learn_phase_t phase;
    bool started;             /* at least one valid sample */
    bool resumed;             /* VDQ was already set at the first sample (MCU restart) */
    uint16_t last_operation_flags;
    uint16_t last_full_mah;
    uint16_t last_design_mah;
    /* The qualified discharge in progress (phase != IDLE). */
    int64_t started_us;
    uint16_t start_remaining_mah;
    uint16_t start_full_mah;
    int32_t min_temperature_deci_c;
    int64_t edv2_us;
    /* The last EDV2 transition, qualified or not. */
    bool edv2_seen;
    uint16_t edv2_voltage_mv;
    int16_t edv2_current_ma;
    int32_t edv2_temperature_deci_c;
    uint16_t edv2_remaining_mah;
    bool edv2_qualified;
    /* Discharge current over all samples with current < 0. */
    uint32_t discharge_samples;
    int64_t discharge_sum_ma;
    int16_t discharge_smallest_ma; /* closest to zero */
    int16_t discharge_largest_ma;  /* most negative */
    /* The last result. */
    roundie_gauge_learn_result_t result;
    roundie_gauge_learn_cause_t cause;
    int64_t result_us;
    uint16_t result_old_full_mah;
    uint16_t result_new_full_mah;
    uint32_t qualified_count;
    uint32_t updated_count;
    uint32_t unchanged_count;
    uint32_t aborted_count;
} roundie_gauge_learning_t;

void roundie_gauge_learning_init(roundie_gauge_learning_t *tracker);
/* Feeds one sample. Returns the result that completed with this sample
 * (also kept in tracker->result), or NONE. */
roundie_gauge_learn_result_t roundie_gauge_learning_update(
    roundie_gauge_learning_t *tracker, const roundie_gauge_learning_config_t *config,
    const roundie_gauge_learning_sample_t *sample);
/* 3C/32 in mA (TRM 1.1.3 minimum discharge current at EDV2). */
uint16_t roundie_gauge_learning_min_current_ma(uint16_t design_mah);
const char *roundie_gauge_learn_phase_name(roundie_gauge_learn_phase_t phase);
const char *roundie_gauge_learn_result_name(roundie_gauge_learn_result_t result);
const char *roundie_gauge_learn_cause_name(roundie_gauge_learn_cause_t cause);

/* Load hold for one controlled cycle. Without it the system turns the screen
 * off and light-sleeps, and its discharge current is expected to fall below
 * C/32, where the gauge detects no EDV (TRM 1.1.4). The hold ends by itself;
 * once ended it stays ended until armed again. */
#define ROUNDIE_GAUGE_HOLD_FLOOR_MV 3300 /* release below: margin above SysDown 3150 mV */
#define ROUNDIE_GAUGE_HOLD_MAX_DECI_C 450 /* release at or above 45.0 degC */
#define ROUNDIE_GAUGE_HOLD_MAX_MISSED 6   /* consecutive invalid samples (30 s) */
#define ROUNDIE_GAUGE_HOLD_MAX_US (INT64_C(24) * 3600 * 1000000)

typedef enum {
    ROUNDIE_GAUGE_HOLD_OFF,         /* never armed */
    ROUNDIE_GAUGE_HOLD_RUNNING,     /* the load is to be held */
    ROUNDIE_GAUGE_HOLD_DONE,        /* learning result UPDATED or UNCHANGED */
    ROUNDIE_GAUGE_HOLD_ABORTED,     /* learning result ABORTED */
    ROUNDIE_GAUGE_HOLD_LOW_VOLTAGE, /* voltage below the floor while discharging */
    ROUNDIE_GAUGE_HOLD_SYSDOWN,     /* BatteryStatus()[SYSDWN] or [FD] */
    ROUNDIE_GAUGE_HOLD_HOT,         /* temperature at or above the limit */
    ROUNDIE_GAUGE_HOLD_NO_DATA,     /* too many invalid samples */
    ROUNDIE_GAUGE_HOLD_EXPIRED,     /* the armed duration elapsed */
    ROUNDIE_GAUGE_HOLD_CANCELLED,   /* stopped by command */
} roundie_gauge_hold_state_t;

typedef struct {
    roundie_gauge_hold_state_t state;
    int64_t deadline_us;
    uint32_t missed;
} roundie_gauge_hold_t;

/* duration_us: 1 .. ROUNDIE_GAUGE_HOLD_MAX_US; false otherwise (state kept). */
bool roundie_gauge_hold_arm(roundie_gauge_hold_t *hold, int64_t now_us, int64_t duration_us);
void roundie_gauge_hold_cancel(roundie_gauge_hold_t *hold);
/* Call after roundie_gauge_learning_update() with its return value. True
 * while the load is to be held. */
bool roundie_gauge_hold_update(roundie_gauge_hold_t *hold, const roundie_gauge_learning_sample_t *sample,
                               roundie_gauge_learn_result_t result, int64_t now_us);
const char *roundie_gauge_hold_state_name(roundie_gauge_hold_state_t state);

/* Learned Full Charge Capacity record (one NVS blob). The gauge cannot
 * identify the cell: the record is tied to the configured Design Capacity
 * and a CRC of the gauge's profile parameters, so a changed profile or
 * target discards it; a different cell of the same capacity is not detected
 * and is corrected by the next learning cycle. */
#define ROUNDIE_GAUGE_RECORD_MAGIC UINT32_C(0x52474c31) /* "RGL1" */
#define ROUNDIE_GAUGE_RECORD_VERSION 1
#define ROUNDIE_GAUGE_RECORD_MAX_SAVES_PER_BOOT 4

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t design_mah;
    uint32_t profile_id;
    uint16_t learned_full_mah;
    uint16_t reserved; /* 0 */
    uint32_t sequence; /* 1 for the first save, +1 per save */
    uint32_t crc;      /* CRC-32 of the 20 bytes before it */
} roundie_gauge_record_t;

/* CRC-32 of the profile parameters that configure learning: everything the
 * capacity read reports except capacities, flags and progress. */
uint32_t roundie_gauge_profile_id(const roundie_gauge_capacity_t *state);
/* A learned FCC plausible for design_mah: design/2 .. design*5/4. */
bool roundie_gauge_full_acceptable(uint16_t design_mah, uint16_t full_mah);
void roundie_gauge_record_make(roundie_gauge_record_t *record, uint16_t design_mah,
                               uint32_t profile_id, uint16_t learned_full_mah, uint32_t sequence);
/* size: bytes actually stored. Checks size, magic, version, CRC, design,
 * profile and plausibility. */
bool roundie_gauge_record_valid(const roundie_gauge_record_t *record, size_t size,
                                uint16_t design_mah, uint32_t profile_id);
/* The FCC to write after a gauge reset: the record's learned value when
 * valid, otherwise design_mah. record may be NULL. */
uint16_t roundie_gauge_record_restore_full(const roundie_gauge_record_t *record, size_t size,
                                           uint16_t design_mah, uint32_t profile_id);
/* Whether a learned value should be written: plausible, at most
 * MAX_SAVES_PER_BOOT per boot, and different from a valid stored record by at
 * least 1 % of design (at least 1 mAh). stored may be NULL. */
bool roundie_gauge_record_save_due(const roundie_gauge_record_t *stored, size_t stored_size,
                                   uint16_t design_mah, uint32_t profile_id,
                                   uint16_t learned_full_mah, uint32_t saves_this_boot);
