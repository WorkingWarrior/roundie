#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Wall-clock validity across a software reset on a board without an external
 * RTC. ESP-IDF itself keeps system time in the RTC domain (boot time in
 * RTC_BOOT_TIME registers plus the RTC timer); this record only decides
 * whether that time is still the one set by a trusted sync. It never supplies
 * a time. Internal firmware state, not application ABI. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    int64_t wall_us; /* system UTC at the trusted sync, microseconds */
    int64_t rtc_us;  /* RTC timer at the same moment, microseconds */
    uint32_t checksum;
} roundie_clock_retention_t;

void roundie_clock_retention_record(roundie_clock_retention_t *record,
                                    int64_t wall_us, int64_t rtc_us);
void roundie_clock_retention_clear(roundie_clock_retention_t *record);

/* True only for an intact record whose RTC timer has not restarted and whose
 * wall-clock progress since the sync matches the RTC timer's progress. The
 * caller decides separately whether the reset reason kept the RTC domain. */
bool roundie_clock_retention_accept(const roundie_clock_retention_t *record,
                                    int64_t wall_now_us, int64_t rtc_now_us);
