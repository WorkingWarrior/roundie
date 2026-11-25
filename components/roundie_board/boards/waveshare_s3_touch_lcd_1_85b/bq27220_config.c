#include "bq27220_config.h"

#include <string.h>

#include "sensor_codec.h"

#define CMD_CONTROL 0x00
#define CMD_BATTERY_STATUS 0x0a
#define CMD_OPERATION_STATUS 0x3a
#define CMD_DESIGN_CAPACITY 0x3c
#define CMD_MAC 0x3e
#define CMD_MAC_DATA 0x40
#define CMD_MAC_SUM 0x60

#define SUBCMD_ENTER_CFG_UPDATE 0x0090
#define SUBCMD_EXIT_CFG_UPDATE_REINIT 0x0091
#define SUBCMD_EXIT_CFG_UPDATE 0x0092

#define BATTERY_PRESENT 0x0008
#define OP_CFGUPDATE 0x0400
#define OP_INITCOMP 0x0020
#define OP_SECURITY_MASK 0x0006
#define OP_SECURITY_SEALED 0x0006

#define BLOCK_SIZE 32
#define BLOCK_READ_LENGTH (2 + BLOCK_SIZE + 2) /* address, data, sum, length */

/* Blocks read for the report; offsets are into the 32 bytes read there. */
#define BLOCK_CHARGE 0x91fb   /* +2 Charging Voltage, +6 Taper Current */
#define BLOCK_LEARNING 0x9248 /* +9 Battery Low %, +0x13 Learning Low Temp, +0x1C OverLoad Current */
#define BLOCK_GAUGING 0x9268  /* +3 Near Full, +5 Reserve Capacity, +0x17 Flag Config A, +0x19 Flag Config B */
#define BLOCK_PROFILE 0x929b  /* +0 Gauging Configuration, +2 FCC, +4 Design Capacity, +8 Design Voltage */
#define ADDRESS_FULL_CHARGE 0x929d /* FCC, then Design Capacity: one 4-byte write */

#define COMMAND_DELAY_MS 10
#define ECHO_POLLS 10
/* TRM 2.2.21: read the flag at least 2 s after ENTER_CFG_UPDATE. */
#define ENTER_FIRST_READ_MS 2000
#define ENTER_TIMEOUT_MS 4000
#define EXIT_FIRST_READ_MS 500
#define EXIT_TIMEOUT_MS 3000
/* At most two OperationStatus() reads per second while waiting. */
#define STATUS_POLL_MS 500

static uint16_t little_endian(const uint8_t raw[2])
{
    return (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8));
}

static uint16_t big_endian(const uint8_t raw[2])
{
    return (uint16_t)(((uint16_t)raw[0] << 8) | raw[1]);
}

static esp_err_t read_word(const roundie_bq27220_io_t *io, uint8_t command, uint16_t *value)
{
    uint8_t raw[2];
    esp_err_t error = io->read(io->context, command, raw, sizeof(raw));
    if (error == ESP_OK) {
        *value = little_endian(raw);
    }
    return error;
}

static esp_err_t control(const roundie_bq27220_io_t *io, uint16_t subcommand)
{
    const uint8_t data[2] = {(uint8_t)(subcommand & 0xff), (uint8_t)(subcommand >> 8)};
    esp_err_t error = io->write(io->context, CMD_CONTROL, data, sizeof(data));
    io->delay_ms(io->context, COMMAND_DELAY_MS);
    return error;
}

/* Selects the address, waits for the gauge to echo it, reads 32 bytes and
 * checks MACDataSum()/MACDataLen() against them. */
