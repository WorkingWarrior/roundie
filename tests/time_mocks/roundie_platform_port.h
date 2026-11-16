#pragma once

/* Host tests of the shared time code (tests/test_wall_clock.c): one thread
 * and a virtual monotonic clock that only the test moves. roundie_sleep_ms()
 * advances it instead of waiting. A lock taken twice fails at once. */

#include <assert.h>
#include <stdint.h>

typedef int roundie_lock_t;
#define ROUNDIE_LOCK_INITIALIZER 0
typedef int roundie_thread_t;
#define ROUNDIE_THREAD_NONE 0

int64_t roundie_time_us(void);
void roundie_sleep_ms(uint32_t ms);

static inline void roundie_lock(roundie_lock_t *lock)
{
    assert(*lock == 0);
    *lock = 1;
}

static inline void roundie_unlock(roundie_lock_t *lock)
{
    assert(*lock == 1);
    *lock = 0;
}

static inline roundie_thread_t roundie_thread_self(void)
{
    return 1;
}

static inline uint32_t roundie_thread_stack_unused(void)
{
    return 0;
}
