#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "roundie_board.h"
#include "roundie_platform.h"
#include "roundie_power.h"
#include "roundie_power_inhibit.h"
#include "roundie_power_lifecycle.h"

/* State shared by the power manager's GUI half (power_gui.c, also built by
 * the desktop) and its system worker half (power_manager.c on the board, the
 * desktop's own worker). Private to these files. */

typedef enum {
    SLEEP_IDLE,
    SLEEP_REQUESTED, /* GUI or RW1 asked; worker has not checked admission. */
    SLEEP_PARK,      /* Admitted; GUI must turn the panel off and park. */
    SLEEP_PARKED,    /* GUI parked; worker may sleep. */
    SLEEP_RESUMED,   /* Worker finished or aborted; GUI resumes. */
} sleep_phase_t;

typedef enum {
    COMMAND_NONE,
    COMMAND_RESTART,
    COMMAND_SAFE_MODE,
    COMMAND_STORAGE, /* restart into USB storage mode */
} command_t;

typedef enum {
    SCREEN_REQUEST_NONE,
    SCREEN_REQUEST_OFF,
    SCREEN_REQUEST_ON,
} screen_request_t;

/* The only state shared by the GUI task and the worker; copied under lock. */
typedef struct {
    roundie_lock_t lock;
    roundie_board_t *board;
    sleep_phase_t phase;
    uint32_t sleep_limit_s;
    int64_t parked_at_us;
    bool user_wake;
    command_t command;
    screen_request_t screen_request;
    uint32_t message_serial;
    char message[40];
    roundie_power_state_t state;
    roundie_power_refusal_t refusal;
    esp_err_t sleep_error;
    uint32_t sleeps;
    uint32_t screen_offs;
    uint32_t wake_causes;
    int64_t last_sleep_us;
    roundie_pm_t pm;                     /* M8-B state model and its diagnostics */
    roundie_power_inhibitors_t inhibitors;
} roundie_power_shared_t;

extern roundie_power_shared_t roundie_power_shared;
