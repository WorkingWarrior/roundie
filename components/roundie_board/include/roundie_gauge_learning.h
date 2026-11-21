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
    int16_t current_ma;         /* Current(), positive = charging */
    int16_t average_current_ma; /* AverageCurrent() (IIR, about 14.5 s); 0 when not read */
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

/* Load hold for one controlled cycle, and the protection of its end. The
 * hold keeps the screen and CPU awake (measured 105-107 mA on battery, the
 * screen-off system 62-63 mA). It is armed only by an explicit command and
 * lives in RAM: after an MCU reset it is OFF and nothing resumes.
 *
 * It ends by itself at EDV2 (load released, snapshot taken, the FCC update
 * awaited), at a learning result, or at a safety stop: voltage at or below
 * the floor while discharging, SYSDWN/FD, temperature outside the window,
 * missing samples. Every end except EXPIRED (without a warning) and
 * CANCELLED sets protect: the system should light-sleep between samples and
 * show an alert until a charging sample is seen. learned is set only for an
 * UPDATED result before any safety stop. Once ended it stays ended until
 * armed again. */
#define ROUNDIE_GAUGE_HOLD_FLOOR_MV 3300  /* stop at or below: margin above SysDown 3150 mV */
#define ROUNDIE_GAUGE_HOLD_MAX_DECI_C 450 /* stop at or above 45.0 degC */
#define ROUNDIE_GAUGE_HOLD_MIN_DECI_C 100 /* stop below 10.0 degC (Learning Low Temp 11.9) */
#define ROUNDIE_GAUGE_HOLD_MAX_MISSED 6   /* consecutive invalid samples (30 s) */
#define ROUNDIE_GAUGE_HOLD_MAX_US (INT64_C(24) * 3600 * 1000000)
/* Early warning: RM at or below 15 % of FCC, or voltage at or below 3550 mV,
 * while discharging (EDV2 is at Battery Low 7 %, about 27 min later at
 * 106 mA for 600 mAh). */
#define ROUNDIE_GAUGE_HOLD_WARN_PERCENT 15U
#define ROUNDIE_GAUGE_HOLD_WARN_MV 3550
/* After EDV2 the gauge saves FCC within 4 s (TRM 1.1.3); without a learning
 * result within this time the hold ends as NO_RESULT. */
#define ROUNDIE_GAUGE_HOLD_EDV2_WAIT_US (INT64_C(120) * 1000000)

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
    ROUNDIE_GAUGE_HOLD_EDV2,        /* EDV2 seen: load released, learning result awaited */
    ROUNDIE_GAUGE_HOLD_COLD,        /* temperature below the limit */
    ROUNDIE_GAUGE_HOLD_NO_RESULT,   /* no learning result within the wait after EDV2 */
    ROUNDIE_GAUGE_HOLD_TEST_END,    /* simulated EDV2 end (development test), never learned */
    ROUNDIE_GAUGE_HOLD_TEST_STOP,   /* simulated safety stop (development test) */
    ROUNDIE_GAUGE_HOLD_TEST_USB,    /* external power returned during a started test */
} roundie_gauge_hold_state_t;

/* Development test of the end path without discharging the cell. */
typedef enum {
    ROUNDIE_GAUGE_TEST_NONE,
    ROUNDIE_GAUGE_TEST_END,  /* warning, then a simulated EDV2 */
    ROUNDIE_GAUGE_TEST_STOP, /* warning, then a simulated safety stop */
} roundie_gauge_test_t;
#define ROUNDIE_GAUGE_TEST_GAP_US (INT64_C(60) * 1000000) /* warning to end */
#define ROUNDIE_GAUGE_TEST_MAX_DELAY_US (INT64_C(3600) * 1000000)
/* Consecutive battery samples (no USB link, discharging) that start a test. */
#define ROUNDIE_GAUGE_TEST_BATTERY_SAMPLES 2U

/* What the device should show; it does not depend on a USB host. */
typedef enum {
    ROUNDIE_GAUGE_ALERT_NONE,
    ROUNDIE_GAUGE_ALERT_NEAR_END, /* warning: the end of the test is near */
    ROUNDIE_GAUGE_ALERT_FINISHED, /* EDV2 or a learning result: connect USB */
    ROUNDIE_GAUGE_ALERT_STOPPED,  /* a safety stop or abort: connect USB */
} roundie_gauge_alert_t;

