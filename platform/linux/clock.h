#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "roundie_elf.h"

/* Emulated timekeeping hardware of the Linux desktop (docs/roundie-core.md,
 * M3): the system wall clock, the ESP32-S3 RTC timer with its boot-time
 * register and RTC_NOINIT record, and on the 1.85b the battery-backed
 * PCF85063. The shared time code (components/roundie_time) runs on it through
 * roundie_clock_port.h and roundie_board_rtc_*(). The model follows the
 * firmware's reading of ESP-IDF; it is not measured board behaviour.
 *
 * Every function may be called from any thread; the state is under one lock. */

typedef enum {
    ROUNDIE_DESKTOP_RESET_POWER, /* power-on, also the process start */
    ROUNDIE_DESKTOP_RESET_SW,
    ROUNDIE_DESKTOP_RESET_PANIC,
    ROUNDIE_DESKTOP_RESET_TASK_WDT,
    ROUNDIE_DESKTOP_RESET_BROWNOUT,
    ROUNDIE_DESKTOP_RESET_PIN,
} roundie_desktop_reset_t;

typedef enum {
    ROUNDIE_DESKTOP_RTC_RUNNING,
    ROUNDIE_DESKTOP_RTC_STOPPED, /* oscillator stopped: time frozen, OS flag set */
    ROUNDIE_DESKTOP_RTC_BUS,     /* I2C transactions time out */
} roundie_desktop_rtc_fault_t;

/* Main thread, before any other thread. zone_path holds the stored time zone
 * rule (NVS "clock"/"tz" on the board). The PCF85063 starts as after a battery
 * change: 2000-01-01 00:00:00 with the oscillator-stop flag set. */
void roundie_desktop_clock_init(bool board_has_rtc, const char *zone_path);
bool roundie_desktop_clock_has_rtc(void);

/* Time passes faster: monotonic time, the system wall clock and both
 * oscillators move forward by us >= 0 at once. */
void roundie_desktop_clock_advance(int64_t us);
/* Only the system wall clock jumps, as if something outside Roundie changed
 * it. Monotonic time and validity are unaffected. */
void roundie_desktop_clock_jump_wall(int64_t us);
/* Rate error of the ESP32-S3 RTC timer (chip = false) or of the PCF85063
 * (chip = true), parts per million. */
void roundie_desktop_clock_drift(bool chip, int32_t ppm);
void roundie_desktop_clock_rtc_fault(roundie_desktop_rtc_fault_t fault);

/* Hardware effect of a reset at this moment: what the RTC domain keeps, and
 * the system time that ESP-IDF restores at startup from the boot-time register
 * and the RTC timer. The caller then reruns the firmware's startup path. */
void roundie_desktop_clock_reset(roundie_desktop_reset_t reset);
const char *roundie_desktop_reset_name(roundie_desktop_reset_t reset);
bool roundie_desktop_reset_parse(const char *name, roundie_desktop_reset_t *reset);

/* Light sleep: ESP-IDF advances the system clock by the RTC timer's count of
 * the sleep, so the RTC rate error enters the wall clock. */
void roundie_desktop_clock_sleep_enter(void);
void roundie_desktop_clock_sleep_exit(void);

/* platform.c: forward offset of roundie_time_us(). */
void roundie_desktop_time_advance(int64_t us);

/* app_runtime.c: the clock source registered for applications. */
void roundie_desktop_set_app_clock_source(roundie_elf_clock_source_t source);
roundie_elf_clock_source_t roundie_desktop_app_clock_source(void);
