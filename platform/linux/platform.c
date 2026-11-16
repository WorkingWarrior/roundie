#include "roundie_platform.h"

#include <stdatomic.h>
#include "clock.h"

/* Forward jumps of emulated time (clock.h); never decreases. */
static atomic_llong advance_us;

int64_t roundie_time_us(void)
{
    static int64_t origin_ns;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    /* The first caller is the main thread before any worker starts. */
    if (origin_ns == 0) {
        origin_ns = ns;
    }
    return (ns - origin_ns) / 1000 + atomic_load(&advance_us);
}

void roundie_desktop_time_advance(int64_t us)
{
    if (us > 0) {
        atomic_fetch_add(&advance_us, us);
    }
}
