#include "stall_probe.h"
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_DEV_PACKAGES
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "roundie_elf.h"

#define PROBE_STACK 2048U
#define PROBE_HIGH_PRIORITY 20U

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static bool armed;
static bool created;
static int64_t gui_last_us;
static roundie_stall_report_t report;
static int64_t probe_last_us[ROUNDIE_STALL_PROBES];

static void probe_task(void *argument)
{
    unsigned index = (unsigned)(uintptr_t)argument;
    while (true) {
        vTaskDelay(1);
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&lock);
        if (armed) {
            uint32_t gap = (uint32_t)(now - probe_last_us[index]);
            if (gap > report.probe[index].gap_max_us) {
                report.probe[index].gap_max_us = gap;
            }
            report.probe[index].wakes++;
            probe_last_us[index] = now;
        }
        portEXIT_CRITICAL(&lock);
    }
}

void roundie_stall_gui_mark(int64_t iteration_at_us, uint32_t late_us)
{
    portENTER_CRITICAL(&lock);
    if (armed) {
        if (gui_last_us != 0) {
            uint32_t gap = (uint32_t)(iteration_at_us - gui_last_us);
            if (gap > report.gui_gap_max_us) {
                report.gui_gap_max_us = gap;
            }
        }
        if (late_us > report.gui_late_max_us) {
            report.gui_late_max_us = late_us;
        }
        report.gui_iterations++;
        gui_last_us = iteration_at_us;
    }
    portEXIT_CRITICAL(&lock);
}

static bool create_probes(void)
{
    for (unsigned i = 0; i < ROUNDIE_STALL_PROBES; i++) {
        UBaseType_t priority = i < 2U ? 1U : PROBE_HIGH_PRIORITY;
        BaseType_t core = (BaseType_t)(i % 2U);
        if (xTaskCreatePinnedToCore(probe_task, "stall_probe", PROBE_STACK, (void *)(uintptr_t)i,
                                    priority, NULL, core) != pdPASS) {
            return false;
        }
        report.probe[i].core = (uint8_t)core;
        report.probe[i].priority = (uint8_t)priority;
    }
    return true;
}

bool roundie_stall_arm(void)
{
    if (!created) {
        /* Never deleted: a partial set stays unusable rather than retried. */
        created = true;
        if (!create_probes()) {
            return false;
        }
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    report.gui_iterations = 0;
    report.gui_gap_max_us = 0;
    report.gui_late_max_us = 0;
    gui_last_us = 0;
    for (unsigned i = 0; i < ROUNDIE_STALL_PROBES; i++) {
        report.probe[i].wakes = 0;
        report.probe[i].gap_max_us = 0;
        probe_last_us[i] = now;
    }
    armed = true;
    portEXIT_CRITICAL(&lock);
    return report.probe[ROUNDIE_STALL_PROBES - 1U].priority != 0;
}

void roundie_stall_disarm(roundie_stall_report_t *out)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    armed = false;
    /* A probe still stopped at disarm has an open gap up to now. */
    for (unsigned i = 0; i < ROUNDIE_STALL_PROBES; i++) {
        uint32_t open_gap = (uint32_t)(now - probe_last_us[i]);
        if (open_gap > report.probe[i].gap_max_us) {
            report.probe[i].gap_max_us = open_gap;
        }
    }
    if (gui_last_us != 0 && (uint32_t)(now - gui_last_us) > report.gui_gap_max_us) {
        report.gui_gap_max_us = (uint32_t)(now - gui_last_us);
    }
    *out = report;
    portEXIT_CRITICAL(&lock);
}

/* Bounded so the executor still answers lifecycle requests promptly. */
#define ELF_DELAY_MAX_MS 1000U

/* "TICKGAP": application timer posts and their longest gap since the last
 * TICKGAP. "LOGLEVEL <0..5>": esp_log level of the rep_app tag (app_log). */
