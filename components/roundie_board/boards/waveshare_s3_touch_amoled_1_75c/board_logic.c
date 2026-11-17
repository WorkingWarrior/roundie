#include "board_logic.h"

#include <stddef.h>

static uint16_t clamp_coordinate(uint16_t value, uint16_t size)
{
    if (value >= size) {
        return (uint16_t)(size - 1U);
    }
    return value;
}

void board_touch_filter_reset(board_touch_filter_t *filter)
{
    filter->pressed = false;
    filter->unacknowledged = 0;
}

bool board_touch_filter_update(board_touch_filter_t *filter, board_touch_report_t report,
                               uint16_t raw_x, uint16_t raw_y, uint16_t width,
                               uint16_t height, uint16_t *x, uint16_t *y)
{
    switch (report) {
    case BOARD_TOUCH_POINT:
        filter->pressed = true;
        filter->unacknowledged = 0;
        /* The driver mirrors as x_max - x, so a raw 0 becomes width. */
        filter->x = clamp_coordinate(raw_x, width);
        filter->y = clamp_coordinate(raw_y, height);
        break;
    case BOARD_TOUCH_NO_ACK:
        if (filter->pressed && filter->unacknowledged < BOARD_TOUCH_NO_ACK_LIMIT) {
            filter->unacknowledged++;
        } else {
            board_touch_filter_reset(filter);
        }
        break;
    case BOARD_TOUCH_NONE:
    default:
        board_touch_filter_reset(filter);
        break;
    }
    if (filter->pressed) {
        *x = filter->x;
        *y = filter->y;
    }
    return filter->pressed;
}

bool board_pmic_battery_present(const uint8_t status[2])
{
    return (status[0] & 0x08U) != 0;
}

bool board_pmic_charging(const uint8_t status[2])
{
    return ((status[1] >> 5) & 0x03U) == 0x01U;
}

bool board_pmic_discharging(const uint8_t status[2])
{
    return ((status[1] >> 5) & 0x03U) == 0x02U;
}

uint16_t board_pmic_battery_mv(const uint8_t voltage[2])
{
    /* 13 bits, as XPowersLib's readRegisterH5L8(0x34, 0x35). */
    return (uint16_t)(((voltage[0] & 0x1FU) << 8) | voltage[1]);
}

bool board_pmic_voltage_plausible(uint16_t millivolts)
{
    /* 0 while the PMIC's battery voltage ADC is disabled. */
    return millivolts > 0 && millivolts <= 6000;
}

bool board_pmic_percent_valid(uint8_t percent)
{
    return percent <= 100;
}

uint16_t board_pmic_aldo1_mv(uint8_t voltage_register)
{
    /* 0x92 bits 4..0: 0.5 V + 100 mV per step; codes above 28 (3.3 V) are reserved. */
    uint8_t code = voltage_register & 0x1FU;
    if (code > 28U) {
        return 0;
    }
    return (uint16_t)(500U + 100U * code);
}

int board_pmic_set_bits(const board_pmic_bus_t *bus, uint8_t reg, uint8_t mask)
{
    uint8_t value = 0;
    int error = bus->read(bus->context, reg, &value);
    if (error != 0) {
        return error;
    }
    if ((value & mask) == mask) {
        return 0;
    }
    return bus->write(bus->context, reg, (uint8_t)(value | mask));
}

void board_pmic_start(const board_pmic_bus_t *bus, board_pmic_start_t *result)
{
    *result = (board_pmic_start_t){0};
    result->adc_error = board_pmic_set_bits(bus, BOARD_PMIC_REG_ADC_CHANNEL_CTRL,
                                            BOARD_PMIC_ADC_VBAT_BIT);
    result->detection_error = board_pmic_set_bits(bus, BOARD_PMIC_REG_BAT_DET_CTRL,
                                                  BOARD_PMIC_BAT_DET_BIT);
    result->aldo1_read_error = bus->read(bus->context, BOARD_PMIC_REG_ALDO1_VOLTAGE,
                                         &result->aldo1_register);
    if (result->aldo1_read_error != 0) {
        return;
    }
    result->aldo1_mv = board_pmic_aldo1_mv(result->aldo1_register);
    if (result->aldo1_mv != BOARD_PMIC_ALDO1_EXPECTED_MV) {
        return;
    }
    result->aldo1_enable_tried = true;
    result->aldo1_enable_error = board_pmic_set_bits(bus, BOARD_PMIC_REG_LDO_ONOFF_CTRL0,
                                                     BOARD_PMIC_LDO_ALDO1_BIT);
}
