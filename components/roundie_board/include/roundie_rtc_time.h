#pragma once

#include <stdbool.h>
#include <stdint.h>

/* UTC calendar, restricted to 2000..2099: the range of a two-digit RTC year
 * register (PCF85063 on the 1.85B); the firmware-wide calendar range. */
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday; /* 0 = Sunday */
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} roundie_rtc_time_t;

/* Field ranges only; weekday/date agreement is checked by roundie_rtc_unix_seconds(). */
bool roundie_rtc_time_valid(const roundie_rtc_time_t *time);
/* Rejects invalid calendar values and weekday/date disagreement. */
bool roundie_rtc_unix_seconds(const roundie_rtc_time_t *time, int64_t *seconds);
bool roundie_rtc_from_unix(int64_t seconds, roundie_rtc_time_t *time);

/* Seconds-resolution comparisons; never accept backward or frozen time. */
bool roundie_rtc_readback_matches(int64_t expected, int64_t actual);
bool roundie_rtc_progress_matches(int64_t first, int64_t next, int64_t elapsed_us);
