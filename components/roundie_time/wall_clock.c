#include "roundie_wall_clock.h"

#include <string.h>
#include "clock_retention.h"
#include "esp_log.h"
#include "roundie_clock_port.h"
#include "roundie_platform.h"

/* The firmware's log tag before M3, so logs read the same. */
static const char *TAG = "roundie";

static roundie_lock_t lock = ROUNDIE_LOCK_INITIALIZER;
/* Under lock. */
static bool valid;
static bool published;
static uint32_t sync_count;
static int64_t sync_at_us;
/* Set by init() before the system worker exists, then read-only. */
static bool has_rtc;
/* Startup, then the system worker only. */
static bool sync_failed; /* A failed explicit SET requires another successful SET. */

static void set_valid(bool value)
{
    roundie_lock(&lock);
    valid = value;
    roundie_unlock(&lock);
}

static bool set_wall_clock(int64_t seconds)
{
    if (!roundie_clock_port_wall_set(seconds)) {
        ESP_LOGE(TAG, "System wall clock set failed: %s", esp_err_to_name(ESP_FAIL));
        return false;
    }
    int64_t now = roundie_time_us();
    roundie_lock(&lock);
    sync_count++;
    sync_at_us = now;
    roundie_unlock(&lock);
    return true;
}

static bool sync_wall_clock(const roundie_rtc_sample_t *sample)
{
    int64_t seconds;
    if (sample->state != ROUNDIE_SENSOR_VALID || sample->oscillator_stopped ||
        !roundie_rtc_unix_seconds(&sample->time, &seconds)) {
        return false;
    }
    if (!set_wall_clock(seconds)) {
        return false;
    }
    ESP_LOGI(TAG, "System wall clock synchronized from valid RTC (UTC)");
    return true;
}

/* After a trusted SET without an RTC. The platform keeps the time across a
 * software reset; this record marks it trusted. */
static void retain_wall_clock(void)
{
    roundie_clock_port_retention_align();
    roundie_clock_retention_record(roundie_clock_port_retained(), roundie_clock_port_wall_us(),
                                   roundie_clock_port_retention_timer_us());
}

/* Startup without an RTC. Never restores a stored time: it accepts the system
 * time that the platform kept in the RTC domain, or invalidates it. */
static bool accept_retained_wall_clock(void)
{
    int reason = 0;
    bool kept = roundie_clock_port_reset_kept_domain(&reason);
    bool accepted = kept && roundie_clock_retention_accept(roundie_clock_port_retained(),
                                                           roundie_clock_port_wall_us(),
                                                           roundie_clock_port_retention_timer_us());
    if (!accepted) {
        roundie_clock_retention_clear(roundie_clock_port_retained());
        ESP_LOGW(TAG, "System wall clock unsynchronized after reset reason %d; no external RTC", reason);
        return false;
    }
    retain_wall_clock();
    int64_t now = roundie_time_us();
    roundie_lock(&lock);
    sync_count++;
    sync_at_us = now;
    roundie_unlock(&lock);
    ESP_LOGI(TAG, "System wall clock kept across reset reason %d (UTC)", reason);
    return true;
}

void roundie_wall_clock_init(bool board_has_rtc)
{
    has_rtc = board_has_rtc;
}

bool roundie_wall_clock_start(const roundie_rtc_sample_t *rtc)
{
    roundie_lock(&lock);
    valid = false;
    published = false;
    sync_count = 0;
    sync_at_us = 0;
    roundie_unlock(&lock);
    sync_failed = false;
    bool trusted;
    if (has_rtc) {
        trusted = sync_wall_clock(rtc);
        if (!trusted) {
            ESP_LOGW(TAG, "System wall clock unsynchronized; RTC time not trusted");
        }
    } else {
        trusted = accept_retained_wall_clock();
    }
    set_valid(trusted);
    return trusted;
}

void roundie_wall_clock_publish(void)
{
    roundie_lock(&lock);
    published = true;
    roundie_unlock(&lock);
}

static bool current_valid(void)
{
    roundie_lock(&lock);
    bool value = valid;
    roundie_unlock(&lock);
    return value;
}

void roundie_wall_clock_poll(const roundie_rtc_sample_t *rtc)
{
    bool wall_valid = current_valid();
    if (rtc->state == ROUNDIE_SENSOR_VALID && !wall_valid && !sync_failed) {
        set_valid(sync_wall_clock(rtc));
    } else if (has_rtc && rtc->state != ROUNDIE_SENSOR_VALID && wall_valid) {
        set_valid(false);
        ESP_LOGW(TAG, "System wall clock marked unsynchronized after RTC failure");
    }
    if (!has_rtc && current_valid()) {
        roundie_clock_port_retention_align();
    }
}

