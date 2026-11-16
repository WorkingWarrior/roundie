#include "roundie_platform.h"

#include <stdatomic.h>
#include "clock.h"

/* Forward jumps of emulated time (clock.h); never decreases. */
static atomic_llong advance_us;
static atomic_bool frozen;
static atomic_bool released; /* sleeps no longer wait for virtual time */
static _Thread_local bool follows_virtual_time;
static _Atomic int64_t follower_until; /* wake time of the sleeping follower, 0 while it runs */
static _Atomic int64_t follower_gate;  /* the GUI has done its work up to this time */

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
    if (atomic_load(&frozen)) {
        return atomic_load(&advance_us);
    }
    return (ns - origin_ns) / 1000 + atomic_load(&advance_us);
}

void roundie_desktop_time_freeze(void)
{
    /* Starts at 1 s: callers treat 0 as "never". */
    atomic_store(&advance_us, 1000000);
    atomic_store(&frozen, true);
}

bool roundie_desktop_time_frozen(void)
{
    return atomic_load(&frozen);
}

void roundie_desktop_time_release(void)
{
    atomic_store(&released, true);
}

void roundie_desktop_time_follow(void)
{
    follows_virtual_time = true;
}

void roundie_desktop_time_gui_done(void)
{
    atomic_store(&follower_gate, atomic_load(&advance_us));
}

void roundie_desktop_time_wait_follower(void)
{
    while (atomic_load(&follower_until) <= atomic_load(&advance_us) && !atomic_load(&released)) {
        roundie_desktop_sleep_real_ms(1);
    }
}

void roundie_desktop_sleep_real_ms(uint32_t ms)
{
    if (ms == 0) {
        ms = 1;
    }
    struct timespec delay = {
        .tv_sec = (time_t)(ms / 1000U),
        .tv_nsec = (long)(ms % 1000U) * 1000000L,
    };
    nanosleep(&delay, NULL);
}

/* Under virtual time a following thread's sleep lasts ms of virtual time, so
 * the system worker's waits (the RTC progress check, its poll period) keep their
 * meaning however fast the GUI loop advances the clock. Other threads sleep in
 * real time: the executor's waits for the GUI must not wait for a clock that
 * stands still while the executor is busy. */
void roundie_desktop_sleep_ms(uint32_t ms)
{
    if (!atomic_load(&frozen) || !follows_virtual_time) {
        roundie_desktop_sleep_real_ms(ms);
        return;
    }
    int64_t until = atomic_load(&advance_us) + (int64_t)(ms == 0 ? 1 : ms) * 1000;
    atomic_store(&follower_until, until);
    while ((atomic_load(&advance_us) < until || atomic_load(&follower_gate) < until) &&
           !atomic_load(&released)) {
        roundie_desktop_sleep_real_ms(1);
    }
    atomic_store(&follower_until, 0);
}

void roundie_desktop_time_advance(int64_t us)
{
    if (us > 0) {
        atomic_fetch_add(&advance_us, us);
    }
}
