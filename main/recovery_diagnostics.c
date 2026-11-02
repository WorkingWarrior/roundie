#include "recovery_diagnostics.h"

#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "roundie_ota.h"
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_APP_RUNTIME
#include "app_runtime.h"
#include "roundie_elf.h"
#include "esp_timer.h"
static bool deferred_safe_restart;
#endif

bool roundie_recovery_active(void)
{
    roundie_boot_status_t status;
    roundie_boot_status(&status);
    return status.safe;
}

bool roundie_recovery_skip_gui(void)
{
    roundie_boot_status_t status;
    roundie_boot_status(&status);
    return status.skip_gui;
}

bool roundie_recovery_restart_allowed(void)
{
#if CONFIG_ROUNDIE_APP_RUNTIME
    if (roundie_app_runtime_busy()) {
        return false;
    }
#endif
    roundie_ota_status_t ota;
    roundie_ota_status(&ota);
    return ota.initialized && ota.running.state == ESP_OTA_IMG_VALID &&
        (ota.phase == ROUNDIE_OTA_IDLE ||
         (ota.phase == ROUNDIE_OTA_ACTIVATION_PENDING && ota.last_error == ESP_OK));
}

bool roundie_recovery_confirmation_allowed(int ota_state)
{
    roundie_boot_status_t status;
    roundie_boot_status(&status);
    /* LittleFS remains optional in the existing core self-test. All actual
     * core/GUI/DMA checks are still required. An inherited latch is no pass. */
    return ota_state != ESP_OTA_IMG_PENDING_VERIFY || !status.safe ||
        status.reason == ROUNDIE_SAFE_STORAGE;
}

void roundie_recovery_poll(int64_t now_us)
{
#if CONFIG_ROUNDIE_APP_RUNTIME
    if (deferred_safe_restart && !roundie_app_runtime_busy()) {
        deferred_safe_restart = false;
        if (roundie_boot_persistence_sync()) {
            roundie_boot_planned_restart();
            esp_restart();
        } else {
            ESP_LOGE("recovery", "Deferred Safe Mode persistence failed; restart refused");
        }
        return;
    }
#endif
    roundie_ota_status_t ota;
    roundie_ota_status(&ota);
    /* Recovery's operational marker means transport + core are serviceable;
     * it is deliberately NOT an OTA pass signal. */
    if (ota.boot_checked && (roundie_recovery_active() || ota.boot_passed)) {
        roundie_boot_operational(now_us, true);
    }
    roundie_boot_tick(now_us);
#if CONFIG_ROUNDIE_APP_RUNTIME
    if (!roundie_app_runtime_busy()) {
        roundie_boot_persistence_sync();
    }
#else
    roundie_boot_persistence_sync();
#endif
}