typedef struct {
    roundie_gauge_hold_state_t state;
    int64_t deadline_us;
    int64_t edv2_deadline_us;
    uint32_t missed;
    bool warned;
    int64_t warned_us;
    bool protect; /* light-sleep between samples until a charging sample */
    roundie_gauge_alert_t alert;
    bool edv2_seen;
    roundie_gauge_learning_sample_t edv2; /* first EDV2 sample (or the result sample) */
    roundie_gauge_learn_result_t result;  /* the learning result that ended the hold */
    bool learned;                         /* UPDATED before any safety stop, never simulated */
    int64_t ended_us;
    roundie_gauge_test_t test; /* armed simulation, RAM only */
    int64_t test_delay_us;     /* battery start to the warning */
    bool test_started;         /* the countdown runs; set once per arming */
    int64_t test_started_us;
    uint32_t test_battery;     /* consecutive battery samples while armed */
    int64_t test_warn_us;
    int64_t test_end_us;
    bool simulated; /* a simulated event changed this hold */
} roundie_gauge_hold_t;

/* duration_us: 1 .. ROUNDIE_GAUGE_HOLD_MAX_US; false otherwise (state kept).
 * Arming clears the previous end, alert and snapshot. */
bool roundie_gauge_hold_arm(roundie_gauge_hold_t *hold, int64_t now_us, int64_t duration_us);
void roundie_gauge_hold_cancel(roundie_gauge_hold_t *hold);
/* Development only: arm a simulation on a RUNNING hold. Nothing is timed
 * yet: roundie_gauge_hold_supply() starts the countdown on battery power,
 * then the simulated warning comes delay_us later and the simulated end
 * (test) ROUNDIE_GAUGE_TEST_GAP_US after it. Changes the hold only: no
 * sample, tracker or gauge value is altered, and a simulated end never sets
 * learned. The EDV2 snapshot of a simulated end is the real sample at that
 * time. false: not RUNNING, a test already armed, or arguments out of
 * range. */
bool roundie_gauge_hold_test(roundie_gauge_hold_t *hold, roundie_gauge_test_t test, int64_t delay_us);
/* Call before roundie_gauge_hold_update() with each sample. usb_link: a USB
 * host link was seen. An armed test starts after
 * ROUNDIE_GAUGE_TEST_BATTERY_SAMPLES consecutive valid samples without a
 * link and with a discharge current. External power (a link or a charging
 * sample) during a started test ends it as TEST_USB; it never restarts.
 * A link also ends the protection of an ended hold, as
 * roundie_gauge_hold_external_power(). */
void roundie_gauge_hold_supply(roundie_gauge_hold_t *hold, const roundie_gauge_learning_sample_t *sample,
                               bool usb_link, int64_t now_us);
/* Call after roundie_gauge_learning_update() with its return value. True
 * while the load is to be held. */
bool roundie_gauge_hold_update(roundie_gauge_hold_t *hold, const roundie_gauge_learning_sample_t *sample,
                               roundie_gauge_learn_result_t result, int64_t now_us);
const char *roundie_gauge_hold_state_name(roundie_gauge_hold_state_t state);
const char *roundie_gauge_alert_name(roundie_gauge_alert_t alert);
/* Whether the system should light-sleep between samples: protect is set and
 * the hold has ended (not while RUNNING, nor while the EDV2 result is
 * awaited, so the result is sampled without a sleep gap). */
bool roundie_gauge_hold_sleep_wanted(const roundie_gauge_hold_t *hold);
/* A USB host link was seen: external power is present even when a full
 * battery shows no charging current. Ends the protection and alert of an
 * ended hold; a running hold or the EDV2 wait is not changed. */
void roundie_gauge_hold_external_power(roundie_gauge_hold_t *hold);

/* Battery test window (development telemetry): from the start of a test
 * countdown on battery to the return of external power. Light-sleep
 * counters come from the PowerManager and are cumulative since boot; the
 * window keeps the values at its start, at the first protective sleep
 * request and at the last battery sample. The gauge has 1 mAh RM
 * resolution and does not integrate currents inside its deadband, so the
 * mean current derived from RM is an estimate with that uncertainty. */
