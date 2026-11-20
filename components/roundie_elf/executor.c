#include "manager_private.h"
#include "idf/executor_stack.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdatomic.h>

typedef struct {
    uint8_t *bytes;
    size_t size;
} request_t;

static QueueHandle_t requests;
static TaskHandle_t task;
static atomic_bool input_owned;

/* Executor stack guard (M9.1): fail closed when it cannot be armed. */
enum { GUARD_PENDING, GUARD_ARMED, GUARD_FAILED };
static atomic_int guard_state;

bool roundie_elf_input_busy(void)
{
    return atomic_load(&input_owned);
}

static void execute(void *unused)
{
    (void)unused;
    roundie_elf_bind_executor(xTaskGetCurrentTaskHandle());
    bool guarded = roundie_elf_stack_arm();
    atomic_store(&guard_state, guarded ? GUARD_ARMED : GUARD_FAILED);
    if (!guarded) {
        ESP_LOGE("elf", "Executor stack guard unavailable; native applications are disabled");
    }
    uint8_t *retained = NULL;
    for (;;) {
        request_t request;
        if (xQueueReceive(requests, &request, pdMS_TO_TICKS(10)) == pdTRUE) {
            /* Idle task is unsubscribed. A non-returning native callback trips
             * the configured system watchdog; never delete/unload it forcibly. */
            if (guarded && esp_task_wdt_add(NULL) == ESP_OK) {
                roundie_elf_execute(request.bytes, request.size);
                esp_task_wdt_delete(NULL);
            } else {
                roundie_elf_cancel_prepare();
            }
            retained = request.bytes;
        }
        roundie_elf_status_t current;
        roundie_elf_status(&current);
        if ((current.state != ROUNDIE_ELF_UNLOADED ||
             current.control_submitted != current.control_completed) && esp_task_wdt_add(NULL) == ESP_OK) {
            roundie_elf_step(esp_timer_get_time());
            esp_task_wdt_delete(NULL);
        }
        roundie_elf_status(&current);
        if (current.state == ROUNDIE_ELF_UNLOADED && retained != NULL) {
            heap_caps_free(retained);
            retained = NULL;
            atomic_store(&input_owned, false);
        }
    }
}

static esp_err_t submit(uint8_t *bytes, size_t size, uint32_t abi_version)
{
    roundie_elf_info_t info;
    char error[ROUNDIE_ELF_ERROR_SIZE];
    if (!roundie_elf_validate(bytes, size, &info, error) ||
        (info.abi_version != abi_version && !(abi_version == 2 && info.abi_version == 3))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load(&guard_state) == GUARD_FAILED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&input_owned, &expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!roundie_elf_prepare()) {
        atomic_store(&input_owned, false);
        return ESP_ERR_INVALID_STATE;
    }
    if (requests == NULL) {
        requests = xQueueCreate(1, sizeof(request_t));
    }
    if (requests == NULL) {
        roundie_elf_cancel_prepare();
        atomic_store(&input_owned, false);
        return ESP_ERR_NO_MEM;
    }
    if (task == NULL && !roundie_elf_stack_create(execute, &task)) {
        vQueueDelete(requests);
        requests = NULL;
        roundie_elf_cancel_prepare();
        atomic_store(&input_owned, false);
        return ESP_ERR_NO_MEM;
    }
    request_t request = {bytes, size};
    if (xQueueSend(requests, &request, 0) != pdTRUE) {
        roundie_elf_cancel_prepare();
        atomic_store(&input_owned, false);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t roundie_elf_submit(uint8_t *bytes, size_t size)
{
    return submit(bytes, size, 1);
}

esp_err_t roundie_elf_start(uint8_t *bytes, size_t size)
{
    return submit(bytes, size, 2);
}
