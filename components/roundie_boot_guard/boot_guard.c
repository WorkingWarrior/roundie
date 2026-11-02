#include "roundie_boot_guard.h"

#include <stddef.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&lock)
#define UNLOCK() portEXIT_CRITICAL(&lock)
#else
#define RTC_NOINIT_ATTR
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif

/* Private, versioned, same-image RTC record. Deliberately not a cross-image ABI
 * or the bootloader's reserved RTC area. No bootloader changes are required. */
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t version;
    uint8_t image[32];
    uint32_t failures;
    uint32_t stage;
    uint32_t safe;
    uint32_t reason;
    uint32_t planned;
    uint32_t trial;
    uint32_t fault_remaining;
    uint32_t skip_gui;
    char app[ROUNDIE_BOOT_APP_ID];
    uint32_t crc;
} retained;

static roundie_boot_status_t current;
static int64_t operational_since;

static uint32_t checksum(void)
{
    const uint8_t *bytes = (const uint8_t *)&retained;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < offsetof(typeof(retained), crc); i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

static void seal(void)
{
    retained.failures = current.failures;
    retained.stage = current.stage;
    retained.safe = current.safe;
    retained.reason = current.reason;
    retained.skip_gui = current.skip_gui;
    retained.fault_remaining = current.fault_remaining;
    retained.crc = checksum();
}

static void enter_safe(roundie_safe_reason_t reason)
{
    if (!current.safe) {
        current.reason = reason;
    }
    current.safe = true;
    current.latch = current.reason;
    current.fault_remaining = 0;
    retained.trial = 0;
    if (reason == ROUNDIE_SAFE_GUI) {
        current.skip_gui = true;
    }
}

void roundie_boot_begin(roundie_reset_class_t reset, const uint8_t image[32])
{
    bool retained_reset = reset != ROUNDIE_RESET_POWER && reset != ROUNDIE_RESET_BROWNOUT;
    bool valid = retained_reset && retained.magic == UINT32_C(0x52424731) &&
        retained.version == 2 && retained.crc == checksum() &&
        memcmp(retained.image, image, 32) == 0 &&
        retained.stage <= ROUNDIE_BOOT_STABLE && retained.reason <= ROUNDIE_SAFE_PERSISTENCE &&
        retained.failures <= ROUNDIE_BOOT_FAILURE_LIMIT && retained.safe <= 1 &&
        retained.planned <= 1 && retained.trial <= 2 && retained.skip_gui <= 1 &&
        retained.fault_remaining <= ROUNDIE_BOOT_FAILURE_LIMIT &&
        memchr(retained.app, '\0', sizeof(retained.app)) != NULL;
    current = (roundie_boot_status_t){.retained_valid = valid, .reset = reset};
    operational_since = -1;
    if (valid) {
        current.failures = retained.failures;
        current.previous_stage = retained.stage;
        current.safe = retained.safe;
        current.reason = retained.reason;
        current.skip_gui = retained.skip_gui;
        current.fault_remaining = retained.fault_remaining;
        bool abnormal = reset == ROUNDIE_RESET_PANIC || reset == ROUNDIE_RESET_WATCHDOG ||
            (reset == ROUNDIE_RESET_SOFTWARE && !retained.planned);
        if (abnormal) {
            memcpy(current.fault_app, retained.app, sizeof(current.fault_app));
        }
        if (abnormal && current.safe) {
            /* Even a late fault in recovery must not retry optional GUI. */
            current.skip_gui = true;
        } else if (abnormal && retained.stage == ROUNDIE_BOOT_STABLE) {
            /* This is a core runtime failure, not an incomplete startup.
             * Recover conservatively on the next boot instead of allowing
             * recurring late failures to evade the startup counter forever.
             * This never asserts that the firmware image is defective. */
            enter_safe(ROUNDIE_SAFE_CORE);
        } else if (abnormal && retained.stage != ROUNDIE_BOOT_STABLE) {
            if (current.failures < ROUNDIE_BOOT_FAILURE_LIMIT) {
                current.failures++;
            }
            if (current.failures >= ROUNDIE_BOOT_FAILURE_LIMIT) {
                enter_safe(ROUNDIE_SAFE_BOOT_LOOP);
                current.skip_gui = retained.stage == ROUNDIE_BOOT_GUI;
            }
        }
        /* A failed one-shot trial returns directly to recovery, even below
         * the ordinary threshold; unrelated restarts cannot clear the latch. */
        if (retained.trial == 2) {
            enter_safe(ROUNDIE_SAFE_BOOT_LOOP);
        }
        current.trial = retained.trial == 1 && retained.planned && reset == ROUNDIE_RESET_SOFTWARE;
    } else {
        memset(&retained, 0, sizeof(retained));
        retained.magic = UINT32_C(0x52424731);
        retained.version = 2;
        memcpy(retained.image, image, 32);
    }
    memset(retained.app, 0, sizeof(retained.app));
    /* 2 means the one-shot token was consumed by this boot. */
    retained.trial = current.trial ? 2 : 0;
    retained.planned = 0;
    current.stage = ROUNDIE_BOOT_START;
    seal();
}

void roundie_boot_persistence_loaded(bool available, roundie_safe_reason_t latch)
{
    LOCK();
    current.persistence_ok = available;
    if (!available) {
        enter_safe(ROUNDIE_SAFE_PERSISTENCE);
    } else if (latch != ROUNDIE_SAFE_NONE && !current.trial) {
        enter_safe(latch);
    }
    if (current.latch == ROUNDIE_SAFE_NONE) {
        current.latch = latch;
    }
    seal();
    UNLOCK();
}

void roundie_boot_status(roundie_boot_status_t *out)
{
    LOCK();
    *out = current;
    UNLOCK();
}

void roundie_boot_enter_safe(roundie_safe_reason_t reason)
{
    LOCK();
    if (!current.safe || (reason == ROUNDIE_SAFE_GUI && !current.skip_gui)) {
        enter_safe(reason);
        seal();
    }
    UNLOCK();
}

void roundie_boot_stage(roundie_boot_stage_t stage)
{
    LOCK();
    current.stage = stage;
    seal();
    UNLOCK();
}

void roundie_boot_operational(int64_t now_us, bool core_passed)
{
    LOCK();
    if (core_passed && operational_since < 0) {
        current.stage = ROUNDIE_BOOT_OPERATIONAL;
        operational_since = now_us;
        seal();
    }
    UNLOCK();
}

void roundie_boot_tick(int64_t now_us)
{
    LOCK();
    if (operational_since >= 0 && current.stage == ROUNDIE_BOOT_OPERATIONAL &&
        now_us - operational_since >= ROUNDIE_BOOT_STABILITY_US) {
        current.stage = ROUNDIE_BOOT_STABLE;
        if (!current.safe) {
            current.failures = 0;
            current.reason = ROUNDIE_SAFE_NONE;
            current.latch = ROUNDIE_SAFE_NONE;
            current.trial = false;
            retained.trial = 0;
        }
        seal();
    }
    UNLOCK();
}

void roundie_boot_app_loaded(const char *id)
{
    LOCK();
    if (strncmp(retained.app, id, sizeof(retained.app) - 1) != 0) {
        memset(retained.app, 0, sizeof(retained.app));
        strncpy(retained.app, id, sizeof(retained.app) - 1);
        seal();
    }
    UNLOCK();
}

void roundie_boot_planned_restart(void)
{
    LOCK();
    retained.planned = 1;
    retained.crc = checksum();
    UNLOCK();
}

bool roundie_boot_clear(void)
{
    LOCK();
    bool allowed = current.safe && current.persistence_ok;
    if (allowed) {
        current.latch = ROUNDIE_SAFE_NONE;
        current.failures = 0;
        current.fault_remaining = 0;
        retained.trial = 0;
        seal();
    }
    UNLOCK();
    return allowed;
}

bool roundie_boot_normal_trial(void)
{
    LOCK();
    bool allowed = current.safe && current.persistence_ok;
    if (allowed) {
        /* Preserve the latch until stable. Only the immediate SW restart can
         * consume the token; power loss or a panic goes back to Safe Mode. */
        current.latch = current.reason;
        retained.safe = 0;
        retained.trial = 1;
        retained.planned = 1;
        retained.crc = checksum();
    }
    UNLOCK();
    return allowed;
}

bool roundie_boot_fault_arm(void)
{
    bool allowed = false;
#if CONFIG_ROUNDIE_BOOT_GUARD_TEST
    LOCK();
    allowed = !current.safe && current.stage == ROUNDIE_BOOT_STABLE && current.persistence_ok;
    if (allowed) {
        current.fault_remaining = ROUNDIE_BOOT_FAILURE_LIMIT;
        seal();
    }
    UNLOCK();
#endif
    return allowed;
}

bool roundie_boot_fault_step(bool confirmed_valid)
{
    bool reset = false;
#if CONFIG_ROUNDIE_BOOT_GUARD_TEST
    LOCK();
    reset = confirmed_valid && !current.safe && current.fault_remaining > 0;
    if (reset) {
        current.fault_remaining--;
        current.stage = ROUNDIE_BOOT_CORE;
        retained.planned = 0;
        seal();
    }
    UNLOCK();
#else
    (void)confirmed_valid;
#endif
    return reset;
}

const char *roundie_boot_reason_name(uint32_t reason)
{
    static const char *names[] = {"none", "request", "boot_loop", "storage", "gui", "core", "persistence"};
    return reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "unknown";
}
