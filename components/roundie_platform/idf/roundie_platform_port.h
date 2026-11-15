#pragma once

/* ESP-IDF port of roundie_platform.h: thin inline wrappers, so Core code
 * compiles to the same calls it made before the seam existed. */

#include <stdint.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef portMUX_TYPE roundie_lock_t;
#define ROUNDIE_LOCK_INITIALIZER portMUX_INITIALIZER_UNLOCKED

typedef TaskHandle_t roundie_thread_t;
#define ROUNDIE_THREAD_NONE NULL

static inline int64_t roundie_time_us(void)
{
    return esp_timer_get_time();
}

static inline void roundie_lock(roundie_lock_t *lock)
{
    portENTER_CRITICAL(lock);
}

static inline void roundie_unlock(roundie_lock_t *lock)
{
    portEXIT_CRITICAL(lock);
}

static inline roundie_thread_t roundie_thread_self(void)
{
    return xTaskGetCurrentTaskHandle();
}

static inline uint32_t roundie_thread_stack_unused(void)
{
    return (uint32_t)uxTaskGetStackHighWaterMark(NULL);
}

static inline void roundie_sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}
