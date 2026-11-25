/* bq27220_config.c and gauge_restore.c against a BQ27220 model.
 *
 * The model follows TI SLUUBD4A and TI E2E 719878 as far as the procedure
 * uses them: ENTER_CFG_UPDATE works when UNSEALED or FULL ACCESS and is
 * ignored when SEALED; CFGUPDATE appears after a delay; a Data Memory write is
 * address and data to 0x3E followed by checksum and length to 0x60, applied
 * only when both match; EXIT_CFG_UPDATE_REINIT and EXIT_CFG_UPDATE clear
 * CFGUPDATE after a delay. It is a model of the documented behavior, not of the
 * device: device behavior stays a device test. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bq27220_config.h"
#include "roundie_gauge_learning.h"
#include "sensor_codec.h"

#define DM_BASE 0x9180
#define DM_SIZE 0x200
#define MAX_COMMANDS 64
#define MAX_STATUS_READS 64

#define SECURITY_FULL 0x0002
#define SECURITY_UNSEALED 0x0004
#define SECURITY_SEALED 0x0006

typedef enum {
    FAULT_NONE,
    FAULT_LOST,    /* the transfer fails and has no effect */
    FAULT_APPLIED, /* the transfer takes effect but reports an error */
} fault_kind_t;

typedef struct {
    uint8_t dm[DM_SIZE];
    uint16_t battery_flags;
    uint16_t security;
    bool initcomp;
    bool cfgupdate;
    int design_command_override; /* -1: DesignCapacity() reads Data Memory */
    uint32_t now_ms;
    /* Pending CFGUPDATE change: 0 none, 1 enter, 2 exit with reinit, 3 exit */
    int pending;
    uint32_t pending_at_ms;
    uint32_t enter_delay_ms;
    uint32_t exit_delay_ms;
    bool enter_never;
    bool exit_never;
    bool ignore_commit;
    uint16_t selected;
    uint8_t staged[40];
    size_t staged_length;
    /* Fault injection by transaction number (reads and writes, from 1). */
    int transactions;
    int fault_at;
    fault_kind_t fault_kind;
    /* What the procedure did. */
    uint16_t commands[MAX_COMMANDS];
    int command_count;
    uint32_t command_at_ms[MAX_COMMANDS];
    uint32_t status_read_at_ms[MAX_STATUS_READS];
    int status_read_count;
    int commits;
} gauge_model_t;

static void put_be(gauge_model_t *gauge, uint16_t address, uint16_t value)
{
    gauge->dm[address - DM_BASE] = (uint8_t)(value >> 8);
    gauge->dm[address - DM_BASE + 1] = (uint8_t)(value & 0xff);
}

static uint16_t get_be(const gauge_model_t *gauge, uint16_t address)
{
    return (uint16_t)((gauge->dm[address - DM_BASE] << 8) | gauge->dm[address - DM_BASE + 1]);
}

/* TI defaults (TRM 6.1), as read on the 1.85B unit for DC/FCC. */
static void model_reset(gauge_model_t *gauge, uint16_t security)
{
    memset(gauge, 0, sizeof(*gauge));
    for (size_t i = 0; i < DM_SIZE; i++) {
        gauge->dm[i] = (uint8_t)(i * 7 + 3);
    }
    put_be(gauge, 0x929b, 0x102a);
    put_be(gauge, 0x929d, 3000);
    put_be(gauge, 0x929f, 3000);
    put_be(gauge, 0x92a3, 3700);
    put_be(gauge, 0x91fd, 4200);
    put_be(gauge, 0x9201, 100);
    put_be(gauge, 0x926b, 200);
    put_be(gauge, 0x9251, 700);
    gauge->dm[0x925b - DM_BASE] = 119;
    put_be(gauge, 0x9264, 1500);
    put_be(gauge, 0x926d, 0);
    put_be(gauge, 0x927f, 0x0c8c);
    gauge->dm[0x9281 - DM_BASE] = 0x8c;
    gauge->battery_flags = 0x5028;
    gauge->security = security;
    gauge->initcomp = true;
    gauge->design_command_override = -1;
    gauge->enter_delay_ms = 1100;
    gauge->exit_delay_ms = 600;
}

