/* Application runtime of the Linux desktop (docs/native-apps.md, N4-N7).
 *
 * The shared manager (components/roundie_elf/manager.c) runs linux-x64.rep1
 * applications through the desktop loader and executor
 * (components/roundie_elf/linux). Native applications are trusted developer
 * code in the desktop process and run only when the desktop was started with
 * --native-apps; otherwise every launch is refused as before. This file
 * implements the package manager's integration seams
 * (components/roundie_packages/platform.h) and the application storage flow of
 * main/app_runtime.c. */

#include "app_runtime.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include "esp_log.h"
#include "platform.h"
#include "rep/rep_storage_v1.h"
#include "roundie_elf.h"
#include "roundie_packages.h"
#include "roundie_platform.h"

/* An executor call running this long is reported as an application that does not return. */
#define STALL_REPORT_US (5 * 1000 * 1000)

static atomic_bool native_apps;
static _Atomic(roundie_elf_clock_source_t) clock_source;

/* Worker only: package whose application storage the loaded or last instance holds. */
static char storage_owner[ROUNDIE_PACKAGE_ID];
static uint8_t storage_buffer[REP_STORAGE_CAPACITY];
static int64_t stall_reported;

void roundie_desktop_apps_enable(bool enabled)
{
    atomic_store(&native_apps, enabled);
    if (enabled) {
        ESP_LOGW("elf", "native applications enabled: trusted code in this process, validated but not isolated");
    }
}

bool roundie_desktop_apps_enabled(void)
{
    return atomic_load(&native_apps);
}

/* Flash write of the blob an unloaded instance wrote, as on the board. */
static void persist_storage(void)
{
    size_t length;
    if (storage_owner[0] == '\0' || roundie_elf_busy() || roundie_packages_busy() ||
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

void roundie_desktop_apps_poll(int64_t now_us)
{
    roundie_elf_set_allowed(atomic_load(&native_apps) && !roundie_packages_busy());
    if (!roundie_elf_busy()) {
        persist_storage();
    }
    int64_t since = roundie_elf_executor_busy_since();
    if (since != 0 && now_us - since >= STALL_REPORT_US && since != stall_reported) {
        stall_reported = since;
        roundie_elf_status_t status;
        roundie_elf_status(&status);
        ESP_LOGE("elf", "instance %" PRIu32 " has not returned for %" PRId64 " ms; native code cannot be "
                 "stopped safely, so the desktop keeps running without it and refuses new launches. "
                 "Restart the desktop to reclaim it.", status.instance, (now_us - since) / 1000);
    }
}

bool roundie_packages_allowed(void)
{
    return true;
}

bool roundie_packages_runtime_busy(void)
{
    return roundie_elf_busy();
}

bool roundie_packages_capacity(size_t *free_bytes)
{
    struct statvfs info;
    if (statvfs(".", &info) != 0) {
        return false;
    }
    *free_bytes = (size_t)info.f_bavail * (size_t)info.f_frsize;
    return true;
}

int64_t roundie_packages_clock(void)
{
    return roundie_time_us();
}

void *roundie_packages_allocate(size_t size)
{
    return malloc(size);
}

void roundie_packages_free(void *memory)
{
    free(memory);
}

int roundie_packages_execute(const char *id, uint8_t *bytes, size_t size, uint16_t abi)
{
    if (!atomic_load(&native_apps)) {
        ESP_LOGW("elf", "%s: native applications are disabled; start the desktop with --native-apps", id);
        return ESP_ERR_NOT_SUPPORTED;
    }
    roundie_elf_set_allowed(!roundie_packages_busy());
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
    esp_err_t error = abi == 1 ? roundie_elf_submit(bytes, size) : roundie_elf_start(bytes, size);
    if (error != ESP_OK) {
        roundie_elf_storage_clear();
    }
    return error;
}

/* Kept for the script's clock command: the source applications get. */
void roundie_desktop_set_app_clock_source(roundie_elf_clock_source_t source)
{
    atomic_store(&clock_source, source);
    roundie_elf_set_clock_source(source);
}

roundie_elf_clock_source_t roundie_desktop_app_clock_source(void)
{
    return atomic_load(&clock_source);
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:
        return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:
        return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    default:
        return "UNKNOWN ERROR";
    }
}
