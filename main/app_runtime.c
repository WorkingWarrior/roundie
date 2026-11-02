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
#include "roundie_packages.h"
#include "storage_diagnostics.h"
#if CONFIG_ROUNDIE_DEV_ELF
#include "elf_diagnostics.h"
#endif

static bool staging_busy(void)
{
#if CONFIG_ROUNDIE_DEV_ELF
    return roundie_elf_transfer_busy();
#else
    return false;
#endif
}

bool roundie_app_runtime_allowed(void)
{
    roundie_boot_status_t boot;
    roundie_boot_status(&boot);
    roundie_ota_status_t ota;
    roundie_ota_status(&ota);
    return !boot.safe && boot.stage == ROUNDIE_BOOT_STABLE &&
        ota.initialized && ota.boot_checked && ota.boot_passed &&
        ota.running.state == ESP_OTA_IMG_VALID && ota.phase == ROUNDIE_OTA_IDLE &&
        strcmp(ota.running.label, ota.boot) == 0 && roundie_storage_available();
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

void roundie_app_runtime_poll(int64_t now_us)
{
    reconcile(now_us);
    roundie_packages_poll(now_us);
}

bool roundie_packages_allowed(void)
{
    return roundie_app_runtime_allowed();
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

int roundie_packages_execute(uint8_t *bytes, size_t size, uint16_t abi)
{
    /* Reopen admission after a package transaction has retired. Runtime still
     * checks its own policy/reservation before accepting the input. */
    reconcile(esp_timer_get_time());
    return abi == 1 ? roundie_elf_submit(bytes, size) : roundie_elf_start(bytes, size);
}
#endif
