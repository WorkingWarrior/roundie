#include "roundie_boot_guard.h"

#include <stddef.h>
#include <string.h>

#include "esp_system.h"
#include "nvs.h"

static nvs_handle_t handle;
static bool opened;
static uint32_t saved = UINT32_MAX;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t reason;
    uint32_t crc;
} latch_record_t;

/* Diagnostic cause record, never read by any decision (roundie_boot_guard.h
 * "Safe Mode cause diagnostics"). Written in its own NVS commit after the
 * latch commit succeeded, so a full or damaged NVS page cannot fail, delay or
 * change the latch write. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    roundie_boot_cause_t cause;
    uint32_t crc;
} cause_record_t;
_Static_assert(sizeof(cause_record_t) <= ROUNDIE_BOOT_DIAG_BLOB_MAX, "cause record exceeds the diagnostic limit");

static uint32_t crc_bytes(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t value = UINT32_MAX;
    for (size_t i = 0; i < length; i++) {
        value ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            value = (value >> 1) ^ (UINT32_C(0xedb88320) & (0U - (value & 1U)));
        }
    }
    return ~value;
}

static void load_cause(void)
{
    cause_record_t record = {0};
    size_t size = sizeof(record);
    if (nvs_get_blob(handle, "cause", &record, &size) == ESP_OK && size == sizeof(record) &&
        record.magic == UINT32_C(0x52424331) && record.version == 1 &&
        record.crc == crc_bytes(&record, offsetof(cause_record_t, crc))) {
        record.cause.fault_app[sizeof(record.cause.fault_app) - 1] = '\0';
        roundie_boot_diag_set_stored_cause(&record.cause);
    }
}

static void write_cause(uint32_t latch)
{
    cause_record_t record;
    memset(&record, 0, sizeof(record));
    record.magic = UINT32_C(0x52424331);
    record.version = 1;
    roundie_boot_diag_cause(latch, &record.cause);
    record.crc = crc_bytes(&record, offsetof(cause_record_t, crc));
    roundie_diag_store_t result = roundie_boot_diag_store("cause", &record, sizeof(record));
    if (result == ROUNDIE_DIAG_STORED || result == ROUNDIE_DIAG_UNCHANGED) {
        roundie_boot_diag_set_stored_cause(&record.cause);
    }
}

static uint32_t crc(const latch_record_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t value = UINT32_MAX;
    for (size_t i = 0; i < 12; i++) {
        value ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            value = (value >> 1) ^ (UINT32_C(0xedb88320) & (0U - (value & 1U)));
        }
    }
    return ~value;
}

void roundie_boot_persistence_init(bool nvs_available)
{
    opened = nvs_available && nvs_open("boot_guard", NVS_READWRITE, &handle) == ESP_OK;
    if (!opened) {
        roundie_boot_persistence_loaded(false, ROUNDIE_SAFE_PERSISTENCE);
        return;
    }
    load_cause();
    latch_record_t record = {0};
    size_t size = sizeof(record);
    esp_err_t error = nvs_get_blob(handle, "latch", &record, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        saved = ROUNDIE_SAFE_NONE;
        roundie_boot_persistence_loaded(true, ROUNDIE_SAFE_NONE);
    } else if (error == ESP_OK && size == sizeof(record) &&
               record.magic == UINT32_C(0x52424c31) && record.version == 1 &&
               record.reason <= ROUNDIE_SAFE_PERSISTENCE && record.crc == crc(&record)) {
        saved = record.reason;
        roundie_boot_persistence_loaded(true, record.reason);
    } else {
        /* Preserve the malformed record until an explicit recovery operation.
         * An I/O error is unavailable; corrupt data is a recoverable latch. */
        saved = ROUNDIE_SAFE_PERSISTENCE;
        roundie_boot_persistence_loaded(error == ESP_OK || error == ESP_ERR_NVS_INVALID_LENGTH ||
            error == ESP_ERR_NVS_TYPE_MISMATCH, ROUNDIE_SAFE_PERSISTENCE);
    }
}