static uint16_t operation_flags(const gauge_model_t *gauge)
{
    uint16_t flags = (uint16_t)(0x0080 | gauge->security);
    if (gauge->initcomp) {
        flags |= 0x0020;
    }
    if (gauge->cfgupdate) {
        flags |= 0x0400;
    }
    return flags;
}

static void model_advance(gauge_model_t *gauge, uint32_t milliseconds)
{
    gauge->now_ms += milliseconds;
    if (gauge->pending != 0 && gauge->now_ms >= gauge->pending_at_ms) {
        if (gauge->pending == 1) {
            gauge->cfgupdate = true;
            gauge->initcomp = false;
        } else {
            gauge->cfgupdate = false;
            gauge->initcomp = true;
        }
        gauge->pending = 0;
    }
}

/* Returns the fault for this transaction. */
static fault_kind_t next_transaction(gauge_model_t *gauge)
{
    gauge->transactions++;
    return gauge->transactions == gauge->fault_at ? gauge->fault_kind : FAULT_NONE;
}

static void run_subcommand(gauge_model_t *gauge, uint16_t subcommand)
{
    assert(gauge->command_count < MAX_COMMANDS);
    gauge->command_at_ms[gauge->command_count] = gauge->now_ms;
    gauge->commands[gauge->command_count++] = subcommand;
    bool unsealed = gauge->security != SECURITY_SEALED;
    if (subcommand == 0x0090 && unsealed && !gauge->cfgupdate && !gauge->enter_never) {
        gauge->pending = 1;
        gauge->pending_at_ms = gauge->now_ms + gauge->enter_delay_ms;
    } else if ((subcommand == 0x0091 || subcommand == 0x0092) && gauge->cfgupdate && !gauge->exit_never) {
        gauge->pending = subcommand == 0x0091 ? 2 : 3;
        gauge->pending_at_ms = gauge->now_ms + gauge->exit_delay_ms;
    }
}

static void commit(gauge_model_t *gauge, uint8_t sum, uint8_t length)
{
    if (!gauge->cfgupdate || gauge->ignore_commit || gauge->staged_length < 3) {
        return;
    }
    uint16_t address = (uint16_t)(gauge->staged[0] | (gauge->staged[1] << 8));
    size_t data_length = gauge->staged_length - 2;
    if (length != gauge->staged_length + 2 ||
        sum != roundie_gauge_block_checksum(address, &gauge->staged[2], data_length)) {
        return;
    }
    memcpy(&gauge->dm[address - DM_BASE], &gauge->staged[2], data_length);
    gauge->commits++;
}

static esp_err_t model_write(void *context, uint8_t command, const uint8_t *data, size_t length)
{
    gauge_model_t *gauge = context;
    fault_kind_t fault = next_transaction(gauge);
    if (fault == FAULT_LOST) {
        return ESP_FAIL;
    }
    if (command == 0x00 && length == 2) {
        run_subcommand(gauge, (uint16_t)(data[0] | (data[1] << 8)));
    } else if (command == 0x3e && length >= 2 && length <= sizeof(gauge->staged)) {
        gauge->selected = (uint16_t)(data[0] | (data[1] << 8));
        memcpy(gauge->staged, data, length);
        gauge->staged_length = length;
    } else if (command == 0x60 && length == 2) {
        commit(gauge, data[0], data[1]);
    } else {
        assert(!"unexpected write");
    }
    return fault == FAULT_APPLIED ? ESP_FAIL : ESP_OK;
}

static esp_err_t model_read(void *context, uint8_t command, uint8_t *data, size_t length)
{
    gauge_model_t *gauge = context;
    if (next_transaction(gauge) != FAULT_NONE) {
        return ESP_FAIL;
    }
    uint16_t value = 0;
    if (command == 0x0a) {
        value = gauge->battery_flags;
    } else if (command == 0x3a) {
        assert(gauge->status_read_count < MAX_STATUS_READS);
        gauge->status_read_at_ms[gauge->status_read_count++] = gauge->now_ms;
        value = operation_flags(gauge);
    } else if (command == 0x3c) {
        value = gauge->design_command_override >= 0 ? (uint16_t)gauge->design_command_override :
                                                      get_be(gauge, 0x929f);
    } else if (command == 0x3e) {
        value = gauge->selected;
    } else if (command == 0x40 && length == 32) {
        memcpy(data, &gauge->dm[gauge->selected - DM_BASE], 32);
        return ESP_OK;
    } else if (command == 0x60 && length == 2) {
        data[0] = roundie_gauge_block_checksum(gauge->selected, &gauge->dm[gauge->selected - DM_BASE], 32);
        data[1] = 36;
        return ESP_OK;
    } else {
        assert(!"unexpected read");
    }
    assert(length == 2);
    data[0] = (uint8_t)(value & 0xff);
    data[1] = (uint8_t)(value >> 8);
    return ESP_OK;
}

