#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Internal system interface, not SDK/ELF ABI. Initialize before starting the
 * worker. Thereafter ONE worker owns all calls, including status and boot
 * confirmation. Publish copied snapshots to GUI; never call from GUI callbacks.
 * No background jobs retain caller buffers. No implicit reboot on activation.
 */
typedef enum {
    ROUNDIE_OTA_IDLE,
    ROUNDIE_OTA_RECEIVING,
    ROUNDIE_OTA_VERIFYING,
    ROUNDIE_OTA_READY,
    ROUNDIE_OTA_ACTIVATION_PENDING,
} roundie_ota_phase_t;

typedef struct {
    char label[17];
    char version[33];
    char elf[65];
    uint32_t address;
    uint32_t size;
    uint32_t secure_version;
    uint32_t image_size;
    int state; /* ESP_OTA_IMG_* (-1 = UNDEFINED); -2 = no readable metadata. */
    bool image_valid;
    bool compatible;
} roundie_ota_slot_t;

typedef struct {
    roundie_ota_phase_t phase;
    roundie_ota_slot_t running;
    roundie_ota_slot_t other;
    char boot[17];
    char digest[65];
    size_t received;
    size_t total;
    uint32_t session;
    esp_err_t last_error;
    bool initialized;
    bool boot_checked;
    bool boot_passed;
    bool rollback_possible;
} roundie_ota_status_t;

esp_err_t roundie_ota_init(void);
void roundie_ota_status(roundie_ota_status_t *out);
/* Complete short core self-test. Only PENDING_VERIFY is confirmed/rejected.
 * Failure without fallback stays unconfirmed, blocks OTA, and does not reboot.
 */
esp_err_t roundie_ota_boot_result(bool passed);
esp_err_t roundie_ota_begin(uint32_t session, size_t size, const uint8_t digest[32]);
esp_err_t roundie_ota_write(uint32_t session, size_t offset, const void *data, size_t size);
esp_err_t roundie_ota_finish(uint32_t session);
esp_err_t roundie_ota_abort(uint32_t session);
esp_err_t roundie_ota_activate(const uint8_t digest[32]);
/* Recovery owner only: explicitly select the other compatible, verified VALID
 * image by exact label and ELF identity. IDF changes it to NEW; first-boot
 * confirmation is required again. Does not invalidate the running image. */
esp_err_t roundie_ota_select_valid(const char *label, const char *elf);
/* Internal approved-control path only; NOT exposed on USB. Marks current image
 * invalid and reboots only with a compatible VALID destination and IDF eligibility.
 */
esp_err_t roundie_ota_rollback(void);
/* Controlled automatic-rollback experiment: pending + verified VALID fallback
 * required. Caller must be an explicitly armed development build. No confirmation.
 */
bool roundie_ota_test_reset_allowed(void);
/* Armed-test refusal only: record failure and block new OTA without changing
 * metadata, confirming, rejecting or rebooting. Not exposed through USB. */
esp_err_t roundie_ota_test_hold_unconfirmed(esp_err_t reason);
void roundie_ota_tick(int64_t now_us);
const char *roundie_ota_phase_name(roundie_ota_phase_t phase);
const char *roundie_ota_image_state_name(int state);
