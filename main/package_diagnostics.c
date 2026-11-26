#include "package_diagnostics.h"
#if CONFIG_ROUNDIE_DEV_PACKAGES
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "roundie_packages.h"
#include "roundie_elf.h"
#include "roundie_ota_protocol.h"
#include "app_runtime.h"
#include "storage_diagnostics.h"
#include "roundie_launcher.h"
#include "gui_diagnostics.h"
#include "storage_bench.h"
#include "memory_probe.h"
#include "stall_probe.h"
#include "psa/crypto.h"
#include "rep/rep_storage_v1.h"

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

static bool navigation_number(const char **text, uint32_t *value)
{
    const char *cursor = *text;
    if (*cursor < '0' || *cursor > '9') {
        return false;
    }
    uint32_t number = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        uint32_t digit = (uint32_t)(*cursor++ - '0');
        if (number > (UINT32_MAX - digit) / 10) {
            return false;
        }
        number = number * 10 + digit;
    }
    *value = number;
    *text = cursor;
    return true;
}

/* "DATAHASH <id>" (P1 fix 4): length and SHA-256 of an installed package's
 * app data blob, as roundie_packages_data_read() returns it (the payload, not
 * the RPD1 file). Lets a device test compare state across a relaunch instead of
 * judging the screen. Read only; runs on io_worker like every RP1 line. */
static bool data_hash_command(const char *operation, const char *reply_id, esp_err_t parsed)
{
    if (strncmp(operation, "DATAHASH ", 9) != 0) {
        return false;
    }
    const char *id = operation + 9;
    static uint8_t blob[REP_STORAGE_CAPACITY];
    size_t length = 0;
    uint8_t digest[32] = {0};
    size_t digest_length = 0;
    roundie_package_record_t record;
    esp_err_t error = parsed;
    if (error == ESP_OK) {
        error = roundie_packages_scan();
    }
    if (error == ESP_OK) {
        error = roundie_packages_find(id, &record);
    }
    if (error == ESP_OK) {
        error = roundie_packages_data_read(id, blob, sizeof(blob), &length);
    }
    if (error == ESP_OK &&
        (psa_hash_compute(PSA_ALG_SHA_256, blob, length, digest, sizeof(digest), &digest_length) != PSA_SUCCESS ||
         digest_length != sizeof(digest))) {
        error = ESP_FAIL;
    }
    if (error != ESP_OK) {
        length = 0;
        memset(digest, 0, sizeof(digest));
    }
    char hex[65];
    for (size_t i = 0; i < sizeof(digest); i++) {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }
    char response[192];
    int count = snprintf(response, sizeof(response),
        "\n!RP1 %s %d {\"format\":1,\"datahash\":%zu,\"sha256\":\"%s\"}\n",
        reply_id, (int)error, length, hex);
    if (count > 0 && (size_t)count < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)count, pdMS_TO_TICKS(100));
    }
    return true;
}

