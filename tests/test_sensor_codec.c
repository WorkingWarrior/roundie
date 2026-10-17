#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sensor_codec.h"

static void test_rtc(void)
{
    roundie_rtc_time_t leap_day = {
        .year = 2024, .month = 2, .day = 29, .weekday = 4,
        .hour = 23, .minute = 59, .second = 58,
    };
    uint8_t raw[7];
    assert(roundie_rtc_encode(&leap_day, raw));
    assert(memcmp(raw, (uint8_t[]){0x58, 0x59, 0x23, 0x29, 0x04, 0x02, 0x24}, 7) == 0);
    roundie_rtc_time_t decoded;
    bool stopped;
    assert(roundie_rtc_decode(raw, &decoded, &stopped));
    assert(!stopped && decoded.year == 2024 && decoded.day == 29);
    int64_t epoch;
    assert(roundie_rtc_unix_seconds(&decoded, &epoch));
    assert(epoch == INT64_C(1709251198));

    raw[0] |= 0x80;
    assert(roundie_rtc_decode(raw, &decoded, &stopped) && stopped);
    raw[0] = 0x6a;
    assert(!roundie_rtc_decode(raw, &decoded, &stopped));
    raw[0] = 0x58;
    raw[3] = 0x30;
    assert(!roundie_rtc_decode(raw, &decoded, &stopped));
    leap_day.year = 2025;
    assert(!roundie_rtc_encode(&leap_day, raw));
    leap_day.year = 2024;
    leap_day.weekday = 5;
    assert(!roundie_rtc_encode(&leap_day, raw));
}

static void test_gauge(void)
{
    assert(roundie_gauge_word((uint8_t[]){0x82, 0x0f}) == 3970);
    assert(roundie_gauge_current(0xff85) == -123);
    assert(roundie_gauge_current(123) == 123);
    assert(roundie_gauge_deci_c(2982) == 250);
}

int main(void)
{
    test_rtc();
    test_gauge();
    puts("sensor codec tests passed");
    return 0;
}