bool roundie_boot_persistence_sync(void)
{
    roundie_boot_status_t status;
    roundie_boot_status(&status);
    if (!opened || !status.persistence_ok) {
        return false;
    }
    if (status.latch == saved) {
        return true;
    }
    latch_record_t record = {.magic = UINT32_C(0x52424c31), .version = 1, .reason = status.latch};
    record.crc = crc(&record);
    if (nvs_set_blob(handle, "latch", &record, sizeof(record)) != ESP_OK) {
        roundie_boot_persistence_loaded(false, ROUNDIE_SAFE_PERSISTENCE);
        return false;
    }
    if (nvs_commit(handle) != ESP_OK) {
        roundie_boot_persistence_loaded(false, ROUNDIE_SAFE_PERSISTENCE);
        return false;
    }
    saved = record.reason;
    if (status.latch != ROUNDIE_SAFE_NONE) {
        write_cause(status.latch);
    }
    return true;
}

uint32_t roundie_boot_diag_entries(size_t size)
{
    return 2U + (uint32_t)((size + 31U) / 32U);
}

void roundie_boot_diag_nvs_stats(roundie_boot_nvs_stats_t *out)
{
    nvs_stats_t stats;
    memset(out, 0, sizeof(*out));
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        out->valid = true;
        out->used_entries = (uint32_t)stats.used_entries;
        out->available_entries = (uint32_t)stats.available_entries;
        out->total_entries = (uint32_t)stats.total_entries;
    }
}

bool roundie_boot_diag_load(const char *key, void *data, size_t size)
{
    size_t found = size;
    if (!opened || size > ROUNDIE_BOOT_DIAG_BLOB_MAX) {
        return false;
    }
    return nvs_get_blob(handle, key, data, &found) == ESP_OK && found == size;
}

roundie_diag_store_t roundie_boot_diag_store(const char *key, const void *data, size_t size)
{
    if (size > ROUNDIE_BOOT_DIAG_BLOB_MAX) {
        return ROUNDIE_DIAG_TOO_LARGE;
    }
    if (!opened) {
        return ROUNDIE_DIAG_FAILED;
    }
    uint8_t current[ROUNDIE_BOOT_DIAG_BLOB_MAX];
    if (roundie_boot_diag_load(key, current, size) && memcmp(current, data, size) == 0) {
        return ROUNDIE_DIAG_UNCHANGED;
    }
    /* NVS writes the new copy before it erases the old one, so the whole
     * blob must fit beside the old copy and the reserve. */
    roundie_boot_nvs_stats_t stats;
    roundie_boot_diag_nvs_stats(&stats);
    if (!stats.valid || stats.available_entries < roundie_boot_diag_entries(size) + ROUNDIE_BOOT_DIAG_RESERVE_ENTRIES) {
        return ROUNDIE_DIAG_NO_ROOM;
    }
    if (nvs_set_blob(handle, key, data, size) != ESP_OK) {
        return ROUNDIE_DIAG_FAILED;
    }
    return nvs_commit(handle) == ESP_OK ? ROUNDIE_DIAG_STORED : ROUNDIE_DIAG_FAILED;
}

roundie_reset_class_t roundie_boot_reset_class(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return ROUNDIE_RESET_POWER;
    case ESP_RST_SW: return ROUNDIE_RESET_SOFTWARE;
    case ESP_RST_DEEPSLEEP: return ROUNDIE_RESET_SLEEP;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return ROUNDIE_RESET_WATCHDOG;
    case ESP_RST_PANIC:
    case ESP_RST_CPU_LOCKUP: return ROUNDIE_RESET_PANIC;
    case ESP_RST_BROWNOUT:
    case ESP_RST_PWR_GLITCH: return ROUNDIE_RESET_BROWNOUT;
    case ESP_RST_EXT:
    case ESP_RST_USB:
    case ESP_RST_JTAG:
    case ESP_RST_SDIO: return ROUNDIE_RESET_EXTERNAL;
    default: return ROUNDIE_RESET_UNKNOWN;
    }
}
