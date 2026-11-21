#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "roundie_board.h"
#include "sdkconfig.h"

/* REP §17 PowerManager glue. The GUI task owns the policy, panel and menu;
 * the system worker owns restart, Safe Mode requests and light sleep. Safe
 * Mode never binds it: its display stays on and has no power menu. */

/* GUI task, normal mode only. brightness_percent stays owned by the caller. */
void roundie_power_gui_bind(roundie_board_t *board, const uint8_t *brightness_percent,
    int64_t now_us);
/* Touch read callback: whether LVGL may receive this sample. */
bool roundie_power_gui_touch(int64_t now_us, bool pressed);
/* Once per GUI iteration, before LVGL work. dma_idle: every submitted LCD
 * transfer has completed. Returns false while parked for light sleep; the
 * caller then skips LVGL and only feeds its watchdog. */
bool roundie_power_gui_poll(int64_t now_us, bool dma_idle, bool keep_display);

/* GUI task: quick settings actions. sleep returns whether the request was
 * accepted; admission (and unloading the home application) follows in the
 * worker. */
void roundie_power_gui_screen_off(void);
bool roundie_power_gui_sleep(void);

/* Existing system worker loop. */
void roundie_power_worker_poll(int64_t now_us);
#if CONFIG_ROUNDIE_POWER
/* System worker: a diagnostics-owned inhibitor its holder renews. Releases
 * *handle (0 = none), then for seconds 1..600 acquires kinds, in one step
 * under the power lock; seconds 0 only releases. Returns whether held. */
bool roundie_power_diagnostics_hold(uint32_t *handle, uint32_t kinds, uint32_t seconds);
/* System worker: request a light sleep of limit_s (1..3600) seconds through
 * the normal admission path, only while the screen is already off, no sleep
 * or command is pending. Returns whether the request was accepted (the
 * worker may still refuse it at admission). */
bool roundie_power_request_timed_sleep(uint32_t limit_s);
/* System worker: show a battery alert (roundie_gauge_alert_t; 0 removes it).
 * A non-zero alert also turns the screen on. */
void roundie_power_battery_alert(uint8_t alert);
/* Completed light sleeps since boot. */
uint32_t roundie_power_light_sleeps(void);
/* Completed light sleeps since boot: their total time, wakes by the timer
 * alone or by anything else, the awake gaps between consecutive sleeps
 * (each from a wake to the next sleep start) and the last cause mask. */
typedef struct {
    uint32_t sleeps;
    int64_t slept_us;
    uint32_t timer_wakes;
    uint32_t other_wakes;
    int64_t awake_us;
    uint32_t awake_gaps;
    uint32_t causes;
} roundie_power_sleep_counters_t;
void roundie_power_sleep_counters(roundie_power_sleep_counters_t *counters);
#endif
#if CONFIG_ROUNDIE_DEV_POWER
/* Same bounded framer as RS1; replies before any sleep starts. */
void roundie_power_command(char *line, bool discard);
#endif
