#include "roundie_boot_guard.h"

#include <stddef.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
static portMUX_TYPE diag_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&diag_lock)
#define UNLOCK() portEXIT_CRITICAL(&diag_lock)
#else
#include "esp_system.h"
#define RTC_NOINIT_ATTR
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif

/* Diagnostic only (roundie_boot_guard.h "Safe Mode cause diagnostics"). Its own
 * RTC record, separate from the boot guard's: the guard never reads it. */
#define SESSION_MAGIC UINT32_C(0x52424431)
#define SESSION_VERSION 1U
#define BATTERY_PERIOD_US INT64_C(60000000)

static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t version;
    roundie_boot_session_t session;
    uint32_t crc;
} record;

static roundie_boot_session_t previous;
static roundie_boot_cause_t stored;
static uint32_t boot_raw_reset;
static uint8_t boot_image[8];
static uint32_t pending_site = ROUNDIE_SAFE_SITE_NONE;
static int64_t last_battery_us = -1;

static uint32_t checksum(void)
{
    const uint8_t *bytes = (const uint8_t *)&record;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < offsetof(typeof(record), crc); i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

void roundie_boot_diag_begin(uint32_t raw_reset, const uint8_t image[32])
{
    LOCK();
    /* RTC RAM is undefined after power-on and brownout: never interpret it. */
    bool kept = raw_reset != ESP_RST_POWERON && raw_reset != ESP_RST_BROWNOUT &&
        record.magic == SESSION_MAGIC && record.version == SESSION_VERSION && record.crc == checksum();
    memset(&previous, 0, sizeof(previous));
    if (kept) {
        previous = record.session;
        previous.valid = true;
    }
    memset(&record, 0, sizeof(record));
    record.magic = SESSION_MAGIC;
    record.version = SESSION_VERSION;
    memcpy(record.session.image, image, sizeof(record.session.image));
    record.crc = checksum();
    boot_raw_reset = raw_reset;
    memcpy(boot_image, image, sizeof(boot_image));
    pending_site = ROUNDIE_SAFE_SITE_NONE;
    last_battery_us = -1;
    UNLOCK();
}

void roundie_boot_diag_restart_site(roundie_restart_site_t site, int64_t now_us)
{
    LOCK();
    record.session.restart_site = site;
    record.session.uptime_ms = (uint32_t)(now_us / 1000);
    record.crc = checksum();
    UNLOCK();
}

void roundie_boot_diag_safe_site(roundie_safe_site_t site)
{
    LOCK();
    if (pending_site == ROUNDIE_SAFE_SITE_NONE) {
        pending_site = site;
    }
    UNLOCK();
}

void roundie_boot_diag_battery(uint16_t voltage_mv, int16_t current_ma, uint16_t soc_percent,
                               uint16_t battery_flags, int32_t temperature_deci_c, int64_t rtc_unix,
                               int64_t now_us)
{
    LOCK();
    if (last_battery_us < 0 || now_us - last_battery_us >= BATTERY_PERIOD_US) {
        last_battery_us = now_us;
        roundie_boot_session_t *session = &record.session;
        session->battery_valid = true;
        session->voltage_mv = voltage_mv;
        session->current_ma = current_ma;
        session->soc_percent = soc_percent;
        session->battery_flags = battery_flags;
        session->temperature_deci_c = temperature_deci_c;
        session->battery_uptime_ms = (uint32_t)(now_us / 1000);
        session->rtc_unix = rtc_unix;
        session->uptime_ms = session->battery_uptime_ms;
        record.crc = checksum();
    }
    UNLOCK();
}

void roundie_boot_diag_previous(roundie_boot_session_t *out)
{
    LOCK();
    *out = previous;
    UNLOCK();
}

void roundie_boot_diag_cause(uint32_t latch, roundie_boot_cause_t *out)
{
    roundie_boot_status_t status;
    roundie_boot_status(&status);
    LOCK();
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->latch = latch;
    out->site = pending_site;
    if (out->site == ROUNDIE_SAFE_SITE_NONE) {
        /* No caller named a site: the boot guard decided from the RTC record
         * (abnormal reset after STABLE, boot loop, failed trial) or another
         * roundie_boot_enter_safe() did. */
        out->site = status.retained_valid && (latch == ROUNDIE_SAFE_CORE || latch == ROUNDIE_SAFE_BOOT_LOOP) ?
            ROUNDIE_SAFE_SITE_RETAINED : ROUNDIE_SAFE_SITE_OTHER;
    }
    out->raw_reset = boot_raw_reset;
    out->previous_stage = status.previous_stage;
    out->failures = status.failures;
    out->retained_valid = status.retained_valid;
    memcpy(out->fault_app, status.fault_app, sizeof(out->fault_app));
    out->fault_app[sizeof(out->fault_app) - 1] = '\0';
    memcpy(out->image, boot_image, sizeof(out->image));
    out->previous = previous;
    UNLOCK();
}

void roundie_boot_diag_stored_cause(roundie_boot_cause_t *out)
{
    LOCK();
    *out = stored;
    UNLOCK();
}

void roundie_boot_diag_set_stored_cause(const roundie_boot_cause_t *cause)
{
    LOCK();
    stored = *cause;
    UNLOCK();
}