typedef struct {
    uint32_t sleeps;      /* completed light sleeps */
    int64_t slept_us;     /* their total duration */
    uint32_t timer_wakes; /* wakes by the timer alone */
    uint32_t other_wakes; /* any other cause: touch, BOOT, error */
    int64_t awake_us;     /* sum of the awake gaps between consecutive sleeps */
    uint32_t awake_gaps;
    uint32_t causes;      /* the last wake's esp_sleep cause mask */
} roundie_gauge_power_counters_t;

typedef struct {
    int64_t time_us;
    uint16_t voltage_mv;
    uint16_t remaining_mah;
    roundie_gauge_power_counters_t power;
} roundie_gauge_window_point_t;

typedef enum {
    ROUNDIE_GAUGE_WINDOW_NONE,
    ROUNDIE_GAUGE_WINDOW_OPEN,   /* on battery since the test started */
    ROUNDIE_GAUGE_WINDOW_CLOSED, /* external power returned */
} roundie_gauge_window_state_t;

typedef struct {
    roundie_gauge_window_state_t state;
    roundie_gauge_window_point_t start;
    bool protect_seen;
    roundie_gauge_window_point_t protect; /* first sample that wanted a protective sleep */
    roundie_gauge_window_point_t last;    /* last battery sample */
    uint16_t voltage_min_mv;
    uint16_t voltage_max_mv;
    uint32_t causes_seen; /* union of the wake causes seen in the window */
    uint32_t samples;
} roundie_gauge_window_t;

/* Call after roundie_gauge_hold_update(). Opens when the hold's test has
 * started, closes on external power and never reopens; a new arming clears
 * it (memset). Invalid samples only advance nothing. */
void roundie_gauge_window_update(roundie_gauge_window_t *window, const roundie_gauge_hold_t *hold,
                                 const roundie_gauge_learning_sample_t *sample,
                                 const roundie_gauge_power_counters_t *power, bool usb_link, int64_t now_us);

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

/* RAM telemetry ring for measurements without USB: one entry per gauge
 * sample, read over RG1 after the cable is reconnected. Lost on reset. */
#define ROUNDIE_GAUGE_LOG_CAPACITY 128U
#define ROUNDIE_GAUGE_LOG_VALID 0x01U     /* the gauge sample was valid */
#define ROUNDIE_GAUGE_LOG_USB 0x02U       /* USB Serial/JTAG saw host SOF frames */
#define ROUNDIE_GAUGE_LOG_INHIBITOR 0x04U /* the learning load hold held its inhibitor */
#define ROUNDIE_GAUGE_LOG_WAKE 0x08U      /* first sample after a completed light sleep */

typedef struct {
    uint32_t time_ms; /* since boot */
    uint16_t voltage_mv;
    int16_t current_ma;
    int16_t average_current_ma;
    int16_t temperature_deci_c;
    uint16_t remaining_mah;
    uint16_t full_mah;
    uint16_t operation_flags;
    uint16_t battery_flags;
    uint8_t hold_state; /* roundie_gauge_hold_state_t */
    uint8_t flags;      /* ROUNDIE_GAUGE_LOG_* */
} roundie_gauge_log_entry_t;

typedef struct {
    roundie_gauge_log_entry_t entries[ROUNDIE_GAUGE_LOG_CAPACITY];
    uint32_t total; /* entries ever pushed; the next sequence number */
} roundie_gauge_log_t;

void roundie_gauge_log_push(roundie_gauge_log_t *log, const roundie_gauge_learning_sample_t *sample,
                            roundie_gauge_hold_state_t hold_state, bool usb_connected, bool inhibitor_held,
                            bool after_wake);
/* The oldest sequence number still retained. */
uint32_t roundie_gauge_log_first(const roundie_gauge_log_t *log);
/* false when sequence was overwritten or not yet pushed. */
bool roundie_gauge_log_get(const roundie_gauge_log_t *log, uint32_t sequence, roundie_gauge_log_entry_t *entry);
