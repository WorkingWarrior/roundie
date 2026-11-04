#include "roundie_storage_mode.h"
#include <stddef.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define RTC_NOINIT_ATTR
#endif

#define RECORD_MAGIC UINT32_C(0x52534D31)
#define RECORD_VERSION UINT32_C(1)
#define RECORD_REQUEST UINT32_C(0x5354524D)

/* Private, same-image RTC record, like BootGuard's: not a cross-image ABI. */
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t version;
    uint8_t image[32];
    uint32_t request;
    uint32_t check;
} record;

static bool active;

static uint32_t checksum(void)
{
    const uint8_t *bytes = (const uint8_t *)&record;
    uint32_t hash = UINT32_C(2166136261);
    for (size_t index = 0; index < offsetof(typeof(record), check); index++) {
        hash = (hash ^ bytes[index]) * UINT32_C(16777619);
    }
    return hash;
}

roundie_storage_refusal_t roundie_storage_mode_refusal(const roundie_storage_conditions_t *conditions)
{
    if (conditions->storage_mode) {
        return ROUNDIE_STORAGE_REFUSED_ACTIVE;
    }
    if (conditions->safe) {
        return ROUNDIE_STORAGE_REFUSED_SAFE_MODE;
    }
    if (!conditions->stable) {
        return ROUNDIE_STORAGE_REFUSED_NOT_STABLE;
    }
    if (!conditions->ota_idle) {
        return ROUNDIE_STORAGE_REFUSED_OTA;
    }
    if (!conditions->image_valid) {
        return ROUNDIE_STORAGE_REFUSED_UNCONFIRMED;
    }
    if (conditions->app_busy) {
        return ROUNDIE_STORAGE_REFUSED_APP;
    }
    return ROUNDIE_STORAGE_ALLOWED;
}

const char *roundie_storage_refusal_name(roundie_storage_refusal_t refusal)
{
    switch (refusal) {
    case ROUNDIE_STORAGE_ALLOWED:
        return "allowed";
    case ROUNDIE_STORAGE_REFUSED_ACTIVE:
        return "active";
    case ROUNDIE_STORAGE_REFUSED_SAFE_MODE:
        return "safe_mode";
    case ROUNDIE_STORAGE_REFUSED_NOT_STABLE:
        return "not_stable";
    case ROUNDIE_STORAGE_REFUSED_OTA:
        return "ota";
    case ROUNDIE_STORAGE_REFUSED_UNCONFIRMED:
        return "unconfirmed";
    case ROUNDIE_STORAGE_REFUSED_APP:
        return "app";
    }
    return "unknown";
}

void roundie_storage_mode_request(const uint8_t image[32])
{
    record.magic = RECORD_MAGIC;
    record.version = RECORD_VERSION;
    memcpy(record.image, image, sizeof(record.image));
    record.request = RECORD_REQUEST;
    record.check = checksum();
}

bool roundie_storage_mode_begin(bool software_reset, const uint8_t image[32])
{
    bool valid = software_reset && record.magic == RECORD_MAGIC && record.version == RECORD_VERSION &&
        record.request == RECORD_REQUEST && record.check == checksum() &&
        memcmp(record.image, image, sizeof(record.image)) == 0;
    /* Always clear: whatever the reset was, a request never survives a second boot. */
    memset(&record, 0, sizeof(record));
    active = valid;
    return valid;
}

bool roundie_storage_mode_active(void)
{
    return active;
}
