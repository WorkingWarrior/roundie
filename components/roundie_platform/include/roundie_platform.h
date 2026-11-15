#pragma once

/* Roundie Core platform seam (docs/roundie-core.md).
 *
 * Portable Core code uses only these few primitives instead of FreeRTOS or
 * esp_timer directly. Each platform supplies roundie_platform_port.h, which
 * defines them, usually as inline wrappers:
 *
 *   int64_t roundie_time_us(void)
 *       Monotonic microseconds since start; never goes backwards.
 *   roundie_lock_t, ROUNDIE_LOCK_INITIALIZER
 *   void roundie_lock(roundie_lock_t *lock)
 *   void roundie_unlock(roundie_lock_t *lock)
 *       Short critical section. Code inside must not block, sleep, log or
 *       call LVGL: on ESP-IDF it is a spinlock with interrupts disabled.
 *   roundie_thread_t, ROUNDIE_THREAD_NONE
 *   roundie_thread_t roundie_thread_self(void)
 *       Identity of the calling task or thread, for owner checks only.
 *   uint32_t roundie_thread_stack_unused(void)
 *       Bytes of the calling task's stack never used so far; 0 where the
 *       platform cannot measure it.
 *   void roundie_sleep_ms(uint32_t ms)
 *       Yield the calling task for at least one scheduler tick.
 *
 * Status codes stay esp_err_t; a platform without ESP-IDF supplies a
 * compatible esp_err.h with the same values. */

#include <stdbool.h>
#include <stdint.h>
#include "roundie_platform_port.h"
