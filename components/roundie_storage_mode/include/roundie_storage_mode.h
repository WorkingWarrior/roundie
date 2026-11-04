#pragma once
#include <stdbool.h>
#include <stdint.h>

/* USB storage mode (docs/usb-storage-mode-plan.md). A one-shot boot mode: a
 * request is kept in RTC memory across one planned software restart, read and
 * cleared before anything else starts, so a crash, watchdog reset or power loss
 * always returns to normal mode. This file holds only that flag and the
 * admission rule; no IDF calls, no locks. */

typedef enum {
    ROUNDIE_STORAGE_ALLOWED,
    ROUNDIE_STORAGE_REFUSED_ACTIVE,      /* already in storage mode */
    ROUNDIE_STORAGE_REFUSED_SAFE_MODE,
    ROUNDIE_STORAGE_REFUSED_NOT_STABLE,
    ROUNDIE_STORAGE_REFUSED_OTA,         /* an OTA session is open or activation is pending */
    ROUNDIE_STORAGE_REFUSED_UNCONFIRMED, /* the running image is not VALID */
    ROUNDIE_STORAGE_REFUSED_APP,         /* an application or package transaction is busy */
} roundie_storage_refusal_t;

typedef struct {
    bool storage_mode;
    bool safe;
    bool stable;
    bool ota_idle;
    bool image_valid;
    bool app_busy;
} roundie_storage_conditions_t;

roundie_storage_refusal_t roundie_storage_mode_refusal(const roundie_storage_conditions_t *conditions);
const char *roundie_storage_refusal_name(roundie_storage_refusal_t refusal);

/* Stores a one-shot request for this firmware image. The caller then performs a
 * planned restart. */
void roundie_storage_mode_request(const uint8_t image[32]);

/* Early boot, once. Returns true when a valid request for this image survived
 * exactly one software restart. The record is always cleared. */
bool roundie_storage_mode_begin(bool software_reset, const uint8_t image[32]);

bool roundie_storage_mode_active(void);
