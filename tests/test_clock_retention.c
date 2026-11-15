#include "clock_retention.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define WALL INT64_C(1792390000000000)
#define RTC INT64_C(600000000)

int main(void)
{
    roundie_clock_retention_t record;

    /* Uninitialized RTC memory after power-on: zeros or arbitrary bytes. */
    memset(&record, 0, sizeof(record));
    assert(!roundie_clock_retention_accept(&record, WALL, RTC));
    memset(&record, 0xA5, sizeof(record));
    assert(!roundie_clock_retention_accept(&record, WALL, RTC));

    /* Software reset 10 s after the sync: both clocks advanced alike. */
    roundie_clock_retention_record(&record, WALL, RTC);
    assert(roundie_clock_retention_accept(&record, WALL + 10000000, RTC + 10000000));
    assert(roundie_clock_retention_accept(&record, WALL, RTC));
    /* Fixed 2 s allowance, then 2 % of the elapsed RTC time. */
    assert(roundie_clock_retention_accept(&record, WALL + 12000000, RTC + 10000000));
    assert(!roundie_clock_retention_accept(&record, WALL + 12200001, RTC + 10000000));
    assert(roundie_clock_retention_accept(&record, WALL + 1020000000, RTC + 1000000000));
    assert(!roundie_clock_retention_accept(&record, WALL + 1022000001, RTC + 1000000000));

    /* RTC timer restarted (power loss) or system time lost (boot time zero). */
    assert(!roundie_clock_retention_accept(&record, WALL + 1000000, RTC - 1));
    assert(!roundie_clock_retention_accept(&record, RTC + 10000000, RTC + 10000000));
    assert(!roundie_clock_retention_accept(&record, WALL - 1, RTC + 10000000));

    /* Any changed field invalidates the record. */
    roundie_clock_retention_t damaged = record;
    damaged.wall_us += 1000000;
    assert(!roundie_clock_retention_accept(&damaged, WALL + 1000000, RTC));
    damaged = record;
    damaged.rtc_us -= 1;
    assert(!roundie_clock_retention_accept(&damaged, WALL, RTC));
    damaged = record;
    damaged.version++;
    assert(!roundie_clock_retention_accept(&damaged, WALL, RTC));

    /* A cleared record (failed SET, rejected boot) is never accepted. */
    roundie_clock_retention_clear(&record);
    assert(!roundie_clock_retention_accept(&record, WALL, RTC));

    /* A record with no plausible wall time is rejected even if intact. */
    roundie_clock_retention_record(&record, 0, RTC);
    assert(!roundie_clock_retention_accept(&record, 1000000, RTC + 1000000));

    puts("clock retention: OK");
    return 0;
}
