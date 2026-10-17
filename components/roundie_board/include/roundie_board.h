#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

typedef struct roundie_board roundie_board_t;

typedef struct {
    esp_err_t touch;
    esp_err_t rtc;
    esp_err_t gauge;
    esp_err_t rtc_read;
    esp_err_t gauge_read;
    uint8_t rtc_seconds;
    uint8_t rtc_minutes;
    uint8_t rtc_hours;
    bool rtc_voltage_low;
    uint16_t gauge_millivolts;
    uint16_t gauge_status;
    uint8_t lcd_variant;
} roundie_board_status_t;

/* The board owns all handles. A failed LCD or bus start returns an error and
 * releases resources. Optional I2C device failures remain in status. */
esp_err_t roundie_board_start(roundie_board_t **out_board,
                              roundie_board_status_t *status);
void roundie_board_stop(roundie_board_t *board);

/* Borrowed handles remain valid until roundie_board_stop(). */
esp_lcd_panel_handle_t roundie_board_panel(roundie_board_t *board);
esp_lcd_panel_io_handle_t roundie_board_panel_io(roundie_board_t *board);

/* Only call from the GUI task; CST816S access stays on the shared master. */
esp_err_t roundie_board_touch_read(roundie_board_t *board, bool *pressed,
                                   uint16_t *x, uint16_t *y);
esp_err_t roundie_board_backlight_set(roundie_board_t *board, uint8_t percent);
