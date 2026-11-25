#include "gauge_diagnostics.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "gauge_learning_service.h"

static const char *TAG = "gauge_diag";

static void send(const char *response, int length)
{
    if (length > 0) {
        usb_serial_jtag_write_bytes(response, (size_t)length, pdMS_TO_TICKS(100));
    }
}

#if defined(CONFIG_ROUNDIE_GAUGE_DESIGN_CAPACITY_MAH) && CONFIG_ROUNDIE_DEV_POWER
/* LEARN, LEARN HOLD <minutes 1..1440>, LEARN STOP: reply with the learning
 * report. LEARN LOG <from> and LEARN END reply with RAM state only;
 * LEARN TEST END|STOP <s> arms a simulated end and replies like END;
 * LEARN WINDOW replies with the battery test window (RAM only).
 * Returns false when operation is not a LEARN command. */
static bool learn_command(const char *id, const char *operation, bool board_ready)
{
    if (strcmp(operation, "LEARN") != 0 && strncmp(operation, "LEARN ", 6) != 0) {
        return false;
    }
    const int64_t now_us = esp_timer_get_time();
    esp_err_t error = ESP_OK;
    bool log_reply = false;
    bool end_reply = false;
    bool window_reply = false;
    unsigned long log_from = 0;
    if (strcmp(operation, "LEARN END") == 0) {
        end_reply = true;
    } else if (strcmp(operation, "LEARN WINDOW") == 0) {
        window_reply = true;
    } else if (strncmp(operation, "LEARN LOG ", 10) == 0) {
        const char *digits = operation + 10;
        char *end = NULL;
        log_from = strtoul(digits, &end, 10);
        log_reply = true;
        if (digits[0] < '0' || digits[0] > '9' || end == NULL || *end != '\0' || end - digits > 10 ||
            log_from > UINT32_MAX) {
            error = ESP_ERR_INVALID_ARG;
            log_reply = false;
        }
    } else if (!board_ready) {
        error = ESP_ERR_INVALID_STATE;
    } else if (strncmp(operation, "LEARN TEST ", 11) == 0) {
        /* LEARN TEST END|STOP <delay 0..3600>: development simulation. */
        const char *rest = operation + 11;
        roundie_gauge_test_t test = ROUNDIE_GAUGE_TEST_NONE;
        if (strncmp(rest, "END ", 4) == 0) {
            test = ROUNDIE_GAUGE_TEST_END;
            rest += 4;
        } else if (strncmp(rest, "STOP ", 5) == 0) {
            test = ROUNDIE_GAUGE_TEST_STOP;
            rest += 5;
        }
        char *end = NULL;
        unsigned long delay_s = strtoul(rest, &end, 10);
        if (test == ROUNDIE_GAUGE_TEST_NONE || rest[0] < '0' || rest[0] > '9' || end == NULL || *end != '\0' ||
            end - rest > 4 || delay_s > 3600) {
            error = ESP_ERR_INVALID_ARG;
        } else {
            error = roundie_gauge_service_test(test, (uint32_t)delay_s);
            end_reply = true;
        }
    } else if (strcmp(operation, "LEARN STOP") == 0) {
        error = roundie_gauge_service_hold(0, now_us);
    } else if (strncmp(operation, "LEARN HOLD ", 11) == 0) {
        const char *digits = operation + 11;
        char *end = NULL;
        unsigned long minutes = strtoul(digits, &end, 10);
        if (digits[0] < '1' || digits[0] > '9' || end == NULL || *end != '\0' || minutes > 1440) {
            error = ESP_ERR_INVALID_ARG;
        } else {
            ESP_LOGW(TAG, "Learning load hold requested: %lu min", minutes);
            error = roundie_gauge_service_hold((uint32_t)minutes, now_us);
        }
    } else if (strcmp(operation, "LEARN") != 0) {
        error = ESP_ERR_INVALID_ARG;
    }
    /* Worst case about 830 bytes: needs the 2048-byte TX ring of RS1/RO1 builds. */
    char response[1024];
    int length = snprintf(response, sizeof(response), "\n!RG1 %s %d ", id, (int)error);
    if (length > 0 && (size_t)length < sizeof(response)) {
        int body = 0;
        if (log_reply) {
            /* Answered without the board: the ring is RAM only. */
            body = roundie_gauge_service_log_json(response + length, sizeof(response) - (size_t)length - 1,
                                                  (uint32_t)log_from);
        } else if (window_reply) {
            body = roundie_gauge_service_window_json(response + length, sizeof(response) - (size_t)length - 1,
                                                     now_us);
        } else if (end_reply) {
            body = roundie_gauge_service_end_json(response + length, sizeof(response) - (size_t)length - 1, now_us);
        } else {
            body = roundie_gauge_service_json(response + length, sizeof(response) - (size_t)length - 1, now_us);
        }
        if (body > 0 && (size_t)(length + body) < sizeof(response) - 1) {
            length += body;
            response[length++] = '\n';
            send(response, length);
            return true;
        }
    }
    ESP_LOGE(TAG, "LEARN reply does not fit");
    return true;
}
#endif

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
#if defined(CONFIG_ROUNDIE_GAUGE_DESIGN_CAPACITY_MAH) && CONFIG_ROUNDIE_DEV_POWER
        } else if (learn_command(id, operation, board != NULL)) {
            return;
#endif
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
                error = roundie_board_gauge_capacity_set(board, (uint16_t)value, (uint16_t)value, &capacity);
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
    char response[768];
    int length_out = snprintf(response, sizeof(response),
        "\n!RG1 %s %d {\"format\":3,\"step\":%u,\"written\":%u,\"design_mah\":%u,\"dm_full_mah\":%u,"
        "\"dm_design_mah\":%u,\"gauging_config\":%u,\"design_voltage_mv\":%u,\"charging_voltage_mv\":%u,"
        "\"taper_current_ma\":%u,\"near_full_mah\":%u,\"battery_low_centi\":%u,\"learning_low_temp\":%u,"
        "\"overload_ma\":%u,\"reserve_mah\":%u,\"flag_config_a\":%u,\"flag_config_b\":%u,\"battery_flags\":%u,\"operation_flags\":%u,"
        "\"state\":%d,\"voltage_mv\":%u,\"soc\":%u,\"remaining_mah\":%u,\"full_mah\":%u,"
        "\"current_ma\":%d,\"initialized\":%u}\n",
        id, (int)error, capacity.step, capacity.written ? 1U : 0U, capacity.design_mah,
        capacity.dm_full_mah, capacity.dm_design_mah, capacity.gauging_config,
        capacity.design_voltage_mv, capacity.charging_voltage_mv, capacity.taper_current_ma,
        capacity.near_full_mah, capacity.battery_low_centi, capacity.learning_low_temp_deci_c,
        capacity.overload_ma, capacity.reserve_mah, capacity.flag_config_a, capacity.flag_config_b,
        capacity.battery_flags, capacity.operation_flags,
        (int)battery.state, battery.voltage_mv, battery.soc_percent, battery.remaining_mah,
        battery.full_mah, battery.current_ma, battery.initialized ? 1U : 0U);
    if (length_out > 0 && (size_t)length_out < sizeof(response)) {
        send(response, length_out);
    }
}
