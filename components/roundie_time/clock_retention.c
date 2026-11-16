#include "clock_retention.h"

#include <stddef.h>

#define RETENTION_MAGIC 0x524B4C43U /* "CLKR" */
#define RETENTION_VERSION 1U
/* Fixed allowance plus 2 % of the RTC time since the sync. The RTC slow clock
 * may be the calibrated internal RC oscillator; light sleep re-aligns the
 * boot time to it. Assumption until drift is measured on the board. */
#define RETENTION_FIXED_US INT64_C(2000000)
#define RETENTION_RATE_DIVISOR 50

static uint32_t mix(uint32_t hash, uint64_t value)
{
    for (int byte = 0; byte < 8; byte++) {
        hash ^= (uint32_t)(value & 0xFFU);
        hash *= 16777619U;
        value >>= 8;
    }
    return hash;
}

static uint32_t checksum(const roundie_clock_retention_t *record)
{
    uint32_t hash = 2166136261U;
    hash = mix(hash, record->magic);
    hash = mix(hash, record->version);
    hash = mix(hash, (uint64_t)record->wall_us);
    hash = mix(hash, (uint64_t)record->rtc_us);
    return hash;
}

void roundie_clock_retention_record(roundie_clock_retention_t *record,
                                    int64_t wall_us, int64_t rtc_us)
{
    record->magic = RETENTION_MAGIC;
    record->version = RETENTION_VERSION;
    record->wall_us = wall_us;
    record->rtc_us = rtc_us;
    record->checksum = checksum(record);
}

void roundie_clock_retention_clear(roundie_clock_retention_t *record)
{
    record->magic = 0;
    record->version = 0;
    record->wall_us = 0;
    record->rtc_us = 0;
    record->checksum = 0;
}

bool roundie_clock_retention_accept(const roundie_clock_retention_t *record,
                                    int64_t wall_now_us, int64_t rtc_now_us)
{
    if (record->magic != RETENTION_MAGIC || record->version != RETENTION_VERSION ||
        record->checksum != checksum(record)) {
        return false;
    }
    if (record->wall_us <= 0 || record->rtc_us < 0) {
        return false;
    }
    if (rtc_now_us < record->rtc_us || wall_now_us < record->wall_us) {
        return false;
    }
    int64_t rtc_elapsed = rtc_now_us - record->rtc_us;
    int64_t wall_elapsed = wall_now_us - record->wall_us;
    int64_t difference = wall_elapsed - rtc_elapsed;
    if (difference < 0) {
        difference = -difference;
    }
    return difference <= RETENTION_FIXED_US + rtc_elapsed / RETENTION_RATE_DIVISOR;
}
