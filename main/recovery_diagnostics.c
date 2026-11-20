#include "recovery_diagnostics.h"

#include <stdatomic.h>
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
#include "roundie_storage_mode.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "roundie_elf_crash.h"
#if CONFIG_ROUNDIE_APP_RUNTIME
#include "app_runtime.h"
#include "roundie_elf.h"
static bool deferred_safe_restart;
#endif

/* HEALTH counters. The callback runs in the failing caller's context, so it
 * only updates atomics. */
static atomic_uint_least32_t failed_allocations;
static atomic_uint_least32_t failed_allocation_size;
static atomic_uint_least32_t failed_allocation_caps;

/* Tasks reported by name: firmware tasks and the IDF system tasks. A name
 * not found reports -1. */
static const char *const HEALTH_TASKS[] = {
    "main", "system_worker", "rep_executor", "audio", "esp_timer", "ipc0", "ipc1", "IDLE0", "IDLE1",
};

static void count_failed_allocation(size_t size, uint32_t caps, const char *function_name)
{
    (void)function_name;
    atomic_fetch_add(&failed_allocations, 1);
    atomic_store(&failed_allocation_size, (uint_least32_t)size);
    atomic_store(&failed_allocation_caps, caps);
}

void roundie_recovery_health_init(void)
{
    esp_err_t error = heap_caps_register_failed_alloc_callback(count_failed_allocation);
    if (error != ESP_OK) {
        ESP_LOGW("recovery", "Failed-allocation counter not registered: %s", esp_err_to_name(error));
    }
}

/* Appends the HEALTH fields to a status record that ends without its closing
 * brace. Returns the new length, or -1 if they do not fit. */
static int append_health(char *response, size_t capacity, int length)
{
    if (length < 0 || (size_t)length >= capacity) {
        return -1;
    }
    int added = snprintf(response + length, capacity - (size_t)length,
        ",\"uptime_us\":%lld,\"internal_min\":%zu,\"internal_largest\":%zu,"
        "\"psram_free\":%zu,\"psram_min\":%zu,\"psram_largest\":%zu,"
        "\"alloc_failed\":%lu,\"alloc_failed_size\":%lu,\"alloc_failed_caps\":%lu,"
        "\"tasks\":%lu,\"stacks\":{",
        (long long)esp_timer_get_time(),
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        (unsigned long)atomic_load(&failed_allocations),
        (unsigned long)atomic_load(&failed_allocation_size),
        (unsigned long)atomic_load(&failed_allocation_caps),
        (unsigned long)uxTaskGetNumberOfTasks());
    if (added < 0 || (size_t)added >= capacity - (size_t)length) {
        return -1;
    }
    length += added;
    for (size_t i = 0; i < sizeof(HEALTH_TASKS) / sizeof(HEALTH_TASKS[0]); i++) {
        TaskHandle_t task = xTaskGetHandle(HEALTH_TASKS[i]);
        long high_water = -1;
        if (task != NULL) {
            high_water = (long)uxTaskGetStackHighWaterMark(task);
        }
        added = snprintf(response + length, capacity - (size_t)length, "%s\"%s\":%ld",
                         i == 0 ? "" : ",", HEALTH_TASKS[i], high_water);
        if (added < 0 || (size_t)added >= capacity - (size_t)length) {
            return -1;
        }
        length += added;
    }
    added = snprintf(response + length, capacity - (size_t)length, "}");
    if (added < 0 || (size_t)added >= capacity - (size_t)length) {
        return -1;
    }
    return length + added;
}

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

/* RS1 CRASH (M9, read-only): the retained native crash record found at boot,
 * in its own frame so the STATUS/HEALTH frame keeps its margin. Addresses are
 * run-time addresses; tools/roundie_crash.py maps them to the ELF. */
