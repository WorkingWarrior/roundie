#pragma once

#include <stdbool.h>
#include <stdint.h>

/* UTC calendar, restricted to 2000..2099 by the PCF85063 year register. */
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday; /* 0 = Sunday */
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} roundie_rtc_time_t;

/* Rejects invalid calendar values and weekday/date disagreement. */
bool roundie_rtc_unix_seconds(const roundie_rtc_time_t *time, int64_t *seconds);
