#include "sensor_codec.h"

#include <stddef.h>

static bool bcd_decode(uint8_t raw, uint8_t *value)
{
    if ((raw & 0x0f) > 9 || (raw >> 4) > 9) {
        return false;
    }
    *value = (uint8_t)((raw >> 4) * 10 + (raw & 0x0f));
    return true;
}

static uint8_t bcd_encode(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

bool roundie_rtc_decode(const uint8_t raw[7], roundie_rtc_time_t *time,
                        bool *oscillator_stopped)
{
    if (raw == NULL || time == NULL || oscillator_stopped == NULL) {
        return false;
    }
    *oscillator_stopped = (raw[0] & 0x80) != 0;
    /* Reserved bits and the 12-hour mode bit are not valid calendar digits. */
    if ((raw[1] & 0x80) || (raw[2] & 0xc0) || (raw[3] & 0xc0) ||
        (raw[4] & 0xf8) || (raw[5] & 0xe0)) {
        return false;
    }
    uint8_t year;
    if (!bcd_decode(raw[0] & 0x7f, &time->second) ||
        !bcd_decode(raw[1], &time->minute) ||
        !bcd_decode(raw[2], &time->hour) ||
        !bcd_decode(raw[3], &time->day) ||
        !bcd_decode(raw[5], &time->month) ||
        !bcd_decode(raw[6], &year)) {
        return false;
    }
    time->weekday = raw[4];
    time->year = 2000 + year;
    int64_t seconds;
    return roundie_rtc_unix_seconds(time, &seconds);
}

bool roundie_rtc_encode(const roundie_rtc_time_t *time, uint8_t raw[7])
{
    int64_t seconds;
    if (raw == NULL || !roundie_rtc_unix_seconds(time, &seconds)) {
        return false;
    }
    raw[0] = bcd_encode(time->second);
    raw[1] = bcd_encode(time->minute);
    raw[2] = bcd_encode(time->hour);
    raw[3] = bcd_encode(time->day);
    raw[4] = time->weekday;
    raw[5] = bcd_encode(time->month);
    raw[6] = bcd_encode((uint8_t)(time->year - 2000));
    return true;
}

uint16_t roundie_gauge_word(const uint8_t raw[2])
{
    return (uint16_t)raw[0] | ((uint16_t)raw[1] << 8);
}

int16_t roundie_gauge_current(uint16_t raw)
{
    return raw <= INT16_MAX ? (int16_t)raw : (int16_t)((int32_t)raw - 65536);
}

int32_t roundie_gauge_deci_c(uint16_t raw)
{
    return (int32_t)raw - 2732; /* 0.1 K to 0.1 C, rounded at 0 C */
}