static bool navigation_command(const char *operation, const roundie_ota_command_t *command,
                               esp_err_t parsed)
{
    if (strcmp(operation, "NAV") != 0 && strcmp(operation, "REFRESH") != 0 && strcmp(operation, "OPEN") != 0 &&
        strcmp(operation, "HOME") != 0 && strncmp(operation, "SELECT ", 7) != 0 && strncmp(operation, "EXIT ", 5) != 0 &&
        strncmp(operation, "BACK ", 5) != 0) {
        return false;
    }
    esp_err_t result = parsed;
    if (result == ESP_OK && strcmp(operation, "REFRESH") == 0) {
        result = roundie_launcher_refresh();
    } else if (result == ESP_OK && strcmp(operation, "OPEN") == 0) {
        /* Development: Home to the list, as the swipe up (M10). */
        result = roundie_launcher_open_list();
    } else if (result == ESP_OK && strcmp(operation, "HOME") == 0) {
        /* Development (E0): the list to Home, as the gesture in render.c. */
        result = roundie_launcher_home();
    } else if (result == ESP_OK && strncmp(operation, "SELECT ", 7) == 0) {
        uint32_t revision;
        const char *id = operation + 7;
        if (!navigation_number(&id, &revision) || *id++ != ' ' || !roundie_package_id_valid(id)) {
            result = ESP_ERR_INVALID_ARG;
        } else {
            /* Development semantic selection, same mailbox as real touch. */
            result = roundie_app_runtime_allowed() ? roundie_launcher_select(id, revision, 2) :
                ESP_ERR_INVALID_STATE;
        }
    } else if (result == ESP_OK && strncmp(operation, "EXIT ", 5) == 0) {
        uint32_t generation;
        const char *end = operation + 5;
        result = navigation_number(&end, &generation) && *end == '\0' ?
            roundie_launcher_exit(generation) : ESP_ERR_INVALID_ARG;
    } else if (result == ESP_OK && strncmp(operation, "BACK ", 5) == 0) {
        uint32_t request;
        const char *end = operation + 5;
        result = navigation_number(&end, &request) && *end == '\0' ?
            roundie_launcher_back(request) : ESP_ERR_INVALID_ARG;
    }
    static roundie_launcher_status_t navigation;
    roundie_launcher_status(&navigation);
    static char reply[1800];
    char name[ROUNDIE_PACKAGE_NAME * 2 + 1];
    json_text(name, navigation.selected_name);
    int length = snprintf(reply, sizeof(reply),
        "\n!RP1 %s %d {\"nav\":%u,\"revision\":%" PRIu32 ",\"request\":%" PRIu32
        ",\"generation\":%" PRIu32 ",\"count\":%" PRIu32 ",\"selected\":\"%s\","
        "\"pending\":[%u,%u],\"foreground\":%u,\"input\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 "],"
        "\"result\":[%" PRId32 ",%" PRId32 ",%" PRId32 "],"
        "\"us\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "],\"gui\":[%" PRIu32 ",%" PRIu32 "],\"discovery_flushes\":%" PRIu32
        ",\"stack_min\":%u,\"name\":\"%s\",\"completion\":[%" PRIu32 ",%" PRIu32 ",%d,%u,%u,%u,%" PRIu32 ",%" PRIu32 "]"
        ",\"paint\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRId64 "]}\n",
        command->id, (int)result, (unsigned)navigation.state, navigation.revision, navigation.request,
        navigation.generation, navigation.count, navigation.selected, navigation.launch_pending,
        navigation.exit_pending, navigation.foreground, navigation.physical, navigation.injected,
        navigation.rejected, navigation.launch_result, navigation.unload_result, navigation.app_result,
        navigation.refresh_us, navigation.copy_us, navigation.enqueue_max_us, navigation.admission_us,
        navigation.started_us, navigation.exit_admission_us, navigation.return_us, navigation.gui_create_us,
        navigation.gui_latency_max_us, navigation.gui_objects, navigation.pool_free, navigation.discovery_flushes,
        (unsigned)uxTaskGetStackHighWaterMark(NULL), name, navigation.result_request, navigation.completed,
        (int)navigation.loader_result, navigation.executed, navigation.result_success, navigation.result_visible,
        navigation.result_apply_us, navigation.completion_to_gui_us, navigation.painted_state,
        navigation.painted_request, navigation.painted_generation, navigation.gui_ready_us, navigation.gui_applied_us);
    if (length > 0 && (size_t)length < sizeof(reply)) {
        usb_serial_jtag_write_bytes(reply, (size_t)length, pdMS_TO_TICKS(100));
    }
    return true;
}

void roundie_package_command(char *line, bool discard)
{
    roundie_packages_poll(esp_timer_get_time());
    roundie_ota_command_t command;
    char operation[96] = {0};
    size_t length = strlen(line);
    if (length >= 14 && length - 14 < sizeof(operation)) {
        memcpy(operation, line + 14, length - 14 + 1);
    }
    char navigation_frame[21];
    if (length >= 14) {
        memcpy(navigation_frame, line, 14);
        memcpy(navigation_frame + 14, "STATUS", 7);
        esp_err_t parsed = roundie_transfer_parse(navigation_frame, discard, "!RP1 ", "INSTALL", &command);
        if (navigation_command(operation, &command, parsed)) {
            return;
        }
        if (roundie_gui_command(operation, command.id, parsed)) {
            return;
        }
        if (roundie_storage_bench_command(operation, command.id, parsed)) {
            return;
        }
        if (roundie_memory_probe_command(operation, command.id, parsed)) {
            return;
        }
        if (roundie_race_probe_command(operation, command.id, parsed)) {
            return;
        }
        if (data_hash_command(operation, command.id, parsed)) {
            return;
        }
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
    bool capacity = roundie_app_runtime_allowed() && roundie_storage_usage(&total, &used);
    /* Sole system-worker scratch: do not keep a 1900-byte reply frame on the
     * stack across nested LittleFS/PSA validation and commit calls. */
    static char response[1900];
    int count = snprintf(response, sizeof(response),
        "\n!RP1 %s %d {\"format\":1,\"allowed\":%u,\"busy\":%u,\"phase\":%u,"
        "\"session\":\"%08" PRIx32 "\",\"received\":%zu,\"total\":%zu,\"count\":%u,\"orphans\":%u,"
        "\"capacity\":[%u,%zu,%zu],\"stack_min\":%u,\"us\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "],\"error\":\"%s\",\"apps\":[",
        command.id, (int)error, roundie_app_runtime_allowed(), roundie_packages_busy(), (unsigned)status.phase,
        status.session, status.received, status.total, status.count, status.orphans, capacity, total, used,
        (unsigned)uxTaskGetStackHighWaterMark(NULL),
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