static void crash_reply(const char *id)
{
    roundie_elf_crash_report_t report;
    char response[768];
    int length;
    if (!roundie_elf_crash_last(&report)) {
        length = snprintf(response, sizeof(response), "\n!RS1 %s 0 {\"crash\":{\"valid\":0}}\n", id);
    } else {
        const roundie_elf_crash_context_t *context = &report.context;
        const roundie_elf_crash_fault_t *fault = &report.details;
        char digest[65];
        for (unsigned i = 0; i < 32; i++) {
            snprintf(digest + i * 2, 3, "%02x", context->digest[i]);
        }
        length = snprintf(response, sizeof(response),
            "\n!RS1 %s 0 {\"crash\":{\"valid\":1,\"loaded\":%u,\"fault\":%u,\"instance\":%lu,"
            "\"digest\":\"%s\",\"text_exec\":%lu,\"sections\":[",
            id, report.loaded, report.fault, (unsigned long)context->instance, digest,
            (unsigned long)context->text_exec);
        for (unsigned i = 0; i < ROUNDIE_ELF_CRASH_SECTIONS && length > 0 && (size_t)length < sizeof(response); i++) {
            const roundie_elf_crash_section_t *section = &context->sections[i];
            length += snprintf(response + length, sizeof(response) - (size_t)length, "%s[%lu,%lu,%lu]",
                               i == 0 ? "" : ",", (unsigned long)section->vaddr,
                               (unsigned long)section->addr, (unsigned long)section->size);
        }
        if (length > 0 && (size_t)length < sizeof(response)) {
            length += snprintf(response + length, sizeof(response) - (size_t)length,
                "],\"pc\":%lu,\"cause\":%lu,\"vaddr\":%lu,\"sp\":%lu,\"exception\":%lu,\"core\":%lu,\"frames\":[",
                (unsigned long)fault->pc, (unsigned long)fault->exccause, (unsigned long)fault->excvaddr,
                (unsigned long)fault->sp, (unsigned long)fault->exception, (unsigned long)fault->core);
        }
        for (unsigned i = 0; i < fault->depth && length > 0 && (size_t)length < sizeof(response); i++) {
            length += snprintf(response + length, sizeof(response) - (size_t)length, "%s%lu",
                               i == 0 ? "" : ",", (unsigned long)fault->frames[i]);
        }
        if (length > 0 && (size_t)length < sizeof(response)) {
            length += snprintf(response + length, sizeof(response) - (size_t)length, "]}}\n");
        }
    }
    if (length > 0 && (size_t)length < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)length, pdMS_TO_TICKS(100));
    }
}

void roundie_recovery_command(char *line, bool discard)
{
    char id[9] = "00000000";
    esp_err_t error = ESP_ERR_INVALID_ARG;
    bool restart = false;
    bool fault = false;
    bool hang = false;
    bool health = false;
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
                } else if (strcmp(operation, "CRASH") == 0) {
                    crash_reply(id);
                    return;
                } else if (strcmp(operation, "HEALTH") == 0) {
                    /* Read-only: STATUS plus memory, task and stack figures. */
                    health = true;
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
                } else if (strcmp(operation, "STORAGE") == 0) {
                    /* One-shot request for USB storage mode; the restart below is planned. */
                    roundie_boot_status_t boot_now;
                    roundie_boot_status(&boot_now);
                    const roundie_storage_conditions_t conditions = {
                        .storage_mode = roundie_storage_mode_active(),
                        .safe = boot_now.safe,
                        .stable = boot_now.stage == ROUNDIE_BOOT_STABLE,
                        .ota_idle = ota.initialized && ota.phase == ROUNDIE_OTA_IDLE,
                        .image_valid = ota.initialized && ota.running.state == ESP_OTA_IMG_VALID,
                        .app_busy = !app_idle,
                    };
                    roundie_storage_refusal_t refusal = roundie_storage_mode_refusal(&conditions);
                    if (refusal == ROUNDIE_STORAGE_ALLOWED) {
                        roundie_storage_mode_request(esp_app_get_description()->app_elf_sha256);
                        error = ESP_OK;
                        restart = true;
                    } else {
                        ESP_LOGW("recovery", "Storage mode refused: %s", roundie_storage_refusal_name(refusal));
                        error = ESP_ERR_INVALID_STATE;
                    }
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
                } else if (strncmp(operation, "HANG ", 5) == 0 && length == 14 + 5 + 64 &&
                           strcmp(operation + 5, ota.running.elf) == 0) {
                    hang = true;
                    error = ESP_OK;
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
        "\"other_elf\":\"%s\",\"internal_free\":%zu,\"restart\":%u,\"fault_app\":\"%s\",\"storage\":%u",
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
        restart || fault || hang, status.fault_app, roundie_storage_mode_active());
    if (health) {
        length_out = append_health(response, sizeof(response), length_out);
    }
    if (length_out >= 0 && (size_t)length_out + 2 < sizeof(response)) {
        response[length_out] = '}';
        response[length_out + 1] = '\n';
        response[length_out + 2] = '\0';
        length_out += 2;
    } else {
        length_out = -1;
    }
    int written = -1;
    if (length_out > 0 && (size_t)length_out < sizeof(response)) {
        written = usb_serial_jtag_write_bytes(response, (size_t)length_out, pdMS_TO_TICKS(100));
    }
    if (hang && written == length_out) {
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
        /* Keeps the scheduler running and stops only this worker's watchdog
         * feed: a real task watchdog panic, bounded by its timeout. */
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
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
