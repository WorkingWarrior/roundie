#pragma once
#include "esp_err.h"
#include "roundie_package_format.h"

#define ROUNDIE_PACKAGES_MAX 8U
#define ROUNDIE_PACKAGES_RESERVE (256U * 1024U)
typedef enum {
    ROUNDIE_PACKAGES_IDLE, ROUNDIE_PACKAGES_RECEIVING, ROUNDIE_PACKAGES_STAGED,
    ROUNDIE_PACKAGES_VALIDATING, ROUNDIE_PACKAGES_READY, ROUNDIE_PACKAGES_COMMITTING,
} roundie_packages_phase_t;
typedef enum {
    ROUNDIE_PACKAGE_OK, ROUNDIE_PACKAGE_CORRUPT, ROUNDIE_PACKAGE_INCOMPATIBLE,
} roundie_package_health_t;
typedef struct {
    roundie_package_metadata_t metadata;
    roundie_package_health_t health;
    uint8_t digest[32];
    unsigned slot;
} roundie_package_record_t;
typedef struct {
    roundie_packages_phase_t phase;
    uint32_t session;
    size_t received;
    size_t total;
    unsigned count;
    unsigned orphans;
    uint32_t revision; /* Successful cache publication; internal, not persisted. */
    uint32_t upload_us;
    uint32_t validation_us;
    uint32_t commit_us;
    uint32_t enumeration_us;
    uint32_t lookup_us;
    uint32_t launch_us;
    uint32_t removal_us;
    char error[ROUNDIE_ELF_ERROR_SIZE];
} roundie_packages_status_t;

/* Internal, sole system-worker API. No boot prerequisite, threads, callbacks,
 * runtime pointers or filesystem paths in registry records. No autostart. */
bool roundie_packages_busy(void);
void roundie_packages_poll(int64_t now_us);
void roundie_packages_status(roundie_packages_status_t *out);
esp_err_t roundie_packages_begin(uint32_t session, size_t size, const uint8_t digest[32]);
esp_err_t roundie_packages_write(uint32_t session, size_t offset, const void *data, size_t size);
esp_err_t roundie_packages_finish(uint32_t session);
esp_err_t roundie_packages_abort(uint32_t session);
esp_err_t roundie_packages_install(const uint8_t digest[32]);
esp_err_t roundie_packages_scan(void);
esp_err_t roundie_packages_get(unsigned index, roundie_package_record_t *out);
esp_err_t roundie_packages_find(const char *id, roundie_package_record_t *out);
esp_err_t roundie_packages_launch(const char *id);
esp_err_t roundie_packages_remove(const char *id);
/* App storage (docs/app-storage.md): "data" in the package directory, removed
 * with the package. read gives length 0 for no file; a corrupt file is
 * ESP_ERR_INVALID_CRC with length 0 and is left in place. write replaces the
 * file atomically (length 0 removes it); only for an installed package, with
 * no application loaded. */
esp_err_t roundie_packages_data_read(const char *id, void *buffer, size_t capacity, size_t *length);
esp_err_t roundie_packages_data_write(const char *id, const void *data, size_t length);
