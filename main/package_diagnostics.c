#include "package_diagnostics.h"
#if CONFIG_ROUNDIE_DEV_PACKAGES
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "roundie_packages.h"
#include "roundie_elf.h"
#include "roundie_ota_protocol.h"
#include "elf_diagnostics.h"
#include "storage_diagnostics.h"

bool roundie_packages_allowed(void)
{
    return roundie_elf_policy_allowed();
}

bool roundie_packages_runtime_busy(void)
{
    return roundie_elf_busy() || roundie_elf_transfer_busy();
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
    /* Reopen RE1 admission after a package transaction has retired. Runtime
     * still checks its own policy/reservation before accepting the input. */
    roundie_elf_poll(esp_timer_get_time());
    return abi == 1 ? roundie_elf_submit(bytes, size) : roundie_elf_start(bytes, size);
}

void roundie_package_poll(int64_t now_us)
{
    roundie_packages_poll(now_us);
}

static esp_err_t transfer_command(const roundie_ota_command_t *command)
{
    switch (command->operation) {
    case ROUNDIE_OTA_STATUS: return ESP_OK;
    case ROUNDIE_OTA_BEGIN: return roundie_packages_begin(command->session, command->size, command->digest);
    case ROUNDIE_OTA_DATA: return roundie_packages_write(command->session, command->offset, command->data, command->size);
    case ROUNDIE_OTA_END: return roundie_packages_finish(command->session);
    case ROUNDIE_OTA_ABORT: return roundie_packages_abort(command->session);
    case ROUNDIE_OTA_ACTIVATE: return roundie_packages_install(command->digest);
    default: return ESP_ERR_NOT_SUPPORTED;
    }
}

static void hex_digest(char out[65], const uint8_t digest[32])
{
    for (unsigned i = 0; i < 32; i++) {
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    }
}

static void json_text(char out[ROUNDIE_PACKAGE_NAME * 2 + 1], const char *name)
{
    size_t used = 0;
    for (size_t i = 0; name[i] != '\0' && i < ROUNDIE_PACKAGE_NAME - 1; i++) {
        if (name[i] == '"' || name[i] == '\\') {
            out[used++] = '\\';
        }
        out[used++] = name[i];
    }
    out[used] = '\0';
}

void roundie_package_command(char *line, bool discard)
{
    roundie_package_poll(esp_timer_get_time());
    roundie_ota_command_t command;
    char operation[96] = {0};
    size_t length = strlen(line);
    if (length >= 14 && length - 14 < sizeof(operation)) {
        memcpy(operation, line + 14, length - 14 + 1);
    }
    bool list = strcmp(operation, "LIST") == 0;
    bool info = strncmp(operation, "INFO ", 5) == 0;
    bool launch = strncmp(operation, "LAUNCH ", 7) == 0;
    bool remove = strncmp(operation, "REMOVE ", 7) == 0;
    esp_err_t error;
    roundie_package_record_t record = {0};
    if (list || info || launch || remove) {
        char frame[21];
        memcpy(frame, line, 14);
        memcpy(frame + 14, "STATUS", 7);
        error = roundie_transfer_parse(frame, discard, "!RP1 ", "INSTALL", &command);
        if (error == ESP_OK) {
            const char *id = operation + (info ? 5 : 7);
            if (list || info) {
                error = roundie_packages_scan();
                if (error == ESP_OK && info) {
                    error = roundie_packages_find(id, &record);
                }
            } else if (launch) {
                error = roundie_packages_launch(id);
            } else {
                error = roundie_packages_remove(id);
            }
        }
    } else {
        error = roundie_transfer_parse(line, discard, "!RP1 ", "INSTALL", &command);
        if (error == ESP_OK) {
            error = transfer_command(&command);
        }
    }
    roundie_packages_status_t status;
    roundie_packages_status(&status);
    for (size_t i = 0; status.error[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)status.error[i];
        if (ch < 0x20 || ch > 0x7e || ch == '"' || ch == '\\') {
            status.error[i] = '_';
        }
    }
    size_t total = 0;
    size_t used = 0;
    bool capacity = roundie_packages_allowed() && roundie_storage_usage(&total, &used);
    char response[1900];
    int count = snprintf(response, sizeof(response),
        "\n!RP1 %s %d {\"format\":1,\"allowed\":%u,\"busy\":%u,\"phase\":%u,"
        "\"session\":\"%08" PRIx32 "\",\"received\":%zu,\"total\":%zu,\"count\":%u,\"orphans\":%u,"
        "\"capacity\":[%u,%zu,%zu],\"us\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "],\"error\":\"%s\",\"apps\":[",
        command.id, (int)error, roundie_packages_allowed(), roundie_packages_busy(), (unsigned)status.phase,
        status.session, status.received, status.total, status.count, status.orphans, capacity, total, used,
        status.upload_us, status.validation_us, status.commit_us, status.enumeration_us,
        status.lookup_us, status.launch_us, status.removal_us, status.error);
    unsigned records = error == ESP_OK ? (list ? status.count : (info ? 1 : 0)) : 0;
    for (unsigned i = 0; i < records; i++) {
        if (list) {
            roundie_packages_get(i, &record);
        }
        if (count < 0 || (size_t)count >= sizeof(response)) {
            return;
        }
        int added = snprintf(response + count, sizeof(response) - (size_t)count,
            "%s[\"%s\",\"%u.%u.%u\",%u,%u]", i == 0 ? "" : ",", record.metadata.id,
            record.metadata.version[0], record.metadata.version[1], record.metadata.version[2],
            record.metadata.abi, (unsigned)record.health);
        if (added < 0) {
            return;
        }
        count += added;
    }
    char name[ROUNDIE_PACKAGE_NAME * 2 + 1] = {0};
    char digest[65] = {0};
    if (info && error == ESP_OK) {
        json_text(name, record.metadata.name);
        hex_digest(digest, record.digest);
    }
    if (count < 0 || (size_t)count >= sizeof(response)) {
        return;
    }
    int added = snprintf(response + count, sizeof(response) - (size_t)count,
        "],\"name\":\"%s\",\"sha256\":\"%s\",\"elf_size\":%" PRIu32 "}\n", name, digest,
        info && error == ESP_OK ? record.metadata.elf_size : 0);
    if (added > 0 && (size_t)(count + added) < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)(count + added), pdMS_TO_TICKS(100));
    }
}
#endif
