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

    /* 0x9f + 0x92 + 0x04 + 0xb0 = 485; 485 mod 256 = 229; 255 - 229 = 26. */
    uint8_t block[32] = {0x04, 0xb0};
    assert(roundie_gauge_block_checksum(0x929f, block, sizeof(block)) == 26);
    /* Replacement method (TRM 6.1): new = 255 - ((255 - old - removed + added) mod 256). */
    uint8_t old_sum = roundie_gauge_block_checksum(0x929d, block, sizeof(block));
    block[0] = 0x02;
    block[1] = 0x58;
    uint8_t expected = (uint8_t)(255 - ((255 - old_sum - 0x04 - 0xb0 + 0x02 + 0x58) & 0xff));
    assert(roundie_gauge_block_checksum(0x929d, block, sizeof(block)) == expected);
    assert(roundie_gauge_block_checksum(0x0000, block, 0) == 255);
}

static void test_utc_conversion(void)
{
    roundie_rtc_time_t calendar;
    assert(!roundie_rtc_from_unix(INT64_MIN, &calendar));
    assert(!roundie_rtc_from_unix(946684799, &calendar));
    assert(!roundie_rtc_from_unix(4102444800, &calendar));
    assert(!roundie_rtc_from_unix(INT64_MAX, &calendar));
    assert(!roundie_rtc_from_unix(946684800, NULL));
    assert(roundie_rtc_from_unix(946684800, &calendar));
    assert(calendar.year == 2000 && calendar.month == 1 && calendar.day == 1);
    assert(calendar.weekday == 6 && calendar.hour == 0 && calendar.second == 0);
    assert(roundie_rtc_from_unix(4102444799, &calendar));
    assert(calendar.year == 2099 && calendar.month == 12 && calendar.day == 31);
    assert(calendar.hour == 23 && calendar.minute == 59 && calendar.second == 59);
    assert(roundie_rtc_from_unix(951782400, &calendar));
    assert(calendar.year == 2000 && calendar.month == 2 && calendar.day == 29);
    assert(roundie_rtc_from_unix(951868800, &calendar));
    assert(calendar.month == 3 && calendar.day == 1);
    assert(roundie_rtc_from_unix(1740787200, &calendar));
    assert(calendar.year == 2025 && calendar.month == 3 && calendar.day == 1);
    /* Every supported date, with both sides of every midnight/year boundary. */
    for (int64_t day = 946684800; day <= 4102358400; day += 86400) {
        const int64_t offsets[] = {0, 1, 86398, 86399};
        for (unsigned i = 0; i < 4; i++) {
            int64_t expected = day + offsets[i];
            assert(roundie_rtc_from_unix(expected, &calendar));
            int64_t actual;
            assert(roundie_rtc_unix_seconds(&calendar, &actual));
            assert(actual == expected);
            uint8_t raw[7];
            bool os;
            roundie_rtc_time_t decoded;
            assert(roundie_rtc_encode(&calendar, raw));
            assert(roundie_rtc_decode(raw, &decoded, &os) && !os);
            assert(roundie_rtc_unix_seconds(&decoded, &actual) && actual == expected);
        }
    }
    assert(roundie_rtc_readback_matches(1709251199, 1709251200));
    assert(!roundie_rtc_readback_matches(1709251199, 1709251198));
    assert(!roundie_rtc_readback_matches(1709251199, 1709251201));
    assert(!roundie_rtc_readback_matches(4102444799, 4102444800));
    assert(roundie_rtc_progress_matches(1709251199, 1709251200, 1200000));
    assert(roundie_rtc_progress_matches(1709251199, 1709251201, 1200000));
    assert(!roundie_rtc_progress_matches(1709251199, 1709251199, 1200000));
    assert(!roundie_rtc_progress_matches(1709251199, 1709251198, 1200000));
    assert(!roundie_rtc_progress_matches(1709251199, 1709251202, 1200000));
    assert(!roundie_rtc_progress_matches(1709251199, 1709251200, 500000));
    assert(!roundie_rtc_progress_matches(1709251199, 1709251200, 3100000));
}

int main(void)
{
    test_rtc();
    test_utc_conversion();
    test_gauge();
    puts("sensor codec tests passed");
    return 0;
}