static void model_delay(void *context, uint32_t milliseconds)
{
    model_advance(context, milliseconds);
}

static roundie_bq27220_io_t io_for(gauge_model_t *gauge)
{
    const roundie_bq27220_io_t io = {
        .context = gauge,
        .read = model_read,
        .write = model_write,
        .delay_ms = model_delay,
    };
    return io;
}

static bool command_sent(const gauge_model_t *gauge, uint16_t subcommand)
{
    for (int i = 0; i < gauge->command_count; i++) {
        if (gauge->commands[i] == subcommand) {
            return true;
        }
    }
    return false;
}

/* No access keys, seal, reset or other subcommand: only the three CFGUPDATE ones. */
static void assert_allowed_commands(const gauge_model_t *gauge)
{
    for (int i = 0; i < gauge->command_count; i++) {
        uint16_t subcommand = gauge->commands[i];
        assert(subcommand == 0x0090 || subcommand == 0x0091 || subcommand == 0x0092);
    }
}

/* At most two OperationStatus() reads per second; the flag is read no earlier
 * than 2 s after ENTER_CFG_UPDATE. */
static void assert_status_timing(const gauge_model_t *gauge)
{
    for (int i = 1; i < gauge->status_read_count; i++) {
        assert(gauge->status_read_at_ms[i] - gauge->status_read_at_ms[i - 1] >= 500);
    }
    for (int i = 0; i < gauge->command_count; i++) {
        if (gauge->commands[i] != 0x0090) {
            continue;
        }
        for (int j = 0; j < gauge->status_read_count; j++) {
            if (gauge->status_read_at_ms[j] >= gauge->command_at_ms[i]) {
                assert(gauge->status_read_at_ms[j] - gauge->command_at_ms[i] >= 2000);
                break;
            }
        }
    }
}

static void assert_unchanged(const gauge_model_t *gauge, const gauge_model_t *before)
{
    assert(memcmp(gauge->dm, before->dm, DM_SIZE) == 0);
}

/* Everything except FCC and Design Capacity is as before. */
static void assert_only_capacity_changed(const gauge_model_t *gauge, const gauge_model_t *before)
{
    for (uint16_t address = DM_BASE; address < DM_BASE + DM_SIZE; address++) {
        if (address >= 0x929d && address < 0x92a1) {
            continue;
        }
        assert(gauge->dm[address - DM_BASE] == before->dm[address - DM_BASE]);
    }
}

static void test_checksum_vector(void)
{
    /* TI E2E 719878: 0x929F <- 0x0F 0xCB is committed as 0xF4, length 6. */
    const uint8_t data[2] = {0x0f, 0xcb};
    assert(roundie_gauge_block_checksum(0x929f, data, sizeof(data)) == 0xf4);
}

static void test_initialize(uint16_t security)
{
    gauge_model_t gauge;
    model_reset(&gauge, security);
    gauge_model_t before = gauge;
    roundie_bq27220_io_t io = io_for(&gauge);
    roundie_gauge_capacity_t state;
    assert(roundie_bq27220_initialize(&io, 600, 600, &state) == ESP_OK);
    assert(state.step == ROUNDIE_GAUGE_STEP_NONE && state.written);
    assert(state.design_mah == 600 && state.dm_design_mah == 600 && state.dm_full_mah == 600);
    assert(state.gauging_config == 0x102a && state.design_voltage_mv == 3700);
    assert(state.charging_voltage_mv == 4200 && state.taper_current_ma == 100 && state.near_full_mah == 200);
    assert(get_be(&gauge, 0x929d) == 600 && get_be(&gauge, 0x929f) == 600);
    assert_only_capacity_changed(&gauge, &before);
    assert(gauge.commits == 1 && !gauge.cfgupdate && gauge.initcomp);
    assert(gauge.command_count == 2 && gauge.commands[0] == 0x0090 && gauge.commands[1] == 0x0091);
    assert_allowed_commands(&gauge);
    assert_status_timing(&gauge);
    assert(gauge.now_ms < 9000);
}

