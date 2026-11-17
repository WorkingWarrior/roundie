#include "board_logic.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WIDTH 466
#define HEIGHT 466

static bool update(board_touch_filter_t *filter, board_touch_report_t report,
                   uint16_t raw_x, uint16_t raw_y, uint16_t *x, uint16_t *y)
{
    return board_touch_filter_update(filter, report, raw_x, raw_y, WIDTH, HEIGHT, x, y);
}

static void check_touch_filter(void)
{
    board_touch_filter_t filter = {0};
    uint16_t x = 7;
    uint16_t y = 9;

    /* Idle: no report and an unacknowledged report never start a touch, and
     * the outputs are left alone. */
    assert(!update(&filter, BOARD_TOUCH_NONE, 0, 0, &x, &y));
    assert(!update(&filter, BOARD_TOUCH_NO_ACK, 100, 100, &x, &y));
    assert(x == 7 && y == 9);

    /* A point is reported as is. */
    assert(update(&filter, BOARD_TOUCH_POINT, 100, 200, &x, &y));
    assert(x == 100 && y == 200);

    /* Unacknowledged reads hold the last point up to the limit, then release. */
    for (unsigned i = 0; i < BOARD_TOUCH_NO_ACK_LIMIT; i++) {
        x = 0;
        y = 0;
        assert(update(&filter, BOARD_TOUCH_NO_ACK, 300, 300, &x, &y));
        assert(x == 100 && y == 200);
    }
    assert(!update(&filter, BOARD_TOUCH_NO_ACK, 300, 300, &x, &y));
    assert(!filter.pressed);
    /* Released: further unacknowledged reads do not bring it back. */
    assert(!update(&filter, BOARD_TOUCH_NO_ACK, 300, 300, &x, &y));

    /* Each acknowledged point restarts the allowance: a long press with
     * occasional unacknowledged reads stays one press. */
    for (unsigned round = 0; round < 50; round++) {
        uint16_t px = (uint16_t)(10U + round);
        assert(update(&filter, BOARD_TOUCH_POINT, px, 20, &x, &y));
        assert(x == px && y == 20);
        for (unsigned i = 0; i < BOARD_TOUCH_NO_ACK_LIMIT; i++) {
            assert(update(&filter, BOARD_TOUCH_NO_ACK, 0, 0, &x, &y));
            assert(x == px && y == 20);
        }
    }

    /* An acknowledged report without a point releases at once. */
    assert(!update(&filter, BOARD_TOUCH_NONE, 0, 0, &x, &y));
    assert(filter.unacknowledged == 0);

    /* A bus error resets the filter. */
    assert(update(&filter, BOARD_TOUCH_POINT, 1, 1, &x, &y));
    board_touch_filter_reset(&filter);
    assert(!filter.pressed && filter.unacknowledged == 0);
    assert(!update(&filter, BOARD_TOUCH_NO_ACK, 0, 0, &x, &y));

    /* Edges and corners. The driver mirrors as x_max - x with x_max = 466,
     * so a raw 0 arrives as 466: clamped to the last panel pixel. */
    const uint16_t edges[][4] = {
        {0, 0, 0, 0},
        {465, 0, 465, 0},
        {0, 465, 0, 465},
        {465, 465, 465, 465},
        {466, 466, 465, 465},
        {466, 0, 465, 0},
        {0, 466, 0, 465},
        {UINT16_MAX, UINT16_MAX, 465, 465},
        {233, 233, 233, 233},
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        assert(update(&filter, BOARD_TOUCH_POINT, edges[i][0], edges[i][1], &x, &y));
        assert(x == edges[i][2] && y == edges[i][3]);
        /* A held clamped point stays clamped. */
        assert(update(&filter, BOARD_TOUCH_NO_ACK, 0, 0, &x, &y));
        assert(x == edges[i][2] && y == edges[i][3]);
    }
}

