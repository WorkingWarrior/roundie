#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "roundie_rtc_time.h"

typedef struct roundie_board roundie_board_t;

typedef enum {
    ROUNDIE_SENSOR_VALID,
    ROUNDIE_SENSOR_NOT_FOUND,
    ROUNDIE_SENSOR_COMM_ERROR,
    ROUNDIE_SENSOR_INVALID,
    ROUNDIE_SENSOR_UNAVAILABLE,
    ROUNDIE_SENSOR_BATTERY_ABSENT,
    ROUNDIE_SENSOR_UNCALIBRATED,
} roundie_sensor_state_t;

typedef struct {
    roundie_sensor_state_t state;
    esp_err_t error;
    int64_t observed_us; /* esp_timer monotonic timestamp */
    bool oscillator_stopped;
    roundie_rtc_time_t time;
} roundie_rtc_sample_t;

typedef struct {
    roundie_sensor_state_t state;
    esp_err_t error;
    int64_t observed_us;
    bool battery_present;
    bool initialized;
    bool voltage_valid;
    bool soc_valid;
    bool current_valid;
    bool temperature_valid;
    bool remaining_valid;
    bool full_valid;
    uint16_t voltage_mv;
    uint16_t soc_percent;
    int16_t current_ma; /* positive = charging, negative = discharging */
    int32_t temperature_deci_c;
    uint16_t remaining_mah;
    uint16_t full_mah;
    uint16_t battery_flags;
    uint16_t operation_flags;
    bool discharging;
    bool sleeping;
} roundie_battery_sample_t;

typedef struct {
    esp_err_t touch;
    esp_err_t rtc;
    esp_err_t gauge;
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

/* Sensor calls use the board's shared master and bounded 100 ms transactions.
 * The sensor worker serializes RTC reads and sets after startup. Set verifies
 * immediate readback only; its caller must confirm progression before wall sync.
 * A failed probe is retried on later samples; no handle is permanently absent. */
void roundie_board_rtc_read(roundie_board_t *board, roundie_rtc_sample_t *sample);
esp_err_t roundie_board_rtc_set_trusted(roundie_board_t *board,
                                        const roundie_rtc_time_t *time);
void roundie_board_battery_read(roundie_board_t *board,
                                roundie_battery_sample_t *sample);
