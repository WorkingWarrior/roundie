#include "ota_diagnostics.h"

#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "roundie_ota_protocol.h"
#include "recovery_diagnostics.h"
#include "app_runtime.h"

static const char *TAG = "ota";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static roundie_ota_status_t snapshot;
static bool health_received;
static bool health_passed;
static int64_t verification_started_us;

static void publish(void)
{
    roundie_ota_status_t status;
    roundie_ota_status(&status);
    portENTER_CRITICAL(&lock);
    snapshot = status;
    portEXIT_CRITICAL(&lock);
}

static void log_status(void)
{
    roundie_ota_status_t status;
    roundie_ota_status(&status);
    ESP_LOGI(TAG, "slot=%s version=%s state=%s other=%s/%s image=%u rollback=%u self_test=%u/%u result=%s",
             status.running.label, status.running.version,
             roundie_ota_image_state_name(status.running.state), status.other.label,
             roundie_ota_image_state_name(status.other.state), status.other.image_valid,
             status.rollback_possible, status.boot_checked, status.boot_passed,
             esp_err_to_name(status.last_error));
    if (status.other.state == ESP_OTA_IMG_INVALID || status.other.state == ESP_OTA_IMG_ABORTED) {
        ESP_LOGW(TAG, "Previous slot is INVALID/ABORTED; exact reset cause is not persisted by OTA manager");
    }
}

void roundie_ota_diagnostics_init(void)
{
    esp_err_t error = roundie_ota_init();
    ESP_LOGI(TAG, "OTAManager initialization: %s", esp_err_to_name(error));
    publish();
    log_status();
}

void roundie_ota_startup_failed(void)
{
#if CONFIG_ROUNDIE_OTA_TEST_UNCONFIRMED_RESET
    esp_err_t error = roundie_ota_test_hold_unconfirmed(ESP_FAIL);
#else
    esp_err_t error = roundie_ota_boot_result(false);
#endif
    ESP_LOGE(TAG, "Critical startup failure: %s; no automatic confirmation or reboot retry",
             esp_err_to_name(error));
    publish();
}

void roundie_ota_gui_health(bool passed)
{
    portENTER_CRITICAL(&lock);
    if (!health_received) {
        health_passed = passed;
        health_received = true;
    }
    portEXIT_CRITICAL(&lock);
}

void roundie_ota_snapshot(roundie_ota_status_t *out)
{
    portENTER_CRITICAL(&lock);
    *out = snapshot;
    portEXIT_CRITICAL(&lock);
}

