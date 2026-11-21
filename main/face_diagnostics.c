#include "face_diagnostics.h"
#if CONFIG_ROUNDIE_DEV_PACKAGES
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "roundie_ota_protocol.h"
#include "roundie_watchface.h"

/* "!RF1 " + 8-character request id + space. */
#define FRAME_HEADER 14U

static esp_err_t transfer_command(const roundie_ota_command_t *command)
{
    switch (command->operation) {
    case ROUNDIE_OTA_STATUS:
        return ESP_OK;
    case ROUNDIE_OTA_BEGIN:
        return roundie_watchface_begin(command->session, command->size, command->digest);
    case ROUNDIE_OTA_DATA:
        return roundie_watchface_write(command->session, command->offset, command->data, command->size);
    case ROUNDIE_OTA_END:
        return roundie_watchface_finish(command->session);
    case ROUNDIE_OTA_ABORT:
        return roundie_watchface_abort(command->session);
    case ROUNDIE_OTA_ACTIVATE:
        return roundie_watchface_install(command->digest);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

/* Store strings are bounded; this only keeps a stray byte out of the JSON. */
static void json_safe(char *text, size_t capacity)
{
    for (size_t i = 0; i < capacity && text[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20 || ch > 0x7e || ch == '"' || ch == '\\') {
            text[i] = '_';
        }
    }
    text[capacity - 1] = '\0';
}

void roundie_face_command(char *line, bool discard)
{
    roundie_watchface_poll(esp_timer_get_time());
    roundie_ota_command_t command = {0};
    size_t length = strlen(line);
    const char *operation = length >= FRAME_HEADER ? line + FRAME_HEADER : "";
    bool select = strncmp(operation, "SELECT ", 7) == 0;
    bool remove = strncmp(operation, "REMOVE ", 7) == 0;
    esp_err_t error;
    if (select || remove) {
        /* Validate the frame and request id through the shared parser with a
         * STATUS body, then hand the id to the store, which checks it. */
        char id[ROUNDIE_FACE_ID] = {0};
        size_t id_length = strlen(operation + 7);
        char frame[FRAME_HEADER + sizeof("STATUS")];
        memcpy(frame, line, FRAME_HEADER);
        memcpy(frame + FRAME_HEADER, "STATUS", sizeof("STATUS"));
        error = roundie_transfer_parse(frame, discard, "!RF1 ", "INSTALL", &command);
        if (error == ESP_OK && (id_length == 0 || id_length >= sizeof(id))) {
            error = ESP_ERR_INVALID_ARG;
        }
        if (error == ESP_OK) {
            memcpy(id, operation + 7, id_length + 1);
            if (select) {
                error = roundie_watchface_select(id);
            } else {
                error = roundie_watchface_remove(id);
            }
        }
    } else {
        error = roundie_transfer_parse(line, discard, "!RF1 ", "INSTALL", &command);
        if (error == ESP_OK) {
            error = transfer_command(&command);
        }
    }
    /* Status and the reply frame are system-worker scratch, as in RP1. */
    static roundie_watchface_status_t status;
    static char response[1900];
    roundie_watchface_status(&status);
    json_safe(status.preferred, sizeof(status.preferred));
    json_safe(status.active, sizeof(status.active));
    json_safe(status.failed, sizeof(status.failed));
    json_safe(status.error, sizeof(status.error));
    int count = snprintf(response, sizeof(response),
        "\n!RF1 %s %d {\"format\":1,\"phase\":%" PRIu32 ",\"received\":%" PRIu32 ",\"total\":%" PRIu32
        ",\"count\":%" PRIu32 ",\"objects\":%" PRIu32 ",\"pool\":%" PRIu32 ",\"bytes\":%" PRIu32
        ",\"home\":%u,\"parked\":%u,\"gallery\":%u,\"fallback_only\":%u,\"loads\":%" PRIu32
        ",\"load_failures\":%" PRIu32 ",\"counters\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "],\"us\":[%" PRIu32 ",%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "],\"preferred\":\"%s\",\"active\":\"%s\",\"failed\":\"%s\","
        "\"error\":\"%s\",\"faces\":[",
        command.id, (int)error, status.phase, status.received, status.total, status.count, status.objects,
        status.pool_free, status.face_bytes, status.home, status.parked, status.gallery, status.fallback_only,
        status.loads, status.load_failures, status.creates, status.destroys, status.activations,
        status.deactivations, status.updates, status.switches, status.failures, status.refused, status.resumes,
        status.load_us, status.activate_us, status.activate_max_us, status.update_max_us, status.resume_us,
        status.preferred, status.active, status.failed, status.error);
    uint32_t faces = status.count <= ROUNDIE_WATCHFACE_MAX ? status.count : ROUNDIE_WATCHFACE_MAX;
    for (uint32_t i = 0; i < faces; i++) {
        if (count < 0 || (size_t)count >= sizeof(response)) {
            return;
        }
        const roundie_watchface_entry_t *entry = &status.entries[i];
        int added = snprintf(response + count, sizeof(response) - (size_t)count,
            "%s[\"%s\",\"%u.%u.%u\",%u]", i == 0 ? "" : ",", entry->id, entry->version[0], entry->version[1],
            entry->version[2], (unsigned)entry->healthy);
        if (added < 0) {
            return;
        }
        count += added;
    }
    if (count < 0 || (size_t)count >= sizeof(response)) {
        return;
    }
    int added = snprintf(response + count, sizeof(response) - (size_t)count, "]}\n");
    if (added > 0 && (size_t)(count + added) < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)(count + added), pdMS_TO_TICKS(100));
    }
}
#endif
