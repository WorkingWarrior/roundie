#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Internal diagnostic model, not application ABI. All times are microseconds
 * except RTC's floored UTC seconds. No allocation or clock writes. */
typedef struct {
    int64_t wall_before_us;
    int64_t wall_after_us;
    int64_t mono_before_us;
    int64_t mono_after_us;
    int64_t rtc_seconds;
    bool rtc_valid;
    bool oscillator_stopped;
    bool wall_valid;
    bool read_ok;
} roundie_clock_sample_t;

typedef struct {
    bool usable;
    int64_t lower_us; /* RTC minus system; conservative closed enclosure */
    int64_t upper_us;
} roundie_clock_offset_t;

roundie_clock_offset_t roundie_clock_offset(const roundie_clock_sample_t *sample);
