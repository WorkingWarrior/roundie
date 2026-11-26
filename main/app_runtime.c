#include "app_runtime.h"
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_APP_RUNTIME
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "roundie_boot_guard.h"
#include "roundie_elf.h"
#include "roundie_ota.h"
#include "esp_log.h"
#include "rep/rep_storage_v1.h"
#include "roundie_packages.h"
#include "roundie_storage_mode.h"
#include "roundie_watchface.h"
#include "storage_diagnostics.h"
#include "roundie_storage_io.h"
#if CONFIG_ROUNDIE_DEV_ELF
#include "elf_diagnostics.h"
#endif

_Static_assert(ROUNDIE_BOOT_APP_ID == ROUNDIE_PACKAGE_ID, "BootGuard stores a whole package ID");

/* Package whose application storage the loaded or last instance holds. */
static char storage_owner[ROUNDIE_PACKAGE_ID];
static uint8_t storage_buffer[REP_STORAGE_CAPACITY];

static bool staging_busy(void)
{
#if CONFIG_ROUNDIE_DEV_ELF
    return roundie_elf_transfer_busy();
#else
    return false;
#endif
}

/* Everything that lets an application or a package transaction run, except the mode. */
static bool admission_conditions(void)
{
    roundie_boot_status_t boot;
    roundie_boot_status(&boot);
    roundie_ota_status_t ota;
    roundie_ota_status(&ota);
    return !boot.safe && boot.stage == ROUNDIE_BOOT_STABLE &&
        ota.initialized && ota.boot_checked && ota.boot_passed &&
        ota.running.state == ESP_OTA_IMG_VALID && ota.phase == ROUNDIE_OTA_IDLE &&
        strcmp(ota.running.label, ota.boot) == 0 && roundie_storage_available() &&
        !roundie_storage_claim_ota_active();
}

/* No application starts in USB storage mode; packages are still installed there. */
bool roundie_app_runtime_allowed(void)
{
    return !roundie_storage_mode_active() && admission_conditions();
}

/* A package transaction would be admitted now. True in USB storage mode once the boot is stable. */
bool roundie_app_runtime_packages_ready(void)
{
    return admission_conditions() && !roundie_elf_busy() && !staging_busy() && !roundie_packages_busy();
}

bool roundie_app_runtime_busy(void)
{
    return staging_busy() || roundie_elf_busy() || roundie_packages_busy();
}

bool roundie_app_runtime_upload_busy(void)
{
    return staging_busy() || roundie_packages_busy();
}

bool roundie_app_runtime_admit(void)
{
    bool permitted = roundie_app_runtime_allowed() && !roundie_packages_busy();
    roundie_elf_set_allowed(permitted);
    return permitted;
}

static void reconcile(int64_t now_us)
{
#if CONFIG_ROUNDIE_DEV_ELF
    roundie_elf_poll(now_us);
#else
    (void)now_us;
    roundie_app_runtime_admit();
#endif
}

/* Flash write of the blob an unloaded instance wrote. While a package
 * transaction, an upload or a policy change blocks it, the blob stays pending;
 * no package launch is admitted until it is saved. */
static void persist_storage(void)
{
    size_t length;
    if (storage_owner[0] == '\0' || !roundie_app_runtime_allowed() || roundie_app_runtime_busy() ||
        !roundie_elf_storage_take(storage_buffer, &length)) {
        return;
    }
    esp_err_t error = roundie_packages_data_write(storage_owner, storage_buffer, length);
    if (error != ESP_OK) {
        roundie_packages_status_t packages;
        roundie_packages_status(&packages);
        ESP_LOGW("app_storage", "%s: %zu bytes not saved: error %d (%s)", storage_owner, length,
                 (int)error, packages.error);
    }
    storage_owner[0] = '\0';
}

void roundie_app_runtime_poll(int64_t now_us)
{
    reconcile(now_us);
    roundie_packages_poll(now_us);
    roundie_watchface_poll(now_us);
    if (!roundie_elf_busy()) {
        roundie_boot_app_loaded("");
        persist_storage();
    }
}

bool roundie_packages_allowed(void)
{
    return admission_conditions();
}

bool roundie_packages_runtime_busy(void)
{
    return roundie_elf_busy() || staging_busy();
}

bool roundie_packages_capacity(size_t *free_bytes)
{
    size_t total;
    size_t used;
    if (!roundie_storage_usage(&total, &used)) {
        return false;
    }
    *free_bytes = total - used;
    return true;
}

int64_t roundie_packages_clock(void)
{
    return esp_timer_get_time();
}

void *roundie_packages_allocate(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void roundie_packages_free(void *memory)
{
    heap_caps_free(memory);
}

/* The watch face store shares the package store's admission, capacity and
 * PSRAM buffers (docs/watch-face-platform.md). */
static const roundie_watchface_store_platform_t face_store = {
    roundie_packages_allowed, roundie_packages_capacity, roundie_packages_allocate, roundie_packages_free,
    roundie_packages_clock,
};

void roundie_app_runtime_faces_init(bool fallback_only)
{
    roundie_watchface_init(&face_store, fallback_only);
}

int roundie_packages_execute(const char *id, uint8_t *bytes, size_t size, uint16_t abi)
{
    /* Reopen admission after a package transaction has retired. Runtime still
     * checks its own policy/reservation before accepting the input. */
    reconcile(esp_timer_get_time());
    persist_storage(); /* the previous instance's blob, before reading this one */
    if (abi == 3) {
        size_t length = 0;
        esp_err_t read = roundie_packages_data_read(id, storage_buffer, sizeof(storage_buffer), &length);
        if (read != ESP_OK) {
            ESP_LOGW("app_storage", "%s: stored data unreadable (error %d); starting empty", id, (int)read);
        }
        if (!roundie_elf_storage_set(storage_buffer, length)) {
            return ESP_ERR_INVALID_STATE; /* an earlier blob is still unsaved */
        }
        memcpy(storage_owner, id, strlen(id) + 1);
    }
    roundie_boot_app_loaded(id);
    esp_err_t error = abi == 1 ? roundie_elf_submit(bytes, size) : roundie_elf_start(bytes, size);
    if (error != ESP_OK) {
        roundie_elf_storage_clear();
        roundie_boot_app_loaded("");
    }
    return error;
}
#endif
