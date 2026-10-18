#include "roundie_ota_protocol.h"

#include <string.h>

static int digit(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

static bool hex(const char *text, uint8_t *out, size_t size)
{
    if (strlen(text) != size * 2) {
        return false;
    }
    for (size_t i = 0; i < size; i++) {
        int high = digit(text[i * 2]);
        int low = digit(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool session_id(const char *text, uint32_t *out)
{
    uint8_t bytes[4];
    if (!hex(text, bytes, 4)) {
        return false;
    }
    *out = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
    return *out != 0;
}

static bool number(const char *text, size_t *out)
{
    *out = 0;
    if (*text == '\0') {
        return false;
    }
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9' || i >= 7) {
            return false;
        }
        *out = *out * 10 + (unsigned)(text[i] - '0');
    }
    return *out <= 0x380000;
}

esp_err_t roundie_ota_parse(char *line, bool discard, roundie_ota_command_t *out)
{
    *out = (roundie_ota_command_t){.id = "00000000"};
    size_t length = strlen(line);
    if (length < 14 || memcmp(line, "!RO1 ", 5) != 0 || line[13] != ' ') {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 5; i < 13; i++) {
        if (digit(line[i]) < 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    memcpy(out->id, line + 5, 8);
    if (discard || length >= ROUNDIE_OTA_LINE_CAPACITY) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *fields[5];
    size_t count = 1;
    fields[0] = line + 14;
    for (char *p = line + 14; *p != '\0'; p++) {
        if (*p == ' ') {
            if (p == fields[count - 1] || count == 5 || p[1] == '\0') {
                return ESP_ERR_INVALID_ARG;
            }
            *p = '\0';
            fields[count++] = p + 1;
        } else if (*p < 0x21 || *p > 0x7e) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (count == 1) {
        if (strcmp(fields[0], "STATUS") == 0) {
            out->operation = ROUNDIE_OTA_STATUS;
            return ESP_OK;
        }
        if (strcmp(fields[0], "RESTART") == 0) {
            out->operation = ROUNDIE_OTA_RESTART;
            return ESP_OK;
        }
    }
    if (count == 2 && strcmp(fields[0], "ACTIVATE") == 0 && hex(fields[1], out->digest, 32)) {
        out->operation = ROUNDIE_OTA_ACTIVATE;
        return ESP_OK;
    }
    if (count < 2 || !session_id(fields[1], &out->session)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count == 2 && (strcmp(fields[0], "END") == 0 || strcmp(fields[0], "ABORT") == 0)) {
        out->operation = strcmp(fields[0], "END") == 0 ? ROUNDIE_OTA_END : ROUNDIE_OTA_ABORT;
        return ESP_OK;
    }
    if (count == 4 && strcmp(fields[0], "BEGIN") == 0 &&
        number(fields[2], &out->size) && hex(fields[3], out->digest, 32)) {
        out->operation = ROUNDIE_OTA_BEGIN;
        return ESP_OK;
    }
    if (count == 4 && strcmp(fields[0], "DATA") == 0 && number(fields[2], &out->offset)) {
        size_t length = strlen(fields[3]);
        out->size = length / 2;
        if (length == 0 || length % 2 != 0 || out->size > sizeof(out->data) ||
            !hex(fields[3], out->data, out->size)) {
            return ESP_ERR_INVALID_SIZE;
        }
        out->operation = ROUNDIE_OTA_DATA;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t roundie_ota_execute(const roundie_ota_command_t *command)
{
    switch (command->operation) {
    case ROUNDIE_OTA_STATUS: return ESP_OK;
    case ROUNDIE_OTA_BEGIN: return roundie_ota_begin(command->session, command->size, command->digest);
    case ROUNDIE_OTA_DATA: return roundie_ota_write(command->session, command->offset, command->data, command->size);
    case ROUNDIE_OTA_END: return roundie_ota_finish(command->session);
    case ROUNDIE_OTA_ABORT: return roundie_ota_abort(command->session);
    case ROUNDIE_OTA_ACTIVATE: return roundie_ota_activate(command->digest);
    case ROUNDIE_OTA_RESTART: return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_ERR_INVALID_ARG;
}
