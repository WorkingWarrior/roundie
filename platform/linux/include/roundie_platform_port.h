#pragma once

/* Linux port of roundie_platform.h (docs/roundie-core.md, D1). */

#include <pthread.h>
#include <stdint.h>
#include <time.h>

typedef pthread_mutex_t roundie_lock_t;
#define ROUNDIE_LOCK_INITIALIZER PTHREAD_MUTEX_INITIALIZER

typedef pthread_t roundie_thread_t;
#define ROUNDIE_THREAD_NONE ((pthread_t)0)

/* CLOCK_MONOTONIC from the first call; esp_timer also starts near zero. */
int64_t roundie_time_us(void);

static inline void roundie_lock(roundie_lock_t *lock)
{
    pthread_mutex_lock(lock);
}

static inline void roundie_unlock(roundie_lock_t *lock)
{
    pthread_mutex_unlock(lock);
}

static inline roundie_thread_t roundie_thread_self(void)
{
    return pthread_self();
}

/* Not measured on Linux. */
static inline uint32_t roundie_thread_stack_unused(void)
{
    return 0;
}

/* platform.c: under the desktop's --virtual-time, waits for virtual time. */
void roundie_desktop_sleep_ms(uint32_t ms);

static inline void roundie_sleep_ms(uint32_t ms)
{
    roundie_desktop_sleep_ms(ms);
}