static void test_read_only(void)
{
    gauge_model_t gauge;
    model_reset(&gauge, SECURITY_SEALED);
    gauge_model_t before = gauge;
    roundie_bq27220_io_t io = io_for(&gauge);
    roundie_gauge_capacity_t state;
    memset(&state, 0, sizeof(state));
    assert(roundie_bq27220_read(&io, &state) == ESP_OK);
    assert(state.design_mah == 3000 && state.dm_full_mah == 3000 && state.dm_design_mah == 3000);
    assert(state.operation_flags == 0x00a6 && state.battery_flags == 0x5028);
    assert(state.battery_low_centi == 700 && state.learning_low_temp_deci_c == 119);
    assert(state.overload_ma == 1500 && state.reserve_mah == 0);
    assert(state.flag_config_a == 0x0c8c && state.flag_config_b == 0x8c);
    assert(gauge.command_count == 0 && gauge.commits == 0);
    assert_unchanged(&gauge, &before);
}

static void expect_refusal(gauge_model_t *gauge, uint16_t target, esp_err_t expected)
{
    gauge_model_t before = *gauge;
    roundie_bq27220_io_t io = io_for(gauge);
    roundie_gauge_capacity_t state;
    assert(roundie_bq27220_initialize(&io, target, 600, &state) == expected);
    assert(state.step == ROUNDIE_GAUGE_STEP_PRECHECK && !state.written);
    assert(gauge->command_count == 0 && gauge->commits == 0);
    assert_unchanged(gauge, &before);
}

static void test_refusals(void)
{
    gauge_model_t gauge;
    model_reset(&gauge, SECURITY_SEALED);
    expect_refusal(&gauge, 600, ESP_ERR_INVALID_STATE);

    model_reset(&gauge, SECURITY_UNSEALED);
    gauge.battery_flags = 0x5020;
    expect_refusal(&gauge, 600, ESP_ERR_INVALID_STATE);

    model_reset(&gauge, SECURITY_UNSEALED);
    gauge.initcomp = false;
    expect_refusal(&gauge, 600, ESP_ERR_INVALID_STATE);

    /* Interrupted earlier: the gauge is still in CFGUPDATE. Not resumed. */
    model_reset(&gauge, SECURITY_UNSEALED);
    gauge.cfgupdate = true;
    expect_refusal(&gauge, 600, ESP_ERR_INVALID_STATE);

    model_reset(&gauge, SECURITY_UNSEALED);
    expect_refusal(&gauge, 99, ESP_ERR_INVALID_STATE);
    expect_refusal(&gauge, 6001, ESP_ERR_INVALID_STATE);

    /* Not the reset default: a profile nobody here configured. */
    model_reset(&gauge, SECURITY_UNSEALED);
    put_be(&gauge, 0x929f, 500);
    expect_refusal(&gauge, 600, ESP_ERR_INVALID_RESPONSE);

    model_reset(&gauge, SECURITY_UNSEALED);
    gauge.design_command_override = 2000;
    expect_refusal(&gauge, 600, ESP_ERR_INVALID_RESPONSE);
}

static void test_keep_learned_capacity(void)
{
    gauge_model_t gauge;
    model_reset(&gauge, SECURITY_UNSEALED);
    put_be(&gauge, 0x929f, 600);
    put_be(&gauge, 0x929d, 575);
    gauge_model_t before = gauge;
    roundie_bq27220_io_t io = io_for(&gauge);
    roundie_gauge_capacity_t state;
    assert(roundie_bq27220_initialize(&io, 600, 600, &state) == ESP_OK);
    assert(state.step == ROUNDIE_GAUGE_STEP_NONE && !state.written);
    assert(state.dm_full_mah == 575 && state.dm_design_mah == 600);
    assert(gauge.command_count == 0 && gauge.commits == 0);
    assert_unchanged(&gauge, &before);
}