void roundie_recovery_command(char *line, bool discard)
{
    char id[9] = "00000000";
    esp_err_t error = ESP_ERR_INVALID_ARG;
    bool restart = false;
    bool fault = false;
    size_t length = strlen(line);
    if (length >= 14 && memcmp(line, "!RS1 ", 5) == 0 && line[13] == ' ') {
        bool valid_id = true;
        for (size_t i = 5; i < 13; i++) {
            valid_id = valid_id && ((line[i] >= '0' && line[i] <= '9') ||
                                    (line[i] >= 'a' && line[i] <= 'f'));
        }
        if (valid_id) {
            memcpy(id, line + 5, 8);
            if (!discard && length < 128) {
                const char *operation = line + 14;
                roundie_ota_status_t ota;
                roundie_ota_status(&ota);
                bool idle_valid = ota.initialized && ota.running.state == ESP_OTA_IMG_VALID &&
                    ota.phase == ROUNDIE_OTA_IDLE;
                bool app_idle = true;
#if CONFIG_ROUNDIE_APP_RUNTIME
                app_idle = !roundie_app_runtime_busy();
#endif
                if (strcmp(operation, "STATUS") == 0 || strcmp(operation, "SLOT STATUS") == 0) {
                    error = ESP_OK;
                } else if (strcmp(operation, "REQUEST") == 0 && idle_valid) {
                    roundie_boot_enter_safe(ROUNDIE_SAFE_REQUEST);
#if CONFIG_ROUNDIE_APP_RUNTIME
                    roundie_elf_set_allowed(false);
                    roundie_app_runtime_poll(esp_timer_get_time());
#endif
                    /* NVS commit is a Flash write too. REQUEST first closes
                     * admission; persistence/restart follow completed teardown. */
                    error = app_idle ? (roundie_boot_persistence_sync() ? ESP_OK : ESP_FAIL) : ESP_OK;
                    /* Enter recovery through the early boot path. No GUI work
                     * or storage teardown is performed by the USB owner. */
                    restart = error == ESP_OK;
#if CONFIG_ROUNDIE_APP_RUNTIME
                    if (restart && !app_idle) {
                        deferred_safe_restart = true;
                        restart = false;
                    }
#endif
                } else if (strcmp(operation, "CLEAR") == 0 && idle_valid && app_idle && roundie_boot_clear()) {
                    error = roundie_boot_persistence_sync() ? ESP_OK : ESP_FAIL;
                } else if (strcmp(operation, "REBOOT") == 0 && roundie_recovery_active() &&
                           roundie_recovery_restart_allowed()) {
                    if (ota.phase == ROUNDIE_OTA_ACTIVATION_PENDING) {
                        error = ESP_OK;
                    } else {
                        error = roundie_boot_normal_trial() && roundie_boot_persistence_sync() ? ESP_OK : ESP_FAIL;
                    }
                    restart = error == ESP_OK;
                } else if (strncmp(operation, "SELECT ", 7) == 0 && length == 14 + 7 + 5 + 1 + 64 &&
                           roundie_recovery_active() && idle_valid && app_idle && operation[12] == ' ') {
                    char label[6];
                    memcpy(label, operation + 7, 5);
                    label[5] = '\0';
                    error = roundie_ota_select_valid(label, operation + 13);
#if CONFIG_ROUNDIE_BOOT_GUARD_TEST
                } else if (strncmp(operation, "FAULT ", 6) == 0 && length == 14 + 6 + 64 &&
                           idle_valid && app_idle && strcmp(operation + 6, ota.running.elf) == 0 && roundie_boot_fault_arm()) {
                    fault = roundie_boot_fault_step(true);
                    error = fault ? ESP_OK : ESP_ERR_INVALID_STATE;
#endif
                } else {
                    error = ESP_ERR_INVALID_STATE;
                }
            }
        }
    }
    roundie_boot_status_t status;
    roundie_boot_status(&status);
    roundie_ota_status_t ota;
    roundie_ota_status(&ota);
    char version[sizeof(ota.running.version)];
    memcpy(version, ota.running.version, sizeof(version));
    version[sizeof(version) - 1] = '\0';
    for (size_t i = 0; version[i] != '\0'; i++) {
        char ch = version[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_' || ch == '+')) {
            version[i] = '_';
        }
    }
    char response[1024];
    int length_out = snprintf(response, sizeof(response),
        "\n!RS1 %s %d {\"safe\":%u,\"reason\":\"%s\",\"latch\":%lu,\"failures\":%lu,"
        "\"reset\":%lu,\"raw_reset\":%d,\"stage\":%lu,\"previous_stage\":%lu,"
        "\"retained\":%u,\"persistence\":%u,\"skip_gui\":%u,\"trial\":%u,\"fault_remaining\":%lu,"
        "\"test_build\":%u,\"version\":\"%s\",\"running\":\"%s\",\"ota_state\":\"%s\",\"elf\":\"%s\","
        "\"other\":\"%s\",\"other_state\":\"%s\",\"other_image\":%u,\"other_compatible\":%u,"
        "\"other_elf\":\"%s\",\"internal_free\":%zu,\"restart\":%u}\n",
        id, (int)error, status.safe, roundie_boot_reason_name(status.reason),
        (unsigned long)status.latch, (unsigned long)status.failures, (unsigned long)status.reset,
        (int)esp_reset_reason(), (unsigned long)status.stage, (unsigned long)status.previous_stage,
        status.retained_valid, status.persistence_ok, status.skip_gui, status.trial,
        (unsigned long)status.fault_remaining,
#if CONFIG_ROUNDIE_BOOT_GUARD_TEST
        1U,
#else
        0U,
#endif
        version, ota.running.label, roundie_ota_image_state_name(ota.running.state), ota.running.elf,
        ota.other.label, roundie_ota_image_state_name(ota.other.state), ota.other.image_valid,
        ota.other.compatible, ota.other.elf, heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        restart || fault);
    int written = -1;
    if (length_out > 0 && (size_t)length_out < sizeof(response)) {
        written = usb_serial_jtag_write_bytes(response, (size_t)length_out, pdMS_TO_TICKS(100));
    }
    if ((restart || fault) && written == length_out) {
        if (!fault) {
            roundie_boot_planned_restart();
        }
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
}