static void check_pmic(void)
{
    /* 0x00 bit 3: battery connected. */
    assert(board_pmic_battery_present((const uint8_t[]){0x08, 0x00}));
    assert(board_pmic_battery_present((const uint8_t[]){0xFF, 0x00}));
    assert(!board_pmic_battery_present((const uint8_t[]){0xF7, 0xFF}));
    assert(!board_pmic_battery_present((const uint8_t[]){0x00, 0xFF}));

    /* 0x01 bits 6..5: 01 charging, 10 discharging, 00 standby (also charge
     * complete), which is neither. */
    assert(board_pmic_charging((const uint8_t[]){0x08, 0x20}));
    assert(board_pmic_charging((const uint8_t[]){0x08, 0x33})); /* device: charging, CV */
    assert(!board_pmic_charging((const uint8_t[]){0x08, 0x00}));
    assert(!board_pmic_charging((const uint8_t[]){0x08, 0x14})); /* device: standby, done */
    assert(!board_pmic_charging((const uint8_t[]){0x08, 0x55})); /* device: discharging */
    assert(!board_pmic_charging((const uint8_t[]){0x08, 0x60}));
    assert(!board_pmic_discharging((const uint8_t[]){0x08, 0x00}));
    assert(!board_pmic_discharging((const uint8_t[]){0x08, 0x20}));
    assert(board_pmic_discharging((const uint8_t[]){0x08, 0x40}));
    assert(!board_pmic_discharging((const uint8_t[]){0x08, 0x60}));
    /* Bits outside 6..5 do not change the direction. */
    assert(board_pmic_discharging((const uint8_t[]){0x08, 0xDF}));
    assert(!board_pmic_discharging((const uint8_t[]){0x08, 0x9F}));

    /* 0x34..0x35: 13-bit millivolts, upper three bits ignored. */
    assert(board_pmic_battery_mv((const uint8_t[]){0x00, 0x00}) == 0);
    assert(board_pmic_battery_mv((const uint8_t[]){0x0F, 0xA0}) == 4000);
    assert(board_pmic_battery_mv((const uint8_t[]){0xEF, 0xA0}) == 4000);
    assert(board_pmic_battery_mv((const uint8_t[]){0x1F, 0xFF}) == 8191);
    assert(board_pmic_battery_mv((const uint8_t[]){0xFF, 0xFF}) == 8191);

    assert(!board_pmic_voltage_plausible(0)); /* ADC disabled */
    assert(board_pmic_voltage_plausible(1));
    assert(board_pmic_voltage_plausible(3700));
    assert(board_pmic_voltage_plausible(6000));
    assert(!board_pmic_voltage_plausible(6001));
    assert(!board_pmic_voltage_plausible(8191));

    assert(board_pmic_percent_valid(0));
    assert(board_pmic_percent_valid(100));
    assert(!board_pmic_percent_valid(101));
    assert(!board_pmic_percent_valid(0xFF));

    /* 0x92 bits 4..0: 0.5 V + 100 mV per step, 28 = 3.3 V, higher codes reserved. */
    assert(board_pmic_aldo1_mv(0x00) == 500);
    assert(board_pmic_aldo1_mv(0x1C) == 3300);
    assert(board_pmic_aldo1_mv(0xFC) == 3300);
    assert(board_pmic_aldo1_mv(0x1D) == 0);
    assert(board_pmic_aldo1_mv(0x1F) == 0);
}

/* A PMIC register file with per-register read and write faults. */
typedef struct {
    uint8_t registers[256];
    int read_error[256];
    int write_error[256];
    unsigned reads;
    unsigned writes;
    uint8_t last_write_reg;
} mock_pmic_t;

static int mock_read(void *context, uint8_t reg, uint8_t *value)
{
    mock_pmic_t *pmic = context;
    pmic->reads++;
    if (pmic->read_error[reg] != 0) {
        return pmic->read_error[reg];
    }
    *value = pmic->registers[reg];
    return 0;
}

static int mock_write(void *context, uint8_t reg, uint8_t value)
{
    mock_pmic_t *pmic = context;
    pmic->writes++;
    pmic->last_write_reg = reg;
    if (pmic->write_error[reg] != 0) {
        return pmic->write_error[reg];
    }
    pmic->registers[reg] = value;
    return 0;
}

#define NACK 0x103    /* ESP_ERR_INVALID_STATE, as a bus error stand-in */
#define TIMEOUT 0x107 /* ESP_ERR_TIMEOUT */

static mock_pmic_t pmic;
static const board_pmic_bus_t bus = {&pmic, mock_read, mock_write};

