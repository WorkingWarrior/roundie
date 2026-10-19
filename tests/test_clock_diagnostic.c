#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "clock_diagnostic.h"

static roundie_clock_sample_t baseline(void)
{
    return (roundie_clock_sample_t){
        .rtc_seconds = 1792390000,
        .wall_before_us = INT64_C(1792390000400000),
        .wall_after_us = INT64_C(1792390000402000),
        .mono_before_us = INT64_C(6000000000000),
        .mono_after_us = INT64_C(6000000002000),
        .rtc_valid = true, .wall_valid = true, .read_ok = true,
    };
}

int main(void)
{
    assert(!roundie_clock_offset(NULL).usable);
    roundie_clock_sample_t s = baseline();
    roundie_clock_offset_t o = roundie_clock_offset(&s);
    assert(o.usable && o.lower_us == -402000 && o.upper_us == 600000);
    /* Quantization unresolved at a second boundary, including non-atomic read. */
    s.wall_before_us += 599000;
    s.wall_after_us += 599000;
    o = roundie_clock_offset(&s);
    assert(o.usable && o.lower_us == -1001000 && o.upper_us == 1000);
    s.rtc_seconds++;
    o = roundie_clock_offset(&s);
    assert(o.usable && o.lower_us == -1000 && o.upper_us == 1001000);
    /* Sign is RTC minus wall, never unsigned arithmetic. */
    s = baseline();
    s.rtc_seconds -= 2;
    o = roundie_clock_offset(&s);
    assert(o.usable && o.lower_us == -2402000 && o.upper_us == -1400000);
    s.rtc_seconds += 4;
    o = roundie_clock_offset(&s);
    assert(o.lower_us == 1598000 && o.upper_us == 2600000);
    /* Bus contention widens uncertainty; no fake subsecond RTC precision. */
    s = baseline();
    s.wall_after_us += 300000;
    s.mono_after_us += 300000;
    o = roundie_clock_offset(&s);
    assert(o.usable && o.upper_us - o.lower_us == 1302000);
    for (int invalid = 0; invalid < 10; invalid++) {
        s = baseline();
        switch (invalid) {
        case 0: s.rtc_valid = false; break;
        case 1: s.oscillator_stopped = true; break;
        case 2: s.wall_valid = false; break;
        case 3: s.read_ok = false; break;
        case 4: s.wall_after_us += 10000; break;
        case 5: s.wall_after_us -= 10000; break;
        case 6: s.mono_after_us = INT64_MAX; break;
        case 7: s.rtc_seconds = INT64_MAX; break;
        case 8: s.wall_after_us = INT64_MIN; break;
        case 9: s.mono_before_us = -1; break;
        }
        assert(!roundie_clock_offset(&s).usable);
    }
    /* Startup sets sampled whole seconds: true phase remains unknown. */
    s = baseline();
    s.wall_before_us = s.rtc_seconds * INT64_C(1000000);
    s.wall_after_us = s.wall_before_us + 2000;
    o = roundie_clock_offset(&s);
    assert(o.usable && o.lower_us == -2000 && o.upper_us == 1000000);
    /* Supported epoch endpoints, beyond 32-bit signed time and 69-day uptime. */
    s.rtc_seconds = 4102444799;
    s.wall_before_us = INT64_C(4102444799000000);
    s.wall_after_us = s.wall_before_us + 2000;
    o = roundie_clock_offset(&s);
    assert(o.usable && o.lower_us == -2000 && o.upper_us == 1000000);
    puts("clock diagnostic C tests passed");
}
