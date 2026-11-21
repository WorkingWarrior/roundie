#pragma once

/* BQ27220 capacity configuration: Data Memory reads and the one-time Design
 * Capacity / Full Charge Capacity initialization. Hardware-independent: the
 * board supplies the I2C transfers and the delay; host tests supply a model.
 *
 * Sources: TI SLUUBD4A (BQ27220 TRM) 2.2.21-2.2.23, 2.30-2.31, 3.1, 6.1 and
 * TI E2E thread 719878 (Data Memory write by address: address and data to
 * 0x3E, then checksum and length to 0x60/0x61). ENTER_CFG_UPDATE needs
 * UNSEALED, not FULL ACCESS (TRM 2.2.21): no access keys are sent, the
 * gauge is never sealed, reset or put in another mode, OTP is never written. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "roundie_gauge.h"

typedef struct {
    void *context;
    esp_err_t (*read)(void *context, uint8_t command, uint8_t *data, size_t length);
    esp_err_t (*write)(void *context, uint8_t command, const uint8_t *data, size_t length);
    void (*delay_ms)(void *context, uint32_t milliseconds);
} roundie_bq27220_io_t;

/* Design Capacity after a gauge power-on reset: the TI default, also read on
 * the 1.85B unit (docs/bringup.md). Only this value is initialized. */
#define ROUNDIE_BQ27220_RESET_DESIGN_MAH 3000
#define ROUNDIE_BQ27220_CAPACITY_MIN_MAH 100
#define ROUNDIE_BQ27220_CAPACITY_MAX_MAH 6000

typedef enum {
    ROUNDIE_BQ27220_KEEP,           /* Design Capacity is the target: write nothing */
    ROUNDIE_BQ27220_INITIALIZE,     /* reset default: write Design Capacity and FCC */
    ROUNDIE_BQ27220_REFUSE_STATE,   /* sealed, no battery, not initialized, CFGUPDATE, bad target */
    ROUNDIE_BQ27220_REFUSE_PROFILE, /* unexpected Design Capacity or profile mismatch */
} roundie_bq27220_plan_t;

roundie_bq27220_plan_t roundie_bq27220_plan(const roundie_gauge_capacity_t *state,
                                            uint16_t target_mah);

/* Read only: flags, DesignCapacity() and three checksummed Data Memory
 * blocks. Sends no subcommand. */
esp_err_t roundie_bq27220_read(const roundie_bq27220_io_t *io, roundie_gauge_capacity_t *state);

/* Plans, then for INITIALIZE: ENTER_CFG_UPDATE, one write of FCC and Design
 * Capacity (0x929D, 4 bytes), readback, EXIT_CFG_UPDATE_REINIT, readback.
 * KEEP returns ESP_OK without writing. On a failure after ENTER the gauge is
 * taken out of CFGUPDATE with EXIT_CFG_UPDATE (no reinitialization) if it
 * reports CFGUPDATE. About 7 s at most, in io->delay_ms. */
esp_err_t roundie_bq27220_initialize(const roundie_bq27220_io_t *io, uint16_t target_mah,
                                     roundie_gauge_capacity_t *state);