/* A read override that corrupts MACDataSum. */
static esp_err_t corrupt_sum_read(void *context, uint8_t command, uint8_t *data, size_t length)
{
    esp_err_t error = model_read(context, command, data, length);
    if (error == ESP_OK && command == 0x60) {
        data[0] ^= 0x01;
    }
    return error;
}

/* A read override whose echo never shows the selected address. */
static esp_err_t stale_echo_read(void *context, uint8_t command, uint8_t *data, size_t length)
{
    esp_err_t error = model_read(context, command, data, length);
    if (error == ESP_OK && command == 0x3e) {
        data[0] = 0;
        data[1] = 0;
    }
    return error;
}

static void test_block_read_failures(void)
{
    gauge_model_t gauge;
    model_reset(&gauge, SECURITY_UNSEALED);
    gauge_model_t before = gauge;
    roundie_bq27220_io_t io = io_for(&gauge);
    io.read = corrupt_sum_read;
    roundie_gauge_capacity_t state;
    assert(roundie_bq27220_initialize(&io, 600, 600, &state) == ESP_ERR_INVALID_CRC);
    assert(state.step == ROUNDIE_GAUGE_STEP_READ_BLOCK && gauge.command_count == 0);
    assert_unchanged(&gauge, &before);

    model_reset(&gauge, SECURITY_UNSEALED);
    io = io_for(&gauge);
    io.read = stale_echo_read;
    assert(roundie_bq27220_initialize(&io, 600, 600, &state) == ESP_ERR_TIMEOUT);
    assert(state.step == ROUNDIE_GAUGE_STEP_READ_BLOCK && gauge.command_count == 0);
    assert_unchanged(&gauge, &before);
}

static void test_enter_not_confirmed(void)
{
    gauge_model_t gauge;
    model_reset(&gauge, SECURITY_UNSEALED);
    gauge.enter_never = true;
    gauge_model_t before = gauge;
    roundie_bq27220_io_t io = io_for(&gauge);
    roundie_gauge_capacity_t state;
    assert(roundie_bq27220_initialize(&io, 600, 600, &state) == ESP_ERR_TIMEOUT);
    assert(state.step == ROUNDIE_GAUGE_STEP_ENTER && !state.written);
    assert(gauge.commits == 0 && !gauge.cfgupdate && gauge.command_count == 1);
    assert_unchanged(&gauge, &before);
    assert_status_timing(&gauge);

    /* CFGUPDATE arrives after the timeout: the procedure has given up, sees
     * it on the way out and leaves without reinitializing. */
    model_reset(&gauge, SECURITY_UNSEALED);
    gauge.enter_delay_ms = 4200;
    io = io_for(&gauge);
    assert(roundie_bq27220_initialize(&io, 600, 600, &state) == ESP_ERR_TIMEOUT);
    assert(state.step == ROUNDIE_GAUGE_STEP_ENTER && !state.written && gauge.commits == 0);
    assert(command_sent(&gauge, 0x0092) && !command_sent(&gauge, 0x0091));
    assert(!gauge.cfgupdate && gauge.initcomp);
    assert_unchanged(&gauge, &before);
    assert_status_timing(&gauge);
}

static void test_commit_not_applied(void)
{
    gauge_model_t gauge;
    model_reset(&gauge, SECURITY_UNSEALED);
    gauge.ignore_commit = true;
    gauge_model_t before = gauge;
    roundie_bq27220_io_t io = io_for(&gauge);
    roundie_gauge_capacity_t state;
    assert(roundie_bq27220_initialize(&io, 600, 600, &state) == ESP_ERR_INVALID_RESPONSE);
    assert(state.step == ROUNDIE_GAUGE_STEP_VERIFY_CFG && state.written);
    assert(command_sent(&gauge, 0x0092) && !command_sent(&gauge, 0x0091));
    assert(!gauge.cfgupdate && (state.operation_flags & 0x0400) == 0);
    assert_unchanged(&gauge, &before);
    assert_allowed_commands(&gauge);
    assert_status_timing(&gauge);
}

