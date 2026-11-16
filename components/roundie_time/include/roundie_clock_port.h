#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "clock_retention.h"
#include "esp_err.h"
#include "time_protocol.h"

/* Timekeeping hardware and storage under the shared time code
 * (docs/roundie-core.md, M3). Each platform implements these functions:
 * ESP-IDF in components/roundie_time/idf/clock_port.c, the Linux desktop in
 * platform/linux/clock.c. Monotonic time is roundie_time_us() (D1); nothing
 * here is used for timeouts or intervals. */

/* System wall clock, UTC microseconds since 1970 (gettimeofday). */
int64_t roundie_clock_port_wall_us(void);
/* Sets the system wall clock to whole UTC seconds (settimeofday). */
bool roundie_clock_port_wall_set(int64_t seconds);

/* Boards without an external RTC (clock_retention.h). The retention timer
 * keeps counting through a software reset and restarts on power loss
 * (ESP32-S3 RTC timer). */
int64_t roundie_clock_port_retention_timer_us(void);
/* Stores the running wall clock where a software reset finds it again
 * (ESP-IDF: esp_sync_timekeeping_timers). */
void roundie_clock_port_retention_align(void);
/* The record in memory that a software reset keeps (RTC_NOINIT). */
roundie_clock_retention_t *roundie_clock_port_retained(void);
/* Whether the last reset kept the RTC domain; reason is the platform's code
 * for logs. */
bool roundie_clock_port_reset_kept_domain(int *reason);

/* Local time zone storage and lock (local_time.c). */
void roundie_clock_port_zone_lock_init(void);
void roundie_clock_port_zone_lock(void);
void roundie_clock_port_zone_unlock(void);
/* False when no valid rule is stored. */
bool roundie_clock_port_zone_load(char rule[ROUNDIE_TIME_ZONE_CAPACITY]);
esp_err_t roundie_clock_port_zone_store(const char *rule);