static bool tick_command(const char *operation, const char *id, int parsed)
{
    esp_err_t error = parsed;
    char response[96];
    int length = 0;
    if (strcmp(operation, "TICKGAP") == 0) {
        uint32_t ticks = 0;
        uint32_t gap_max_us = 0;
        if (error == ESP_OK) {
            roundie_elf_debug_tick_gap(&ticks, &gap_max_us);
        }
        length = snprintf(response, sizeof(response),
                          "\n!RP1 %s %d {\"format\":1,\"ticks\":%" PRIu32 ",\"gap_max_us\":%" PRIu32 "}\n",
                          id, (int)error, ticks, gap_max_us);
    } else if (strncmp(operation, "LOGLEVEL ", 9) == 0) {
        unsigned level = 0;
        int consumed = 0;
        if (error == ESP_OK && (sscanf(operation + 9, "%u%n", &level, &consumed) != 1 ||
                                operation[9 + consumed] != '\0' || level > ESP_LOG_VERBOSE)) {
            error = ESP_ERR_INVALID_ARG;
        }
        if (error == ESP_OK) {
            esp_log_level_set("rep_app", (esp_log_level_t)level);
        }
        length = snprintf(response, sizeof(response), "\n!RP1 %s %d {\"format\":1,\"loglevel\":%u}\n",
                          id, (int)error, error == ESP_OK ? level : 0U);
    } else {
        return false;
    }
    if (length > 0 && (size_t)length < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)length, pdMS_TO_TICKS(100));
    }
    return true;
}

/* "STALL ARM" / "STALL REPORT" (E0 closure, inversion experiment): the
 * host arms the probes, sends other commands (RP1 LIST) and reads the gaps.
 * ARM is refused while an FSBENCH X job holds the probes. */
static bool host_armed;

static bool stall_command(const char *operation, const char *id, int parsed)
{
    bool arm = strcmp(operation, "STALL ARM") == 0;
    if (!arm && strcmp(operation, "STALL REPORT") != 0) {
        return false;
    }
    esp_err_t error = parsed;
    roundie_stall_report_t stall = {0};
    if (error == ESP_OK) {
        portENTER_CRITICAL(&lock);
        bool busy = armed;
        portEXIT_CRITICAL(&lock);
        if (arm) {
            if (busy) {
                error = ESP_ERR_INVALID_STATE;
            } else if (!roundie_stall_arm()) {
                error = ESP_FAIL;
            } else {
                host_armed = true;
            }
        } else if (!host_armed || !busy) {
            error = ESP_ERR_INVALID_STATE;
        } else {
            roundie_stall_disarm(&stall);
            host_armed = false;
        }
    }
    char response[320];
    int length = snprintf(response, sizeof(response),
        "\n!RP1 %s %d {\"format\":1,\"stall\":\"%s\",\"gui\":[%" PRIu32 ",%" PRIu32 ",%" PRIu32 "],\"probes\":[",
        id, (int)error, arm ? "ARM" : "REPORT", stall.gui_iterations, stall.gui_gap_max_us, stall.gui_late_max_us);
    for (unsigned i = 0; i < ROUNDIE_STALL_PROBES && length > 0 && (size_t)length < sizeof(response); i++) {
        length += snprintf(response + length, sizeof(response) - (size_t)length,
            "%s[%u,%u,%" PRIu32 ",%" PRIu32 "]", i == 0U ? "" : ",", stall.probe[i].core,
            stall.probe[i].priority, stall.probe[i].wakes, stall.probe[i].gap_max_us);
    }
    if (length > 0 && (size_t)length < sizeof(response)) {
        length += snprintf(response + length, sizeof(response) - (size_t)length, "]}\n");
    }
    if (length > 0 && (size_t)length < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)length, pdMS_TO_TICKS(100));
    }
    return true;
}

bool roundie_race_probe_command(const char *operation, const char *id, int parsed)
{
    if (tick_command(operation, id, parsed)) {
        return true;
    }
    if (stall_command(operation, id, parsed)) {
        return true;
    }
    if (strncmp(operation, "ELFDELAY ", 9) != 0) {
        return false;
    }
    esp_err_t error = parsed;
    unsigned ms = 0;
    int consumed = 0;
    if (error == ESP_OK && (sscanf(operation + 9, "%u%n", &ms, &consumed) != 1 ||
                            operation[9 + consumed] != '\0' || ms > ELF_DELAY_MAX_MS)) {
        error = ESP_ERR_INVALID_ARG;
    }
    if (error == ESP_OK) {
        roundie_elf_debug_delivery_delay(ms);
    }
    char response[96];
    int length = snprintf(response, sizeof(response), "\n!RP1 %s %d {\"format\":1,\"elfdelay\":%u}\n",
                          id, (int)error, error == ESP_OK ? ms : 0U);
    if (length > 0 && (size_t)length < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)length, pdMS_TO_TICKS(100));
    }
    return true;
}
#endif
