#include "gauge_diagnostics.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "gauge_diag";

void roundie_gauge_command(roundie_board_t *board, const char *line, bool discard)
{
    char id[9] = "00000000";
    esp_err_t error = ESP_ERR_INVALID_ARG;
    roundie_gauge_capacity_t capacity = {0};
    size_t length = strlen(line);
    if (length >= 14 && memcmp(line, "!RG1 ", 5) == 0 && line[13] == ' ') {
        bool valid_id = true;
        for (size_t i = 5; i < 13; i++) {
            valid_id = valid_id && ((line[i] >= '0' && line[i] <= '9') ||
                                    (line[i] >= 'a' && line[i] <= 'f'));
        }
        if (valid_id) {
            memcpy(id, line + 5, 8);
        }
        const char *operation = line + 14;
        if (!valid_id || discard || length >= 64) {
            error = ESP_ERR_INVALID_ARG;
        } else if (board == NULL) {
            error = ESP_ERR_INVALID_STATE;
        } else if (strcmp(operation, "STATUS") == 0) {
            error = roundie_board_gauge_capacity_read(board, &capacity);
        } else if (strncmp(operation, "CAPACITY ", 9) == 0) {
            const char *digits = operation + 9;
            char *end = NULL;
            unsigned long value = strtoul(digits, &end, 10);
            if (digits[0] < '0' || digits[0] > '9' || end == NULL || *end != '\0' || value > UINT16_MAX) {
                error = ESP_ERR_INVALID_ARG;
            } else {
                ESP_LOGW(TAG, "Gauge capacity request: %lu mAh", value);
                error = roundie_board_gauge_capacity_set(board, (uint16_t)value, &capacity);
            }
        } else {
            error = ESP_ERR_INVALID_ARG;
        }
    }
    /* A fresh sample shows what the gauge reports after the request. */
    roundie_battery_sample_t battery = {0};
    if (board != NULL) {
        roundie_board_battery_read(board, &battery);
    }
    char response[512];
    int length_out = snprintf(response, sizeof(response),
        "\n!RG1 %s %d {\"format\":1,\"step\":%u,\"design_mah\":%u,\"dm_full_mah\":%u,\"dm_design_mah\":%u,"
        "\"battery_flags\":%u,\"operation_flags\":%u,\"state\":%d,\"voltage_mv\":%u,\"soc\":%u,"
        "\"remaining_mah\":%u,\"full_mah\":%u,\"current_ma\":%d,\"initialized\":%u}\n",
        id, (int)error, capacity.step, capacity.design_mah, capacity.dm_full_mah, capacity.dm_design_mah,
        capacity.battery_flags, capacity.operation_flags, (int)battery.state, battery.voltage_mv,
        battery.soc_percent, battery.remaining_mah, battery.full_mah, battery.current_ma,
        battery.initialized ? 1U : 0U);
    if (length_out > 0 && (size_t)length_out < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)length_out, pdMS_TO_TICKS(100));
    }
}
