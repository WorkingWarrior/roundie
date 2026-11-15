#include "roundie_rtc_time.h"

#include <stddef.h>

static bool leap_year(uint16_t year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool roundie_rtc_time_valid(const roundie_rtc_time_t *time)
{
    static const uint8_t month_days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    if (time == NULL || time->year < 2000 || time->year > 2099 ||
        time->month < 1 || time->month > 12 || time->weekday > 6 ||
        time->hour > 23 || time->minute > 59 || time->second > 59) {
        return false;
    }
    uint8_t days = month_days[time->month - 1];
    if (time->month == 2 && leap_year(time->year)) {
        days++;
    }
    return time->day >= 1 && time->day <= days;
}

bool roundie_rtc_unix_seconds(const roundie_rtc_time_t *time, int64_t *seconds)
{
    static const uint8_t month_days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    if (seconds == NULL || !roundie_rtc_time_valid(time)) {
        return false;
    }
    int64_t days = 0;
    for (uint16_t year = 1970; year < time->year; year++) {
        days += leap_year(year) ? 366 : 365;
    }
    for (uint8_t month = 1; month < time->month; month++) {
        days += month_days[month - 1];
        if (month == 2 && leap_year(time->year)) {
            days++;
        }
    }
    days += time->day - 1;
    if ((days + 4) % 7 != time->weekday) {
        return false;
    }
    *seconds = days * 86400 + time->hour * 3600 +
               time->minute * 60 + time->second;
    return true;
}

bool roundie_rtc_from_unix(int64_t seconds, roundie_rtc_time_t *time)
{
    if (time == NULL || seconds < INT64_C(946684800) ||
        seconds > INT64_C(4102444799)) {
        return false;
    }
    static const uint8_t month_days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    int64_t days = (seconds - INT64_C(946684800)) / 86400;
    time->weekday = (uint8_t)((seconds / 86400 + 4) % 7);
    time->year = 2000;
    while (days >= (leap_year(time->year) ? 366 : 365)) {
        days -= leap_year(time->year) ? 366 : 365;
        time->year++;
    }
    time->month = 1;
    for (;;) {
        unsigned length = month_days[time->month - 1];
        if (time->month == 2 && leap_year(time->year)) {
            length++;
        }
        if (days < length) {
            break;
        }
        days -= length;
        time->month++;
    }
    time->day = (uint8_t)(days + 1);
    time->hour = (uint8_t)(seconds % 86400 / 3600);
    time->minute = (uint8_t)(seconds % 3600 / 60);
    time->second = (uint8_t)(seconds % 60);
    return true;
}

bool roundie_rtc_readback_matches(int64_t expected, int64_t actual)
{
    return expected >= INT64_C(946684800) &&
           expected <= INT64_C(4102444799) &&
           actual >= expected && actual <= INT64_C(4102444799) &&
           actual - expected <= 1;
}

bool roundie_rtc_progress_matches(int64_t first, int64_t next, int64_t elapsed_us)
{
    return first >= INT64_C(946684800) && next <= INT64_C(4102444799) &&
           elapsed_us >= 1100000 && elapsed_us <= 3000000 &&
           next > first && next - first <= elapsed_us / 1000000 + 1;
}
