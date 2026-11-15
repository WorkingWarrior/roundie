#pragma once

/* Hardware-independent logic of the ESP32-S3-Touch-AMOLED-1.75C board, kept
 * apart from board.c so that host tests compile the production code
 * (tests/test_board_1_75c.c). */

#include <stdbool.h>
#include <stdint.h>

/* Outcome of one CST9217 read. */
typedef enum {
    BOARD_TOUCH_POINT,  /* acknowledged report with one touch point */
    BOARD_TOUCH_NONE,   /* acknowledged report without a touch point */
    BOARD_TOUCH_NO_ACK, /* report without the 0xAB acknowledge byte */
} board_touch_report_t;

/* Consecutive unacknowledged reads that keep a touch held, about 200 ms at
 * LVGL's 33 ms read period. Assumption until checked on the board
 * (docs/hardware-audit-1.75c.md, D5). */
#define BOARD_TOUCH_NO_ACK_LIMIT 6U

typedef struct {
    bool pressed;
    uint8_t unacknowledged;
    uint16_t x;
    uint16_t y;
} board_touch_filter_t;

/* Applies one read and returns whether a touch is reported; *x and *y are
 * then the panel point, clamped to width x height. An unacknowledged report
 * keeps a held touch at its last point for at most BOARD_TOUCH_NO_ACK_LIMIT
 * reads, as the Waveshare BSP keeps the driver's last data, so a held finger
 * does not chatter; it never starts a touch. */
bool board_touch_filter_update(board_touch_filter_t *filter, board_touch_report_t report,
                               uint16_t raw_x, uint16_t raw_y, uint16_t width,
                               uint16_t height, uint16_t *x, uint16_t *y);
/* Bus error or controller restart: no touch is held. */
void board_touch_filter_reset(board_touch_filter_t *filter);

/* AXP2101 status registers 0x00 and 0x01 and battery voltage registers
 * 0x34..0x35. Register meanings are a hypothesis from XPowersLib
 * (docs/hardware-audit-1.75c.md). */
bool board_pmic_battery_present(const uint8_t status[2]);
bool board_pmic_discharging(const uint8_t status[2]);
uint16_t board_pmic_battery_mv(const uint8_t voltage[2]);
bool board_pmic_voltage_plausible(uint16_t millivolts);
