#include "roundie_boot_guard.h"

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
    if (nvs_set_blob(handle, "latch", &record, sizeof(record)) != ESP_OK || nvs_commit(handle) != ESP_OK) {
        roundie_boot_persistence_loaded(false, ROUNDIE_SAFE_PERSISTENCE);
        return false;
    }
    saved = record.reason;
    return true;
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
