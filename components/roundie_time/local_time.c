#include "local_time.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "roundie_clock_port.h"
#include "roundie_platform.h"

#define TAG "local_time"

/* The C library's TZ state is process-global: setenv/tzset and localtime_r
 * are only called under the port's zone lock. Nothing else in the firmware
 * uses local time. */
static bool initialized;
static char zone[ROUNDIE_TIME_ZONE_CAPACITY] = "UTC0";
static uint32_t zone_revision;
static bool storage_ready;
static roundie_local_wall_t wall_valid;

static void apply(const char *rule)
{
    roundie_clock_port_zone_lock();
    strcpy(zone, rule);
    setenv("TZ", zone, 1);
    tzset();
    zone_revision++;
    roundie_clock_port_zone_unlock();
}

void roundie_local_time_init(bool storage_available, roundie_local_wall_t wall)
{
    roundie_clock_port_zone_lock_init();
    storage_ready = storage_available;
    wall_valid = wall;
    char stored[ROUNDIE_TIME_ZONE_CAPACITY] = "UTC0";
    if (!storage_ready || !roundie_clock_port_zone_load(stored) || !roundie_time_zone_valid(stored)) {
        strcpy(stored, "UTC0");
    }
    apply(stored);
    initialized = true;
    ESP_LOGI(TAG, "Time zone %s", stored);
}

esp_err_t roundie_local_time_set_zone(const char *rule)
{
    if (!roundie_time_zone_valid(rule)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!storage_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = roundie_clock_port_zone_store(rule);
    if (error == ESP_OK) {
        apply(rule);
        ESP_LOGI(TAG, "Time zone set to %s", rule);
    }
    return error;
}

void roundie_local_time_zone(char out[ROUNDIE_TIME_ZONE_CAPACITY])
{
    roundie_clock_port_zone_lock();
    strcpy(out, zone);
    roundie_clock_port_zone_unlock();
}

static bool local_fields(const struct tm *local, uint32_t microseconds, int64_t monotonic_us, uint32_t revision,
                          roundie_elf_clock_t *out)
{
    int64_t days = roundie_time_days_from_civil((int64_t)local->tm_year + 1900,
                                                (unsigned)local->tm_mon + 1, (unsigned)local->tm_mday);
    if (days < 0 || days > UINT32_MAX) {
        return false;
    }
    uint32_t seconds = (uint32_t)(local->tm_hour * 3600 + local->tm_min * 60 + local->tm_sec);
    out->monotonic_us = monotonic_us;
    out->days = (uint32_t)days;
    out->ms_of_day = seconds * 1000U + microseconds / 1000U;
    out->revision = revision;
    return true;
}

bool roundie_local_time_now(roundie_elf_clock_t *out)
{
    uint32_t sync_count = 0;
    if (!initialized || wall_valid == NULL || !wall_valid(&sync_count)) {
        return false;
    }
    struct tm local;
    roundie_clock_port_zone_lock();
    int64_t wall_us = roundie_clock_port_wall_us();
    int64_t monotonic_us = roundie_time_us();
    time_t seconds = (time_t)(wall_us / 1000000);
    bool converted = localtime_r(&seconds, &local) != NULL;
    uint32_t revision = zone_revision + sync_count;
    roundie_clock_port_zone_unlock();
    if (!converted) {
        return false;
    }
    return local_fields(&local, (uint32_t)(wall_us % 1000000), monotonic_us, revision, out);
}

bool roundie_local_time_sample(roundie_elf_clock_t *out, int64_t *utc_seconds)
{
    uint32_t sync_count = 0;
    if (!initialized || wall_valid == NULL || !wall_valid(&sync_count)) {
        return false;
    }
    struct tm local;
    roundie_clock_port_zone_lock();
    int64_t wall_us = roundie_clock_port_wall_us();
    int64_t monotonic_us = roundie_time_us();
    time_t seconds = (time_t)(wall_us / 1000000);
    bool converted = localtime_r(&seconds, &local) != NULL;
    uint32_t revision = zone_revision + sync_count;
    roundie_clock_port_zone_unlock();
    if (!converted) {
        return false;
    }
    *utc_seconds = (int64_t)seconds;
    return local_fields(&local, (uint32_t)(wall_us % 1000000), monotonic_us, revision, out);
}
