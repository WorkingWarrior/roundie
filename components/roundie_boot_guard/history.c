#include "roundie_boot_guard.h"

#include <stddef.h>
#include <string.h>
#include "esp_system.h"
#include "nvs.h"

#define HISTORY_MAGIC UINT32_C(0x52424831) /* "RBH1" */
#define HISTORY_VERSION 1U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t boots;
    uint32_t brownouts;
    uint32_t head; /* index of the newest entry */
    roundie_boot_history_entry_t entries[ROUNDIE_BOOT_HISTORY_ENTRIES];
    uint32_t crc;
} history_record_t;

static nvs_handle_t handle;
static roundie_history_state_t state = ROUNDIE_HISTORY_UNAVAILABLE;
static history_record_t record;
static bool battery_done;

static uint32_t crc(const history_record_t *value)
{
    const uint8_t *bytes = (const uint8_t *)value;
    uint32_t result = UINT32_MAX;
    for (size_t i = 0; i < offsetof(history_record_t, crc); i++) {
        result ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            result = (result >> 1) ^ (UINT32_C(0xedb88320) & (0U - (result & 1U)));
        }
    }
    return ~result;
}

static bool valid(const history_record_t *value)
{
    return value->magic == HISTORY_MAGIC && value->version == HISTORY_VERSION &&
        value->head < ROUNDIE_BOOT_HISTORY_ENTRIES && value->crc == crc(value);
}

static bool store(void)
{
    record.crc = crc(&record);
    if (nvs_set_blob(handle, "history", &record, sizeof(record)) != ESP_OK || nvs_commit(handle) != ESP_OK) {
        state = ROUNDIE_HISTORY_UNAVAILABLE;
        return false;
    }
    return true;
}

void roundie_boot_history_record(bool nvs_available)
{
    state = ROUNDIE_HISTORY_UNAVAILABLE;
    if (!nvs_available || nvs_open("boot_history", NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    history_record_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    size_t size = sizeof(loaded);
    esp_err_t error = nvs_get_blob(handle, "history", &loaded, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        memset(&record, 0, sizeof(record));
        record.magic = HISTORY_MAGIC;
        record.version = HISTORY_VERSION;
        record.head = ROUNDIE_BOOT_HISTORY_ENTRIES - 1U;
    } else if (error == ESP_OK && size == sizeof(loaded) && valid(&loaded)) {
        record = loaded;
    } else if (error == ESP_OK || error == ESP_ERR_NVS_INVALID_LENGTH || error == ESP_ERR_NVS_TYPE_MISMATCH) {
        /* Evidence stays as found; an operator decides what to do with it. */
        state = ROUNDIE_HISTORY_MALFORMED;
        return;
    } else {
        return;
    }
    esp_reset_reason_t reason = esp_reset_reason();
    if (record.boots != UINT32_MAX) {
        record.boots++;
    }
    if (reason == ESP_RST_BROWNOUT && record.brownouts != UINT32_MAX) {
        record.brownouts++;
    }
    record.head = (record.head + 1U) % ROUNDIE_BOOT_HISTORY_ENTRIES;
    record.entries[record.head].boot = record.boots;
    record.entries[record.head].raw_reset = (uint16_t)reason;
    record.entries[record.head].vbat_mv = 0;
    battery_done = false;
    state = ROUNDIE_HISTORY_OK;
    store();
}

void roundie_boot_history_battery(uint16_t voltage_mv)
{
    if (state != ROUNDIE_HISTORY_OK || battery_done || voltage_mv == 0) {
        return;
    }
    battery_done = true;
    /* One aligned 16-bit store; a concurrent RS1 HISTORY sees old or new. */
    record.entries[record.head].vbat_mv = voltage_mv;
    store();
}

void roundie_boot_history_get(roundie_boot_history_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = state;
    if (state != ROUNDIE_HISTORY_OK) {
        return;
    }
    out->boots = record.boots;
    out->brownouts = record.brownouts;
    for (uint32_t i = 0; i < ROUNDIE_BOOT_HISTORY_ENTRIES; i++) {
        uint32_t index = (record.head + ROUNDIE_BOOT_HISTORY_ENTRIES - i) % ROUNDIE_BOOT_HISTORY_ENTRIES;
        if (record.entries[index].boot == 0) {
            break;
        }
        out->entries[out->count++] = record.entries[index];
    }
}