void roundie_ota_poll(int64_t now_us)
{
    roundie_ota_tick(now_us);
    roundie_ota_status_t status;
    roundie_ota_status(&status);
    if (status.initialized && !status.boot_checked) {
        if (verification_started_us == 0) {
            verification_started_us = now_us;
        }
        bool received;
        bool passed;
        portENTER_CRITICAL(&lock);
        received = health_received;
        passed = health_passed;
        portEXIT_CRITICAL(&lock);
        if (received || now_us - verification_started_us >= 5000000) {
#if CONFIG_ROUNDIE_OTA_TEST_UNCONFIRMED_RESET
            /* Compile-time arm, not a runtime command or persistent flag.
             * Never reset without a validated fallback and PENDING_VERIFY. */
            if (received && passed && roundie_ota_test_reset_allowed()) {
                ESP_LOGI(TAG, "ARMED core self-test PASS: PENDING_VERIFY with compatible VALID fallback; confirmation suppressed");
                ESP_LOGW(TAG, "ARMED DEVELOPMENT TEST: reset before confirmation; expect ABORTED + fallback");
                usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
            /* Never fall through to boot_result(false): that ordinary policy
             * can explicitly INVALIDATE/reboot and would be a different test.
             * Hold even if a mocked/unexpected restart call returns. */
            esp_err_t reason = received && passed ? ESP_ERR_OTA_ROLLBACK_FAILED : ESP_FAIL;
            esp_err_t error = roundie_ota_test_hold_unconfirmed(reason);
            ESP_LOGE(TAG, "ARMED reset refused/returned: reason=%s hold=%s; no confirmation or explicit rollback",
                     esp_err_to_name(reason), esp_err_to_name(error));
#else
            esp_err_t error = roundie_ota_boot_result(received && passed);
            ESP_LOGI(TAG, "Core self-test result: %s", esp_err_to_name(error));
#endif
            log_status();
        }
    }
    publish();
}

bool roundie_ota_restart_allowed(void)
{
#if CONFIG_ROUNDIE_APP_RUNTIME
    if (roundie_app_runtime_busy()) {
        return false;
    }
#endif
    roundie_ota_status_t status;
    roundie_ota_status(&status);
    if (!status.initialized) {
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
        return false; /* A broken REP layout must not bypass verification. */
#else
        return true; /* Historical legacy diagnostics have no OTA manager. */
#endif
    }
    return status.boot_checked && status.boot_passed &&
        (status.phase == ROUNDIE_OTA_IDLE ||
         (status.phase == ROUNDIE_OTA_ACTIVATION_PENDING && status.last_error == ESP_OK));
}

void roundie_ota_command(char *line, bool discard)
{
    roundie_ota_command_t command;
    esp_err_t error = roundie_ota_parse(line, discard, &command);
    bool restart = false;
    if (error == ESP_OK) {
#if CONFIG_ROUNDIE_APP_RUNTIME
        if (command.operation != ROUNDIE_OTA_STATUS && roundie_app_runtime_busy()) {
            error = ESP_ERR_INVALID_STATE;
        } else
#endif
        if (command.operation == ROUNDIE_OTA_RESTART) {
            restart = roundie_ota_restart_allowed() ||
                (roundie_recovery_active() && roundie_recovery_restart_allowed());
            error = restart ? ESP_OK : ESP_ERR_INVALID_STATE;
        } else {
            error = roundie_ota_execute(&command);
        }
    }
    if (error == ESP_OK && command.operation == ROUNDIE_OTA_END) {
        ESP_LOGI(TAG, "System worker minimum stack remaining: %u bytes",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    publish();
    roundie_ota_status_t status;
    roundie_ota_status(&status);
    /* Arbitrary image descriptor text must not break JSON framing. ELF hash is
     * the exact identity; version is a printable diagnostic representation. */
    for (size_t i = 0; status.running.version[i] != '\0'; i++) {
        char ch = status.running.version[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_' || ch == '+')) {
            status.running.version[i] = '_';
        }
    }
    char response[1024];
    int length = snprintf(response, sizeof(response),
        "\n!RO1 %s %d {\"phase\":\"%s\",\"session\":\"%08lx\",\"received\":%zu,\"total\":%zu,"
        "\"running\":\"%s\",\"address\":%lu,\"ota_state\":\"%s\","
        "\"other\":\"%s\",\"other_state\":\"%s\",\"other_image\":%u,"
        "\"other_elf\":\"%s\",\"boot\":\"%s\",\"version\":\"%s\",\"elf\":\"%s\","
        "\"digest\":\"%s\",\"rollback\":%u,\"checked\":%u,\"passed\":%u,\"last_error\":%d}\n",
        command.id, (int)error, roundie_ota_phase_name(status.phase),
        (unsigned long)status.session, status.received, status.total, status.running.label, (unsigned long)status.running.address,
        roundie_ota_image_state_name(status.running.state), status.other.label,
        roundie_ota_image_state_name(status.other.state), status.other.image_valid, status.other.elf,
        status.boot, status.running.version, status.running.elf, status.digest,
        status.rollback_possible, status.boot_checked, status.boot_passed, (int)status.last_error);
    int written = -1;
    if (length > 0 && (size_t)length < sizeof(response)) {
        written = usb_serial_jtag_write_bytes(response, (size_t)length, pdMS_TO_TICKS(100));
    }
    if (restart && written == length) {
        roundie_boot_planned_restart();
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
}
