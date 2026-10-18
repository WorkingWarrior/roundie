#pragma once

#include "roundie_ota.h"

#define ROUNDIE_OTA_LINE_CAPACITY 1100
typedef enum {
    ROUNDIE_OTA_STATUS, ROUNDIE_OTA_BEGIN, ROUNDIE_OTA_DATA,
    ROUNDIE_OTA_END, ROUNDIE_OTA_ABORT, ROUNDIE_OTA_ACTIVATE, ROUNDIE_OTA_RESTART,
} roundie_ota_operation_t;

typedef struct {
    char id[9];
    roundie_ota_operation_t operation;
    uint32_t session;
    size_t offset;
    size_t size;
    uint8_t digest[32];
    uint8_t data[512];
} roundie_ota_command_t;

/* Strict ASCII line without LF. Modifies input; always initializes command. */
esp_err_t roundie_ota_parse(char *line, bool discard, roundie_ota_command_t *out);
/* RESTART policy/transport drain belongs to the caller; not executed here. */
esp_err_t roundie_ota_execute(const roundie_ota_command_t *command);
