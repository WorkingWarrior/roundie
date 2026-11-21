#pragma once

/* Fuel gauge capacity configuration shared by the board layer, the system
 * worker and host tests. No driver types. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* The step a capacity request stopped at; NONE on success. */
enum {
    ROUNDIE_GAUGE_STEP_NONE = 0,
    ROUNDIE_GAUGE_STEP_PRECHECK = 1,   /* flags, DesignCapacity() or refusal */
    ROUNDIE_GAUGE_STEP_READ_BLOCK = 2, /* Data Memory read or its checksum */
    ROUNDIE_GAUGE_STEP_ENTER = 3,      /* ENTER_CFG_UPDATE not confirmed */
    ROUNDIE_GAUGE_STEP_WRITE = 4,      /* block write or commit */
    ROUNDIE_GAUGE_STEP_VERIFY_CFG = 5, /* readback inside CFGUPDATE */
    ROUNDIE_GAUGE_STEP_EXIT = 6,       /* EXIT_CFG_UPDATE_REINIT not confirmed */
    ROUNDIE_GAUGE_STEP_VERIFY = 7,     /* readback after reinitialization */
};

/* What the gauge reports (1.85B BQ27220, CEDV profile 1). Data Memory values
 * are raw: the gauge's RAM copy, restored from OTP at a gauge power-on reset.
 *   written   the commit (checksum and length) of new values was sent */
typedef struct {
    uint16_t design_mah;          /* DesignCapacity() 0x3C */
    uint16_t dm_full_mah;         /* Full Charge Capacity 0x929D (learned) */
    uint16_t dm_design_mah;       /* Design Capacity 0x929F */
    uint16_t gauging_config;      /* CEDV Gauging Configuration 0x929B */
    uint16_t design_voltage_mv;   /* Design Voltage 0x92A3 */
    uint16_t charging_voltage_mv; /* Charging Voltage 0x91FD */
    uint16_t taper_current_ma;    /* Taper Current 0x9201 */
    uint16_t near_full_mah;       /* Near Full 0x926B */
    uint16_t battery_flags;
    uint16_t operation_flags;
    uint8_t step;
    bool written;
} roundie_gauge_capacity_t;

/* Restoring a configured capacity after the gauge lost its RAM (battery
 * removed). One attempt per loss: the attempt is re-armed only after the
 * gauge reported the target, or the battery was absent. A capacity already at
 * the target is never written again, so the learned FCC is kept. */
typedef struct {
    bool attempted;
    uint32_t attempts;
    esp_err_t last_error;
} roundie_gauge_restore_t;

/* True when the caller should run one capacity initialization now.
 *   target_mah    0 disables restoring
 *   present       a battery is reported present
 *   ready         the gauge is initialized and the system may spend the time
 *   design_valid  design_mah was read */
bool roundie_gauge_restore_due(roundie_gauge_restore_t *restore, uint16_t target_mah,
                               bool present, bool ready, bool design_valid,
                               uint16_t design_mah);
void roundie_gauge_restore_done(roundie_gauge_restore_t *restore, esp_err_t error);
