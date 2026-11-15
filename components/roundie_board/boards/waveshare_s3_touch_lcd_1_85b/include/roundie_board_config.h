#pragma once

#include "sdkconfig.h"

/* Compile-time description of the Waveshare ESP32-S3-Touch-LCD-1.85B, read by
 * firmware outside the board component through roundie_board.h. Every board
 * defines the same macros; docs/board-support.md lists them. */

#define ROUNDIE_BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-1.85B"

/* ROUNDIE_BOARD_CAP_* bits from roundie_board.h. */
#define ROUNDIE_BOARD_CAPABILITIES \
    (ROUNDIE_BOARD_CAP_TOUCH | ROUNDIE_BOARD_CAP_SPEAKER | ROUNDIE_BOARD_CAP_ROUND_DISPLAY | \
     ROUNDIE_BOARD_CAP_RTC)

/* ST77916, 360 x 360 RGB565, round glass. */
#define ROUNDIE_BOARD_DISPLAY_WIDTH 360
#define ROUNDIE_BOARD_DISPLAY_HEIGHT 360
/* LVGL partial strip height; the QSPI transfer limit is sized from it. */
#define ROUNDIE_BOARD_DISPLAY_STRIP_LINES CONFIG_ROUNDIE_LCD_STRIP_LINES
/* The panel takes RGB565 most significant byte first; LVGL stores CPU order. */
#define ROUNDIE_BOARD_DISPLAY_SWAP_RGB565 1

/* ESP32-S3R8 with a 16 MiB Flash chip. */
#define ROUNDIE_BOARD_FLASH_BYTES 0x1000000U
#define ROUNDIE_BOARD_PSRAM_BYTES 0x800000U
