#include "local_time.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define TAG "local_time"

/* newlib's TZ state is process-global: setenv/tzset and localtime_r are only
 * called under this mutex. Nothing else in the firmware uses local time. */
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static char zone[ROUNDIE_TIME_ZONE_CAPACITY] = "UTC0";
static uint32_t zone_revision;
static bool nvs_ready;
static roundie_local_wall_t wall_valid;

static void apply(const char *rule)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    strcpy(zone, rule);
    setenv("TZ", zone, 1);
    tzset();
    zone_revision++;
    xSemaphoreGive(mutex);
}

void roundie_local_time_init(bool nvs_available, roundie_local_wall_t wall)
{
    mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    nvs_ready = nvs_available;
    wall_valid = wall;
    char stored[ROUNDIE_TIME_ZONE_CAPACITY] = "UTC0";
    nvs_handle_t handle;
    if (nvs_ready && nvs_open("clock", NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(stored);
        if (nvs_get_str(handle, "tz", stored, &size) != ESP_OK || !roundie_time_zone_valid(stored)) {
            strcpy(stored, "UTC0");
        }
        nvs_close(handle);
    }
    apply(stored);
    ESP_LOGI(TAG, "Time zone %s", stored);
}

esp_err_t roundie_local_time_set_zone(const char *rule)
{
    if (!roundie_time_zone_valid(rule)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nvs_ready) {
        return ESP_ERR_INVALID_STATE;
    }
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
    if (error == ESP_OK) {
        apply(rule);
        ESP_LOGI(TAG, "Time zone set to %s", rule);
    }
    return error;
}

void roundie_local_time_zone(char out[ROUNDIE_TIME_ZONE_CAPACITY])
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    strcpy(out, zone);
    xSemaphoreGive(mutex);
}

bool roundie_local_time_now(roundie_elf_clock_t *out)
{
    uint32_t sync_count = 0;
    if (mutex == NULL || wall_valid == NULL || !wall_valid(&sync_count)) {
        return false;
    }
    struct timeval now;
    struct tm local;
    xSemaphoreTake(mutex, portMAX_DELAY);
    gettimeofday(&now, NULL);
    int64_t monotonic_us = esp_timer_get_time();
    bool converted = localtime_r(&now.tv_sec, &local) != NULL;
    uint32_t revision = zone_revision + sync_count;
    xSemaphoreGive(mutex);
    if (!converted) {
        return false;
    }
    int64_t days = roundie_time_days_from_civil((int64_t)local.tm_year + 1900,
                                                (unsigned)local.tm_mon + 1, (unsigned)local.tm_mday);
    if (days < 0 || days > UINT32_MAX) {
        return false;
    }
    uint32_t seconds = (uint32_t)(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
    out->monotonic_us = monotonic_us;
    out->days = (uint32_t)days;
    out->ms_of_day = seconds * 1000U + (uint32_t)(now.tv_usec / 1000);
    out->revision = revision;
    return true;
}
