#include "clock_diagnostic.h"

roundie_clock_offset_t roundie_clock_offset(const roundie_clock_sample_t *sample)
{
    roundie_clock_offset_t result = {0};
    /* Bound input domain before arithmetic, including long uptime. Invalid
     * endpoints cannot wrap subtraction into a plausible measurement. */
    const int64_t min_wall = INT64_C(946684800000000);
    const int64_t max_wall = INT64_C(4102444800000000);
    if (sample == 0 || !sample->rtc_valid || sample->oscillator_stopped ||
        !sample->wall_valid || !sample->read_ok ||
        sample->rtc_seconds < INT64_C(946684800) ||
        sample->rtc_seconds > INT64_C(4102444799) ||
        sample->wall_before_us < min_wall || sample->wall_before_us >= max_wall ||
        sample->wall_after_us < min_wall || sample->wall_after_us >= max_wall ||
        sample->mono_before_us < 0 || sample->mono_after_us < sample->mono_before_us) {
        return result;
    }
    int64_t duration = sample->mono_after_us - sample->mono_before_us;
    int64_t wall_duration = sample->wall_after_us - sample->wall_before_us;
    /* Includes bus contention, control read, calendar burst and error probe.
     * Reject long reads and observable clock discontinuities. The 2 ms gate
     * allows timestamp-call overhead; it is not added measurement precision. */
    if (duration > INT64_C(1000000) || wall_duration < 0 ||
        wall_duration > duration + 2000 || wall_duration < duration - 2000) {
        return result;
    }
    result.usable = true;
    result.lower_us = sample->rtc_seconds * INT64_C(1000000) - sample->wall_after_us;
    result.upper_us = (sample->rtc_seconds + 1) * INT64_C(1000000) - sample->wall_before_us;
    return result;
}