static void test_exit_not_confirmed(void)
{
    gauge_model_t gauge;
    model_reset(&gauge, SECURITY_UNSEALED);
    gauge.exit_never = true;
    roundie_bq27220_io_t io = io_for(&gauge);
    roundie_gauge_capacity_t state;
    assert(roundie_bq27220_initialize(&io, 600, 600, &state) == ESP_ERR_TIMEOUT);
    assert(state.step == ROUNDIE_GAUGE_STEP_EXIT && state.written);
    /* Reported, not hidden: the gauge is still in CFGUPDATE (it leaves it by
     * itself after about 240 s, TRM 8.1.4). */
    assert(gauge.cfgupdate && (state.operation_flags & 0x0400) != 0);
    assert(command_sent(&gauge, 0x0091) && command_sent(&gauge, 0x0092));
    assert_allowed_commands(&gauge);
    assert_status_timing(&gauge);
}

/* One failed transfer at every position, lost or applied: Data Memory holds
 * the old or the new capacity (never a mix), other values are unchanged, the
 * gauge is out of CFGUPDATE and success is reported only when verified. */
static void test_single_fault_everywhere(void)
{
    gauge_model_t clean;
    model_reset(&clean, SECURITY_UNSEALED);
    roundie_bq27220_io_t io = io_for(&clean);
    roundie_gauge_capacity_t state;
    assert(roundie_bq27220_initialize(&io, 600, 600, &state) == ESP_OK);
    int total = clean.transactions;
    assert(total > 20);
    for (int kind = FAULT_LOST; kind <= FAULT_APPLIED; kind++) {
        for (int at = 1; at <= total; at++) {
            gauge_model_t gauge;
            model_reset(&gauge, SECURITY_UNSEALED);
            gauge.fault_at = at;
            gauge.fault_kind = (fault_kind_t)kind;
            gauge_model_t before = gauge;
            io = io_for(&gauge);
            esp_err_t error = roundie_bq27220_initialize(&io, 600, 600, &state);
            uint16_t full = get_be(&gauge, 0x929d);
            uint16_t design = get_be(&gauge, 0x929f);
            assert((full == 3000 && design == 3000) || (full == 600 && design == 600));
            assert_only_capacity_changed(&gauge, &before);
            assert(!gauge.cfgupdate);
            assert_allowed_commands(&gauge);
            assert_status_timing(&gauge);
            if (error == ESP_OK) {
                assert(full == 600 && state.step == ROUNDIE_GAUGE_STEP_NONE);
            } else {
                assert(state.step != ROUNDIE_GAUGE_STEP_NONE);
            }
            if (!state.written) {
                assert(full == 3000 && gauge.commits == 0);
            }
            if (full == 600 && !command_sent(&gauge, 0x0091)) {
                /* Written but not reinitialized: reported as a failure. */
                assert(error != ESP_OK);
            }
        }
    }
}

static void test_restore(void)
{
    roundie_gauge_restore_t restore = {0};
    /* Disabled, not ready, or unknown capacity: nothing. */
    assert(!roundie_gauge_restore_due(&restore, 0, true, true, true, 3000));
    assert(!roundie_gauge_restore_due(&restore, 600, true, false, true, 3000));
    assert(!roundie_gauge_restore_due(&restore, 600, true, true, false, 3000));
    assert(restore.attempts == 0);
    /* Configured: never written again (learned FCC kept). */
    assert(!roundie_gauge_restore_due(&restore, 600, true, true, true, 600));
    /* After a gauge reset: exactly one attempt, even if it fails. */
    assert(roundie_gauge_restore_due(&restore, 600, true, true, true, 3000));
    roundie_gauge_restore_done(&restore, ESP_ERR_TIMEOUT);
    assert(restore.last_error == ESP_ERR_TIMEOUT);
    for (int i = 0; i < 10; i++) {
        assert(!roundie_gauge_restore_due(&restore, 600, true, true, true, 3000));
    }
    assert(restore.attempts == 1);
    /* Battery out and back: a new gauge reset, one new attempt. */
    assert(!roundie_gauge_restore_due(&restore, 600, false, true, true, 0));
    assert(roundie_gauge_restore_due(&restore, 600, true, true, true, 3000));
    roundie_gauge_restore_done(&restore, ESP_OK);
    assert(!roundie_gauge_restore_due(&restore, 600, true, true, true, 600));
    /* Lost again later without a reported absence: re-armed by having seen
     * the target, so one more attempt. */
    assert(roundie_gauge_restore_due(&restore, 600, true, true, true, 3000));
    assert(restore.attempts == 3);
    assert(!roundie_gauge_restore_due(NULL, 600, true, true, true, 3000));
}