/* SET without an RTC: the system clock itself. */
static const char *set_without_rtc(int64_t seconds, bool *wall_valid, esp_err_t *error)
{
    roundie_clock_retention_clear(roundie_clock_port_retained());
    *wall_valid = set_wall_clock(seconds);
    if (!*wall_valid) {
        *error = ESP_FAIL;
        return "CLOCK";
    }
    retain_wall_clock();
    sync_failed = false;
    ESP_LOGI(TAG, "System wall clock set by explicit time sync (UTC)");
    return "OK";
}

/* SET with an RTC: write it, check that it runs, then sync from it. */
static const char *set_with_rtc(roundie_board_t *board, int64_t seconds, roundie_rtc_sample_t *rtc,
                                bool *wall_valid, esp_err_t *error)
{
    roundie_rtc_time_t calendar;
    roundie_rtc_from_unix(seconds, &calendar);
    *error = roundie_board_rtc_set_trusted(board, &calendar);
    if (*error != ESP_OK) {
        return "RTC_SET";
    }
    roundie_rtc_sample_t first;
    roundie_board_rtc_read(board, &first);
    if (first.state != ROUNDIE_SENSOR_VALID) {
        *error = first.error;
        return "RTC_READ";
    }
    int64_t first_seconds;
    int64_t next_seconds;
    roundie_rtc_unix_seconds(&first.time, &first_seconds);
    /* Yield only the system worker. GUI and touch continue. */
    roundie_sleep_ms(1200);
    roundie_board_rtc_read(board, rtc);
    if (rtc->state != ROUNDIE_SENSOR_VALID) {
        *error = rtc->error;
        return rtc->oscillator_stopped ? "RTC_INVALID" : "RTC_READ";
    }
    if (!roundie_rtc_unix_seconds(&rtc->time, &next_seconds) ||
        !roundie_rtc_progress_matches(first_seconds, next_seconds, rtc->observed_us - first.observed_us)) {
        *error = ESP_ERR_INVALID_RESPONSE;
        return "RTC_VERIFY";
    }
    *wall_valid = sync_wall_clock(rtc);
    if (!*wall_valid) {
        *error = ESP_FAIL;
        return "CLOCK";
    }
    sync_failed = false;
    return "OK";
}

const char *roundie_wall_clock_request(roundie_board_t *board, const char *result, bool set, int64_t seconds,
                                       roundie_rtc_sample_t *rtc, esp_err_t *error)
{
    *error = ESP_OK;
    if (board == NULL) {
        /* Board not published: start() may be running. Touch no state. */
        roundie_board_rtc_read(board, rtc);
        if (strcmp(result, "OK") == 0 && set) {
            result = "CLOCK";
            *error = ESP_ERR_INVALID_STATE;
        } else if (strcmp(result, "OK") == 0 && has_rtc && rtc->state != ROUNDIE_SENSOR_VALID) {
            result = rtc->state == ROUNDIE_SENSOR_INVALID ? "RTC_INVALID" : "RTC_READ";
            *error = rtc->error;
        }
        return result;
    }
    bool wall_valid = current_valid();
    if (strcmp(result, "OK") == 0 && set) {
        wall_valid = false;
        sync_failed = true;
        set_valid(false);
        if (has_rtc) {
            result = set_with_rtc(board, seconds, rtc, &wall_valid, error);
        } else {
            result = set_without_rtc(seconds, &wall_valid, error);
        }
    }
    /* Fresh status, also on failure. Never substitute a cached valid sample. */
    roundie_board_rtc_read(board, rtc);
    if (has_rtc && rtc->state != ROUNDIE_SENSOR_VALID) {
        wall_valid = false;
        if (strcmp(result, "OK") == 0) {
            result = rtc->state == ROUNDIE_SENSOR_INVALID ? "RTC_INVALID" : "RTC_READ";
            *error = rtc->error;
        }
    } else if (has_rtc && strcmp(result, "OK") == 0 && !wall_valid && !sync_failed) {
        wall_valid = sync_wall_clock(rtc);
        if (!wall_valid) {
            result = "CLOCK";
            *error = ESP_FAIL;
        }
    }
    if (set && strcmp(result, "OK") != 0) {
        sync_failed = true;
        wall_valid = false;
        if (!has_rtc) {
            roundie_clock_retention_clear(roundie_clock_port_retained());
        }
    }
    set_valid(wall_valid);
    return result;
}

bool roundie_wall_clock_valid(uint32_t *sync_count_out)
{
    roundie_lock(&lock);
    bool value = valid && published;
    *sync_count_out = sync_count;
    roundie_unlock(&lock);
    return value;
}

void roundie_wall_clock_status(roundie_wall_clock_status_t *out)
{
    roundie_lock(&lock);
    out->valid = valid && published;
    out->sync_count = published ? sync_count : 0;
    out->sync_at_us = published ? sync_at_us : 0;
    roundie_unlock(&lock);
}
