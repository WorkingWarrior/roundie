/* ESP-IDF port of roundie_clock_port.h: the calls main.c made before M3. */

#include "roundie_clock_port.h"

#include <string.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_newlib.h"
#include "esp_rtc_time.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "roundie_board.h"

_Static_assert(sizeof(time_t) == 8, "RTC requires ESP-IDF 64-bit time_t");

int64_t roundie_clock_port_wall_us(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (int64_t)now.tv_sec * 1000000 + now.tv_usec;
}

bool roundie_clock_port_wall_set(int64_t seconds)
{
    struct timeval now = {
        .tv_sec = (time_t)seconds,
        .tv_usec = 0,
    };
    return settimeofday(&now, NULL) == 0;
}

int64_t roundie_clock_port_retention_timer_us(void)
{
    return (int64_t)esp_rtc_get_time_us();
}

/* Aligns ESP-IDF's RTC-domain boot time with the running clock, so that time
 * read back after a reset matches the time before it. System worker only. */
void roundie_clock_port_retention_align(void)
{
    esp_sync_timekeeping_timers();
}

#if (ROUNDIE_BOARD_CAPABILITIES & ROUNDIE_BOARD_CAP_RTC) == 0
/* Kept by a software reset in RTC slow memory; arbitrary after power-on. */
static RTC_NOINIT_ATTR roundie_clock_retention_t retained_clock;
#else
/* Never used with an external RTC; ordinary memory keeps RTC_NOINIT free. */
static roundie_clock_retention_t retained_clock;
#endif

roundie_clock_retention_t *roundie_clock_port_retained(void)
{
    return &retained_clock;
}

bool roundie_clock_port_reset_kept_domain(int *reason_out)
{
    esp_reset_reason_t reason = esp_reset_reason();
    *reason_out = (int)reason;
    switch (reason) {
    case ESP_RST_SW:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_DEEPSLEEP:
    case ESP_RST_USB:
    case ESP_RST_JTAG:
    case ESP_RST_CPU_LOCKUP:
        return true;
    default:
        /* Power-on, brownout, power glitch, external pin and unknown resets
         * may have lost the RTC domain: time stays invalid until a sync. */
        return false;
    }
}

static StaticSemaphore_t zone_mutex_storage;
static SemaphoreHandle_t zone_mutex;

void roundie_clock_port_zone_lock_init(void)
{
    zone_mutex = xSemaphoreCreateMutexStatic(&zone_mutex_storage);
}

void roundie_clock_port_zone_lock(void)
{
    xSemaphoreTake(zone_mutex, portMAX_DELAY);
}

void roundie_clock_port_zone_unlock(void)
{
    xSemaphoreGive(zone_mutex);
}

bool roundie_clock_port_zone_load(char rule[ROUNDIE_TIME_ZONE_CAPACITY])
{
    nvs_handle_t handle;
    if (nvs_open("clock", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t size = ROUNDIE_TIME_ZONE_CAPACITY;
    bool loaded = nvs_get_str(handle, "tz", rule, &size) == ESP_OK;
    nvs_close(handle);
    return loaded;
}

esp_err_t roundie_clock_port_zone_store(const char *rule)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("clock", NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_str(handle, "tz", rule);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}
