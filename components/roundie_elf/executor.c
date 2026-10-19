#include "manager_private.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/queue.h"

typedef struct {
    uint8_t *bytes;
    size_t size;
} request_t;

static QueueHandle_t requests;
static TaskHandle_t task;

static void execute(void *unused)
{
    (void)unused;
    roundie_elf_bind_executor(xTaskGetCurrentTaskHandle());
    for (;;) {
        request_t request;
        if (xQueueReceive(requests, &request, portMAX_DELAY) == pdTRUE) {
            /* Idle task is unsubscribed. A non-returning native callback trips
             * the configured system watchdog; never delete/unload it forcibly. */
            if (esp_task_wdt_add(NULL) == ESP_OK) {
                roundie_elf_execute(request.bytes, request.size);
                esp_task_wdt_delete(NULL);
            } else {
                roundie_elf_cancel_prepare();
            }
            heap_caps_free(request.bytes);
        }
    }
}

esp_err_t roundie_elf_submit(uint8_t *bytes, size_t size)
{
    if (bytes == NULL || size == 0 || size > ROUNDIE_ELF_MAX_FILE || !roundie_elf_prepare()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (requests == NULL) {
        requests = xQueueCreate(1, sizeof(request_t));
    }
    if (requests == NULL) {
        roundie_elf_cancel_prepare();
        return ESP_ERR_NO_MEM;
    }
    if (task == NULL && xTaskCreate(execute, "rep_executor", 12288, NULL, 1, &task) != pdPASS) {
        vQueueDelete(requests);
        requests = NULL;
        roundie_elf_cancel_prepare();
        return ESP_ERR_NO_MEM;
    }
    request_t request = {bytes, size};
    if (xQueueSend(requests, &request, 0) != pdTRUE) {
        roundie_elf_cancel_prepare();
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
