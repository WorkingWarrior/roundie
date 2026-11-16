/* System worker half of the power manager on the Linux desktop
 * (docs/roundie-core.md, M2). The GUI half is the firmware's own
 * main/power_gui.c. Light sleep is emulated: the GUI parks as on the board,
 * then the worker "sleeps" until the BOOT key or the touch panel wakes it, or
 * the sleep safety timer ends it, which keeps the screen off as a pure timer
 * wake does on the board. Restarts and USB storage mode do not exist on the
 * desktop and are refused with a message in the power menu. */

#include <stdio.h>
#include "clock.h"
#include "power_manager.h"
#include "power_shared.h"
#include "recovery_diagnostics.h"

/* As on the board: touch INT must be high within a second after the GUI parked. */
#define WAKE_SOURCE_WAIT_US 1000000

/* Worker thread only. */
static bool sleeping;
static int64_t sleep_started_us;

/* The desktop has no Safe Mode. */
bool roundie_recovery_active(void)
{
    return false;
}

static void report(const char *message)
{
    roundie_lock(&roundie_power_shared.lock);
    snprintf(roundie_power_shared.message, sizeof(roundie_power_shared.message), "%s", message);
    roundie_power_shared.message_serial++;
    roundie_unlock(&roundie_power_shared.lock);
}

static void finish_sleep(bool user_wake, roundie_power_refusal_t refusal, esp_err_t error, int64_t slept_us)
{
    roundie_lock(&roundie_power_shared.lock);
    roundie_power_shared.phase = SLEEP_RESUMED;
    roundie_power_shared.user_wake = user_wake;
    roundie_power_shared.refusal = refusal;
    roundie_power_shared.sleep_error = error;
    if (error == ESP_OK) {
        roundie_power_shared.sleeps++;
        roundie_power_shared.last_sleep_us = slept_us;
    }
    roundie_unlock(&roundie_power_shared.lock);
}

void roundie_power_worker_poll(int64_t now_us)
{
    roundie_lock(&roundie_power_shared.lock);
    roundie_board_t *board = roundie_power_shared.board;
    sleep_phase_t phase = roundie_power_shared.phase;
    uint32_t limit_s = roundie_power_shared.sleep_limit_s;
    int64_t parked_at_us = roundie_power_shared.parked_at_us;
    command_t command = roundie_power_shared.command;
    roundie_power_shared.command = COMMAND_NONE;
    roundie_unlock(&roundie_power_shared.lock);
    if (board == NULL) {
        return;
    }
    if (command != COMMAND_NONE) {
        report("Niedostępne na komputerze");
        fprintf(stderr, "roundie-desktop: power menu %s refused: not available on the desktop\n",
                command == COMMAND_STORAGE ? "USB storage" : command == COMMAND_SAFE_MODE ? "Safe Mode" : "restart");
    }
    if (phase == SLEEP_REQUESTED) {
        /* The desktop runtime never runs an application (D6), so no
         * condition of the board's admission check can refuse. */
        const roundie_power_conditions_t conditions = {
            .stable = true,
            .ota_idle = true,
        };
        roundie_power_refusal_t refusal = roundie_power_sleep_refusal(&conditions);
        roundie_lock(&roundie_power_shared.lock);
        roundie_power_shared.refusal = refusal;
        roundie_power_shared.phase = refusal == ROUNDIE_POWER_ALLOWED ? SLEEP_PARK : SLEEP_IDLE;
        roundie_unlock(&roundie_power_shared.lock);
        return;
    }
    if (phase != SLEEP_PARKED) {
        return;
    }
    bool wake_source = roundie_board_wake_source_active(board);
    if (!sleeping) {
        if (wake_source && now_us - parked_at_us < WAKE_SOURCE_WAIT_US) {
            return;
        }
        if (wake_source) {
            fprintf(stderr, "roundie-desktop: sleep aborted: %s\n",
                    roundie_power_refusal_name(ROUNDIE_POWER_REFUSED_WAKE_SOURCE));
            finish_sleep(true, ROUNDIE_POWER_REFUSED_WAKE_SOURCE, ESP_ERR_INVALID_STATE, 0);
            return;
        }
        sleeping = true;
        sleep_started_us = now_us;
        roundie_desktop_clock_sleep_enter();
        fprintf(stderr, "roundie-desktop: light sleep (emulated), safety timer %u s\n", (unsigned)limit_s);
        return;
    }
    bool timer = limit_s > 0 && now_us - sleep_started_us >= (int64_t)limit_s * 1000000;
    if (!wake_source && !timer) {
        return;
    }
    sleeping = false;
    int64_t slept_us = now_us - sleep_started_us;
    roundie_desktop_clock_sleep_exit();
    fprintf(stderr, "roundie-desktop: woke after %lld ms by %s\n", (long long)(slept_us / 1000),
            wake_source ? "BOOT or touch" : "the safety timer");
    finish_sleep(wake_source, ROUNDIE_POWER_ALLOWED, ESP_OK, slept_us);
}