static esp_err_t read_block(const roundie_bq27220_io_t *io, uint16_t address,
                            uint8_t block[BLOCK_SIZE])
{
    const uint8_t select[2] = {(uint8_t)(address & 0xff), (uint8_t)(address >> 8)};
    esp_err_t error = io->write(io->context, CMD_MAC, select, sizeof(select));
    if (error != ESP_OK) {
        return error;
    }
    bool selected = false;
    for (int poll = 0; poll < ECHO_POLLS && !selected; poll++) {
        io->delay_ms(io->context, COMMAND_DELAY_MS);
        uint16_t echo;
        error = read_word(io, CMD_MAC, &echo);
        if (error != ESP_OK) {
            return error;
        }
        selected = echo == address;
    }
    if (!selected) {
        return ESP_ERR_TIMEOUT;
    }
    error = io->read(io->context, CMD_MAC_DATA, block, BLOCK_SIZE);
    if (error != ESP_OK) {
        return error;
    }
    uint8_t trailer[2];
    error = io->read(io->context, CMD_MAC_SUM, trailer, sizeof(trailer));
    if (error != ESP_OK) {
        return error;
    }
    if (trailer[1] != BLOCK_READ_LENGTH ||
        trailer[0] != roundie_gauge_block_checksum(address, block, BLOCK_SIZE)) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

/* Waits first_ms, then polls OperationStatus() until (flags & mask) == value. */
static esp_err_t wait_operation(const roundie_bq27220_io_t *io, uint16_t mask, uint16_t value,
                                uint32_t first_ms, uint32_t timeout_ms, uint16_t *flags)
{
    io->delay_ms(io->context, first_ms);
    for (uint32_t waited = first_ms;; waited += STATUS_POLL_MS) {
        esp_err_t error = read_word(io, CMD_OPERATION_STATUS, flags);
        if (error != ESP_OK) {
            return error;
        }
        if ((*flags & mask) == value) {
            return ESP_OK;
        }
        if (waited >= timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        io->delay_ms(io->context, STATUS_POLL_MS);
    }
}

/* After a failure: EXIT_CFG_UPDATE (no reinitialization) only if the gauge
 * reports CFGUPDATE, read first_ms after the failure. The last flags read are
 * reported. */
static void leave_cfgupdate(const roundie_bq27220_io_t *io, roundie_gauge_capacity_t *state,
                            uint32_t first_ms)
{
    uint16_t flags;
    io->delay_ms(io->context, first_ms);
    if (read_word(io, CMD_OPERATION_STATUS, &flags) != ESP_OK) {
        return;
    }
    state->operation_flags = flags;
    if ((flags & OP_CFGUPDATE) == 0) {
        return;
    }
    if (control(io, SUBCMD_EXIT_CFG_UPDATE) != ESP_OK) {
        return;
    }
    (void)wait_operation(io, OP_CFGUPDATE, 0, STATUS_POLL_MS, EXIT_TIMEOUT_MS,
                         &state->operation_flags);
}

static esp_err_t read_profile(const roundie_bq27220_io_t *io, roundie_gauge_capacity_t *state)
{
    uint8_t block[BLOCK_SIZE];
    esp_err_t error = read_block(io, BLOCK_PROFILE, block);
    if (error != ESP_OK) {
        return error;
    }
    state->gauging_config = big_endian(&block[0]);
    state->dm_full_mah = big_endian(&block[2]);
    state->dm_design_mah = big_endian(&block[4]);
    state->design_voltage_mv = big_endian(&block[8]);
    return ESP_OK;
}

esp_err_t roundie_bq27220_read(const roundie_bq27220_io_t *io, roundie_gauge_capacity_t *state)
{
    state->step = ROUNDIE_GAUGE_STEP_PRECHECK;
    esp_err_t error = read_word(io, CMD_BATTERY_STATUS, &state->battery_flags);
    if (error == ESP_OK) {
        error = read_word(io, CMD_OPERATION_STATUS, &state->operation_flags);
    }
    if (error == ESP_OK) {
        error = read_word(io, CMD_DESIGN_CAPACITY, &state->design_mah);
    }
    if (error != ESP_OK) {
        return error;
    }
    state->step = ROUNDIE_GAUGE_STEP_READ_BLOCK;
    error = read_profile(io, state);
    uint8_t block[BLOCK_SIZE];
    if (error == ESP_OK) {
        error = read_block(io, BLOCK_CHARGE, block);
        if (error == ESP_OK) {
            state->charging_voltage_mv = big_endian(&block[2]);
            state->taper_current_ma = big_endian(&block[6]);
        }
    }
    if (error == ESP_OK) {
        error = read_block(io, BLOCK_LEARNING, block);
        if (error == ESP_OK) {
            state->battery_low_centi = big_endian(&block[0x09]);
            state->learning_low_temp_deci_c = block[0x13];
            state->overload_ma = big_endian(&block[0x1c]);
        }
    }
    if (error == ESP_OK) {
        error = read_block(io, BLOCK_GAUGING, block);
        if (error == ESP_OK) {
            state->near_full_mah = big_endian(&block[3]);
            state->reserve_mah = big_endian(&block[5]);
            state->flag_config_a = big_endian(&block[0x17]);
            state->flag_config_b = block[0x19];
        }
    }
    if (error != ESP_OK) {
        return error;
    }
    state->step = ROUNDIE_GAUGE_STEP_NONE;
    return ESP_OK;
}

roundie_bq27220_plan_t roundie_bq27220_plan(const roundie_gauge_capacity_t *state,
                                            uint16_t target_mah)
{
    if (target_mah < ROUNDIE_BQ27220_CAPACITY_MIN_MAH || target_mah > ROUNDIE_BQ27220_CAPACITY_MAX_MAH ||
        (state->operation_flags & OP_SECURITY_MASK) == OP_SECURITY_SEALED ||
        (state->battery_flags & BATTERY_PRESENT) == 0 ||
        (state->operation_flags & OP_INITCOMP) == 0 ||
        (state->operation_flags & OP_CFGUPDATE) != 0) {
        return ROUNDIE_BQ27220_REFUSE_STATE;
    }
    if (state->dm_design_mah != state->design_mah) {
        /* The block read is not the active profile's. */
        return ROUNDIE_BQ27220_REFUSE_PROFILE;
    }
    if (state->dm_design_mah == target_mah) {
        /* Configured: the learned FCC stays as it is. */
        return ROUNDIE_BQ27220_KEEP;
    }
    if (state->dm_design_mah != ROUNDIE_BQ27220_RESET_DESIGN_MAH) {
        return ROUNDIE_BQ27220_REFUSE_PROFILE;
    }
    return ROUNDIE_BQ27220_INITIALIZE;
}

static esp_err_t fail_in_cfgupdate(const roundie_bq27220_io_t *io, roundie_gauge_capacity_t *state,
                                   esp_err_t error)
{
    leave_cfgupdate(io, state, STATUS_POLL_MS);
    return error;
}

esp_err_t roundie_bq27220_initialize(const roundie_bq27220_io_t *io, uint16_t target_mah,
                                     uint16_t full_mah, roundie_gauge_capacity_t *state)
{
    memset(state, 0, sizeof(*state));
    if (full_mah < ROUNDIE_BQ27220_CAPACITY_MIN_MAH || full_mah > ROUNDIE_BQ27220_CAPACITY_MAX_MAH) {
        state->step = ROUNDIE_GAUGE_STEP_PRECHECK;
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = roundie_bq27220_read(io, state);
    if (error != ESP_OK) {
        return error;
    }
    switch (roundie_bq27220_plan(state, target_mah)) {
    case ROUNDIE_BQ27220_KEEP:
        return ESP_OK;
    case ROUNDIE_BQ27220_REFUSE_STATE:
        state->step = ROUNDIE_GAUGE_STEP_PRECHECK;
        return ESP_ERR_INVALID_STATE;
    case ROUNDIE_BQ27220_REFUSE_PROFILE:
        state->step = ROUNDIE_GAUGE_STEP_PRECHECK;
        return ESP_ERR_INVALID_RESPONSE;
    case ROUNDIE_BQ27220_INITIALIZE:
        break;
    }

    state->step = ROUNDIE_GAUGE_STEP_ENTER;
    error = control(io, SUBCMD_ENTER_CFG_UPDATE);
    if (error != ESP_OK) {
        /* The gauge may have taken the subcommand: its flag is read no
         * earlier than 2 s after it. */
        leave_cfgupdate(io, state, ENTER_FIRST_READ_MS);
        return error;
    }
    error = wait_operation(io, OP_CFGUPDATE, OP_CFGUPDATE, ENTER_FIRST_READ_MS,
                           ENTER_TIMEOUT_MS, &state->operation_flags);
    if (error != ESP_OK) {
        return fail_in_cfgupdate(io, state, error);
    }

    /* One transaction from 0x3E: address (little-endian), then FCC and
     * Design Capacity (big-endian); the commit covers exactly these bytes. */
    state->step = ROUNDIE_GAUGE_STEP_WRITE;
    const uint8_t staged[6] = {
        (uint8_t)(ADDRESS_FULL_CHARGE & 0xff), (uint8_t)(ADDRESS_FULL_CHARGE >> 8),
        (uint8_t)(full_mah >> 8), (uint8_t)(full_mah & 0xff),
        (uint8_t)(target_mah >> 8), (uint8_t)(target_mah & 0xff),
    };
    error = io->write(io->context, CMD_MAC, staged, sizeof(staged));
    io->delay_ms(io->context, COMMAND_DELAY_MS);
    if (error != ESP_OK) {
        return fail_in_cfgupdate(io, state, error);
    }
    const uint8_t commit[2] = {
        roundie_gauge_block_checksum(ADDRESS_FULL_CHARGE, &staged[2], 4),
        (uint8_t)(sizeof(staged) + 2),
    };
    error = io->write(io->context, CMD_MAC_SUM, commit, sizeof(commit));
    io->delay_ms(io->context, COMMAND_DELAY_MS);
    /* Sent: even with an error reported, the gauge may have applied it. */
    state->written = true;
    if (error != ESP_OK) {
        return fail_in_cfgupdate(io, state, error);
    }

    state->step = ROUNDIE_GAUGE_STEP_VERIFY_CFG;
    error = read_profile(io, state);
    if (error == ESP_OK && (state->dm_full_mah != full_mah || state->dm_design_mah != target_mah)) {
        error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error != ESP_OK) {
        /* Not confirmed: leave without reinitializing on these values. */
        return fail_in_cfgupdate(io, state, error);
    }

    state->step = ROUNDIE_GAUGE_STEP_EXIT;
    error = control(io, SUBCMD_EXIT_CFG_UPDATE_REINIT);
    if (error == ESP_OK) {
        error = wait_operation(io, OP_CFGUPDATE | OP_INITCOMP, OP_INITCOMP, EXIT_FIRST_READ_MS,
                               EXIT_TIMEOUT_MS, &state->operation_flags);
    }
    if (error != ESP_OK) {
        return fail_in_cfgupdate(io, state, error);
    }

    state->step = ROUNDIE_GAUGE_STEP_VERIFY;
    io->delay_ms(io->context, STATUS_POLL_MS);
    error = roundie_bq27220_read(io, state);
    if (error == ESP_OK && (state->design_mah != target_mah || state->dm_design_mah != target_mah ||
                            state->dm_full_mah != full_mah)) {
        error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error != ESP_OK) {
        state->step = ROUNDIE_GAUGE_STEP_VERIFY;
    }
    return error;
}