/* After a gauge reset, the learned FCC from the record is written with the
 * design capacity; a later MCU start keeps it (no second write). */
static void test_restore_learned_full(void)
{
    gauge_model_t gauge;
    model_reset(&gauge, SECURITY_UNSEALED);
    roundie_bq27220_io_t io = io_for(&gauge);
    roundie_gauge_capacity_t state;

    /* First configuration, then the profile identity the record keeps. */
    assert(roundie_bq27220_initialize(&io, 600, 600, &state) == ESP_OK);
    const uint32_t configured_profile = roundie_gauge_profile_id(&state);
    roundie_gauge_record_t record;
    roundie_gauge_record_make(&record, 600, configured_profile, 560, 1);

    /* Gauge power-on reset: OTP defaults again, the same profile id. */
    model_reset(&gauge, SECURITY_UNSEALED);
    gauge_model_t before = gauge;
    memset(&state, 0, sizeof(state));
    assert(roundie_bq27220_read(&io, &state) == ESP_OK);
    assert(state.design_mah == 3000);
    assert(roundie_gauge_profile_id(&state) == configured_profile);
    uint16_t full = roundie_gauge_record_restore_full(&record, sizeof(record), 600, roundie_gauge_profile_id(&state));
    assert(full == 560);
    assert(roundie_bq27220_initialize(&io, 600, full, &state) == ESP_OK);
    assert(get_be(&gauge, 0x929d) == 560 && get_be(&gauge, 0x929f) == 600);
    assert(state.dm_full_mah == 560 && state.design_mah == 600);
    assert_only_capacity_changed(&gauge, &before);

    /* MCU restart: Design Capacity is the target, nothing is written even
     * though the FCC differs from it (the learned FCC is not overwritten). */
    gauge.command_count = 0;
    gauge.commits = 0;
    assert(roundie_bq27220_initialize(&io, 600, 600, &state) == ESP_OK);
    assert(gauge.command_count == 0 && gauge.commits == 0);
    assert(get_be(&gauge, 0x929d) == 560);

    /* A corrupted record falls back to the design value. */
    record.learned_full_mah ^= 1;
    model_reset(&gauge, SECURITY_UNSEALED);
    assert(roundie_bq27220_read(&io, &state) == ESP_OK);
    assert(roundie_gauge_record_restore_full(&record, sizeof(record), 600, roundie_gauge_profile_id(&state)) == 600);

    /* A changed gauge profile (other OTP defaults) discards the record. */
    roundie_gauge_record_make(&record, 600, configured_profile, 560, 2);
    model_reset(&gauge, SECURITY_UNSEALED);
    put_be(&gauge, 0x926b, 40);
    assert(roundie_bq27220_read(&io, &state) == ESP_OK);
    assert(roundie_gauge_record_restore_full(&record, sizeof(record), 600, roundie_gauge_profile_id(&state)) == 600);
}

/* An FCC out of range is refused before any transaction. */
static void test_full_out_of_range(void)
{
    gauge_model_t gauge;
    model_reset(&gauge, SECURITY_UNSEALED);
    gauge_model_t before = gauge;
    roundie_bq27220_io_t io = io_for(&gauge);
    roundie_gauge_capacity_t state;
    assert(roundie_bq27220_initialize(&io, 600, 50, &state) == ESP_ERR_INVALID_ARG);
    assert(roundie_bq27220_initialize(&io, 600, 7000, &state) == ESP_ERR_INVALID_ARG);
    assert(state.step == ROUNDIE_GAUGE_STEP_PRECHECK && !state.written);
    assert(gauge.command_count == 0 && gauge.commits == 0 && gauge.now_ms == 0);
    assert_unchanged(&gauge, &before);
}

int main(void)
{
    test_checksum_vector();
    test_read_only();
    test_initialize(SECURITY_UNSEALED);
    test_initialize(SECURITY_FULL);
    test_refusals();
    test_keep_learned_capacity();
    test_block_read_failures();
    test_enter_not_confirmed();
    test_commit_not_applied();
    test_exit_not_confirmed();
    test_single_fault_everywhere();
    test_restore();
    test_restore_learned_full();
    test_full_out_of_range();
    printf("bq27220: OK\n");
    return 0;
}
