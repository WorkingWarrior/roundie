/* Application runtime of the Linux desktop, milestone M1.
 *
 * The desktop has no native application executor yet (docs/roundie-core.md,
 * D6): ESP32 packages are listed by the real package manager, and launching
 * one reports ESP_ERR_NOT_SUPPORTED through the launcher's normal error path.
 * This file implements the package manager's integration seams
 * (components/roundie_packages/platform.h) and the runtime status the
 * launcher observes (roundie_elf.h). */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include "platform.h"
#include "roundie_elf.h"
#include "roundie_platform.h"

static const char *const unsupported = "native apps not supported on desktop";

static roundie_lock_t lock = ROUNDIE_LOCK_INITIALIZER;
static uint32_t refusals;

bool roundie_packages_allowed(void)
{
    return true;
}

bool roundie_packages_runtime_busy(void)
{
    return false;
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
    (void)id;
    (void)bytes;
    (void)size;
    (void)abi;
    roundie_lock(&lock);
    refusals++;
    roundie_unlock(&lock);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t roundie_elf_submit(uint8_t *bytes, size_t size)
{
    (void)bytes;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t roundie_elf_start(uint8_t *bytes, size_t size)
{
    (void)bytes;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t roundie_elf_control(uint32_t instance, roundie_elf_control_t operation)
{
    (void)instance;
    (void)operation;
    return ESP_ERR_INVALID_STATE; /* nothing is ever loaded */
}

esp_err_t roundie_elf_post(uint32_t instance, uint32_t value)
{
    (void)instance;
    (void)value;
    return ESP_ERR_INVALID_STATE;
}

bool roundie_elf_busy(void)
{
    return false;
}

bool roundie_elf_storage_set(const void *data, size_t length)
{
    (void)data;
    (void)length;
    return true;
}

void roundie_elf_storage_clear(void)
{
}

bool roundie_elf_storage_take(void *data, size_t *length)
{
    (void)data;
    (void)length;
    return false;
}

void roundie_elf_status(roundie_elf_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = ROUNDIE_ELF_UNLOADED;
    roundie_lock(&lock);
    if (refusals > 0) {
        out->loader_result = ESP_ERR_NOT_SUPPORTED;
        strncpy(out->error, unsupported, sizeof(out->error) - 1);
    }
    roundie_unlock(&lock);
}

void roundie_elf_set_allowed(bool allowed)
{
    (void)allowed;
}

/* Kept for the script's clock command: the source applications would get. */
static _Atomic(roundie_elf_clock_source_t) clock_source;

void roundie_elf_set_clock_source(roundie_elf_clock_source_t source)
{
    atomic_store(&clock_source, source);
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