static void reset_pmic(void)
{
    memset(&pmic, 0, sizeof(pmic));
}

static void check_pmic_writes(void)
{
    /* Read-modify-write keeps every other bit and writes once. */
    reset_pmic();
    pmic.registers[0x30] = 0xA6;
    assert(board_pmic_set_bits(&bus, 0x30, 0x01) == 0);
    assert(pmic.registers[0x30] == 0xA7 && pmic.writes == 1 && pmic.last_write_reg == 0x30);
    /* Already set: no write. */
    assert(board_pmic_set_bits(&bus, 0x30, 0x01) == 0);
    assert(pmic.writes == 1);
    /* A failed read writes nothing and returns the bus error. */
    reset_pmic();
    pmic.read_error[0x30] = TIMEOUT;
    assert(board_pmic_set_bits(&bus, 0x30, 0x01) == TIMEOUT);
    assert(pmic.writes == 0 && pmic.registers[0x30] == 0);
    /* A failed write is returned. */
    reset_pmic();
    pmic.write_error[0x30] = NACK;
    assert(board_pmic_set_bits(&bus, 0x30, 0x01) == NACK);

    /* Start, all clear, ALDO1 at 3.3 V: three enable bits set, ALDO1's
     * voltage untouched, other bits kept. */
    board_pmic_start_t result;
    reset_pmic();
    pmic.registers[0x30] = 0x40;
    pmic.registers[0x68] = 0x02;
    pmic.registers[0x90] = 0x80;
    pmic.registers[0x92] = 0x1C;
    board_pmic_start(&bus, &result);
    assert(result.adc_error == 0 && result.detection_error == 0 && result.aldo1_read_error == 0);
    assert(result.aldo1_mv == 3300 && result.aldo1_enable_tried && result.aldo1_enable_error == 0);
    assert(pmic.registers[0x30] == 0x41 && pmic.registers[0x68] == 0x03);
    assert(pmic.registers[0x90] == 0x81 && pmic.registers[0x92] == 0x1C);
    assert(pmic.writes == 3);
    /* Everything already enabled (the device's state after a warm restart):
     * no write at all. */
    pmic.writes = 0;
    board_pmic_start(&bus, &result);
    assert(pmic.writes == 0 && result.aldo1_enable_tried && result.aldo1_enable_error == 0);

    /* ALDO1 at another voltage: left alone, no write to 0x90 or 0x92. */
    reset_pmic();
    pmic.registers[0x92] = 0x0D; /* 1.8 V */
    board_pmic_start(&bus, &result);
    assert(result.aldo1_mv == 1800 && !result.aldo1_enable_tried);
    assert(pmic.registers[0x90] == 0 && pmic.registers[0x92] == 0x0D && pmic.writes == 2);
    /* A reserved ALDO1 code: also left alone. */
    reset_pmic();
    pmic.registers[0x92] = 0x1F;
    board_pmic_start(&bus, &result);
    assert(result.aldo1_mv == 0 && !result.aldo1_enable_tried && pmic.registers[0x90] == 0);

    /* Faults: an ADC timeout does not stop detection; an ALDO1 read NACK
     * skips ALDO1; each error is reported as it came. */
    reset_pmic();
    pmic.registers[0x92] = 0x1C;
    pmic.read_error[0x30] = TIMEOUT;
    pmic.read_error[0x92] = NACK;
    board_pmic_start(&bus, &result);
    assert(result.adc_error == TIMEOUT && result.detection_error == 0);
    assert(result.aldo1_read_error == NACK && !result.aldo1_enable_tried);
    assert(pmic.registers[0x68] == 0x01 && pmic.registers[0x90] == 0 && pmic.writes == 1);
    /* A failed ALDO1 enable write is reported. */
    reset_pmic();
    pmic.registers[0x92] = 0x1C;
    pmic.write_error[0x90] = NACK;
    board_pmic_start(&bus, &result);
    assert(result.aldo1_enable_tried && result.aldo1_enable_error == NACK);
}

int main(void)
{
    check_touch_filter();
    check_pmic();
    check_pmic_writes();
    puts("board 1.75C logic: OK");
    return 0;
}
