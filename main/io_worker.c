#include "io_worker.h"
#include "sdkconfig.h"

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "app_runtime.h"
#include "elf_diagnostics.h"
#include "face_diagnostics.h"
#include "package_diagnostics.h"
#include "roundie_boot_guard.h"
#include "roundie_bounded_io.h"
#include "roundie_launcher.h"
#include "roundie_storage_io.h"
#include "storage_import.h"
#include "time_protocol.h"

static_assert(ROUNDIE_IO_LINE_CAPACITY == ROUNDIE_TIME_LINE_CAPACITY,
              "a forwarded line must fit the framer's line");

static const char *TAG = "io_worker";

/* RP1 needs 12 KiB (the stack system_worker had for it). Priority and the
 * V0/V1 choice are measured in P1 step 4 (docs/storage-io.md). */
#define IO_WORKER_STACK 12288
#define IO_WORKER_PRIORITY 1
#define IO_QUEUE_LENGTH 2

static TaskHandle_t task;
static QueueHandle_t queue;
static SemaphoreHandle_t resume_signal;
static atomic_bool park_requested;
static atomic_bool parked;

static bool queue_try_send(const roundie_io_line_t *item)
{
    return queue != NULL && xQueueSend(queue, item, 0) == pdTRUE;
}

static void serial_write(const char *bytes, size_t length)
{
    usb_serial_jtag_write_bytes(bytes, length, pdMS_TO_TICKS(100));
}

static roundie_io_forwarder_t forwarder = {
    .try_send = queue_try_send,
    .write = serial_write,
};

void roundie_io_worker_forward(const char *line, bool discard)
{
    if (!roundie_io_forward(&forwarder, line, discard)) {
        ESP_LOGW(TAG, "Queue full: answered BUSY (%" PRIu32 " so far)", forwarder.busy);
    }
}

#if CONFIG_ROUNDIE_APP_RUNTIME
/* io_worker only: blocks while a park is requested. Called between jobs and,
 * through the bounded I/O hook, between two VFS calls of one transfer. */
static void park_point(void)
{
    if (xTaskGetCurrentTaskHandle() != task || !atomic_load(&park_requested)) {
        return;
    }
    atomic_store(&parked, true);
    while (atomic_load(&park_requested)) {
        xSemaphoreTake(resume_signal, pdMS_TO_TICKS(1000));
        esp_task_wdt_reset();
    }
    atomic_store(&parked, false);
}

#if CONFIG_ROUNDIE_DEV_ELF
/* Development builds: a bounded VFS call outside io_worker (after startup) or
 * under the claim mutex is a design error, not a runtime condition. */
static void storage_guard(void)
{
    roundie_storage_access_t access = roundie_storage_access_check();
    if (access != ROUNDIE_STORAGE_ACCESS_OK) {
        ESP_LOGE(TAG, "VFS call %s", access == ROUNDIE_STORAGE_ACCESS_UNDER_CLAIM ? "under the claim mutex" : "outside the owner");
        abort();
    }
}
#endif

#if CONFIG_ROUNDIE_DEV_ELF
static _Atomic int64_t stall_until_us;

/* Development hook (RS1 IOSTALL): io_worker serves nothing until the deadline
 * but keeps feeding its watchdog, so only the "no service" condition is
 * tested, not a watchdog reset. */
static void stall_point(void)
{
    while (esp_timer_get_time() < atomic_load(&stall_until_us)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        esp_task_wdt_reset();
    }
}
#endif

static void execute(roundie_io_line_t *item)
{
#if CONFIG_ROUNDIE_DEV_ELF
    if (strncmp(item->line, "!RP1 ", 5) == 0) {
        roundie_package_command(item->line, item->discard);
    } else if (strncmp(item->line, "!RF1 ", 5) == 0) {
        roundie_face_command(item->line, item->discard);
    } else if (strncmp(item->line, "!RE1 ", 5) == 0) {
        roundie_elf_command(item->line, item->discard);
    }
#else
    (void)item;
#endif
}

static void io_worker(void *argument)
{
    (void)argument;
    roundie_storage_owner_set();
    /* Same rule as system_worker: a stall resets into BootGuard instead of
     * silently removing packages, faces and the launcher. */
    esp_err_t watchdog_error = esp_task_wdt_add(NULL);
    if (watchdog_error != ESP_OK) {
        ESP_LOGE(TAG, "Watchdog: %s", esp_err_to_name(watchdog_error));
    }
    /* Home is the system WatchFaceHost (docs/watch-face-platform.md). The one
     * normal trial after Safe Mode starts on the list and shows only the
     * built-in face: no installed face may have caused the failure again. */
    roundie_boot_status_t boot_status;
    roundie_boot_status(&boot_status);
    roundie_app_runtime_faces_init(boot_status.trial);
    roundie_launcher_set_home_face(!boot_status.trial);
    static roundie_io_line_t item;
    for (;;) {
        if (watchdog_error == ESP_OK) {
            esp_task_wdt_reset();
        }
#if CONFIG_ROUNDIE_DEV_ELF
        stall_point();
#endif
        bool received = xQueueReceive(queue, &item, pdMS_TO_TICKS(10)) == pdTRUE;
        park_point();
        roundie_storage_claim_io_enter();
        if (received) {
            execute(&item);
        }
        roundie_app_runtime_poll(esp_timer_get_time());
        roundie_storage_import_poll(esp_timer_get_time());
        roundie_launcher_poll(roundie_app_runtime_allowed(), roundie_app_runtime_upload_busy());
        roundie_storage_claim_io_leave();
    }
}
#endif

void roundie_io_worker_stall(uint32_t stall_ms)
{
#if CONFIG_ROUNDIE_APP_RUNTIME && CONFIG_ROUNDIE_DEV_ELF
    atomic_store(&stall_until_us, esp_timer_get_time() + (int64_t)stall_ms * 1000);
    ESP_LOGW(TAG, "Development stall for %" PRIu32 " ms", stall_ms);
#else
    (void)stall_ms;
#endif
}

bool roundie_io_worker_park(int64_t timeout_us)
{
    if (task == NULL) {
        return true;
    }
    int64_t deadline = esp_timer_get_time() + timeout_us;
    atomic_store(&park_requested, true);
    while (!atomic_load(&parked)) {
        if (esp_timer_get_time() >= deadline) {
            roundie_io_worker_resume();
            ESP_LOGW(TAG, "Park timed out after %lld us", (long long)timeout_us);
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

void roundie_io_worker_resume(void)
{
    if (task == NULL) {
        return;
    }
    atomic_store(&park_requested, false);
    xSemaphoreGive(resume_signal);
}

esp_err_t roundie_io_worker_start(void)
{
#if CONFIG_ROUNDIE_APP_RUNTIME
    roundie_storage_claim_init();
    roundie_storage_owner_set(); /* the main task until io_worker takes over */
#if CONFIG_ROUNDIE_DEV_ELF
    roundie_io_set_access_guard(storage_guard);
#endif
    resume_signal = xSemaphoreCreateBinary();
    queue = xQueueCreate(IO_QUEUE_LENGTH, sizeof(roundie_io_line_t));
    if (resume_signal == NULL || queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    roundie_io_set_chunk_hook(park_point);
    if (xTaskCreate(io_worker, "io_worker", IO_WORKER_STACK, NULL, IO_WORKER_PRIORITY, &task) != pdPASS) {
        roundie_io_set_chunk_hook(NULL);
        vQueueDelete(queue);
        queue = NULL;
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}
