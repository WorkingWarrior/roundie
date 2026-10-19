#include "manager_private.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_elf.h"
#include "private/elf_platform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rep/rep_app.h"

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static roundie_elf_status_t status;
static roundie_elf_status_t snapshot;
static atomic_bool allowed;
static esp_elf_t image;
static TaskHandle_t executor;
static struct {
    void *pointer;
    uint32_t bytes;
} allocations[4];

static roundie_elf_memory_t memory(void)
{
#if CONFIG_ELF_LOADER_LOAD_PSRAM
    const uint32_t code_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#elif defined(MALLOC_CAP_EXEC)
    const uint32_t code_caps = MALLOC_CAP_EXEC;
#else
    const uint32_t code_caps = MALLOC_CAP_8BIT | MALLOC_CAP_32BIT;
#endif
    return (roundie_elf_memory_t){
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        heap_caps_get_free_size(code_caps),
        heap_caps_get_largest_free_block(code_caps),
    };
}

void roundie_elf_status(roundie_elf_status_t *out)
{
    portENTER_CRITICAL(&lock);
    *out = snapshot;
    portEXIT_CRITICAL(&lock);
}

static void state(roundie_elf_state_t next)
{
    portENTER_CRITICAL(&lock);
    status.state = next;
    snapshot = status;
    portEXIT_CRITICAL(&lock);
}

bool roundie_elf_busy(void)
{
    roundie_elf_status_t current;
    roundie_elf_status(&current);
    return current.state != ROUNDIE_ELF_UNLOADED;
}

bool roundie_elf_prepare(void)
{
    portENTER_CRITICAL(&lock);
    if (!atomic_load(&allowed) || status.state != ROUNDIE_ELF_UNLOADED || status.instance == UINT32_MAX) {
        portEXIT_CRITICAL(&lock);
        return false;
    }
    uint32_t instance = status.instance + 1;
    uint32_t completed = status.completed;
    memset(&status, 0, sizeof(status));
    status.instance = instance;
    status.completed = completed;
    status.state = ROUNDIE_ELF_LOADING;
    snapshot = status;
    portEXIT_CRITICAL(&lock);
    return true;
}

void roundie_elf_cancel_prepare(void)
{
    portENTER_CRITICAL(&lock);
    if (status.state == ROUNDIE_ELF_LOADING) {
        snprintf(status.error, sizeof(status.error), "executor-start-failed");
        status.state = ROUNDIE_ELF_UNLOADED;
        snapshot = status;
    }
    portEXIT_CRITICAL(&lock);
}

void roundie_elf_set_allowed(bool value)
{
    atomic_store(&allowed, value);
}

void roundie_elf_bind_executor(TaskHandle_t task)
{
    executor = task;
}

static bool owner(uint32_t instance)
{
    return executor != NULL && xTaskGetCurrentTaskHandle() == executor && atomic_load(&allowed) &&
        instance == status.instance && (status.state == ROUNDIE_ELF_RUNNING || status.state == ROUNDIE_ELF_READY);
}

static bool loaded_range(const void *pointer, uint32_t bytes, bool code)
{
    uintptr_t address = (uintptr_t)pointer;
    for (unsigned i = 0; i < ELF_SECS; i++) {
        if ((i == ELF_SEC_TEXT) != code) {
            continue;
        }
        const esp_elf_sec_t *part = &image.sec[i];
        uintptr_t begin = part->addr;
#if CONFIG_ELF_LOADER_CACHE_OFFSET
        if (code) {
            begin = elf_remap_text(&image, begin);
        }
#endif
        if (part->size != 0 && address >= begin && address - begin <= part->size &&
            bytes <= part->size - (address - begin)) {
            return true;
        }
    }
    return false;
}

static bool owned_range(const void *pointer, uint32_t bytes)
{
    uintptr_t address = (uintptr_t)pointer;
    for (unsigned i = 0; i < 4; i++) {
        uintptr_t begin = (uintptr_t)allocations[i].pointer;
        if (begin != 0 && address >= begin && address - begin <= allocations[i].bytes &&
            bytes <= allocations[i].bytes - (address - begin)) {
            return true;
        }
    }
    return false;
}

static int32_t app_log(uint32_t instance, const char *text, uint32_t length)
{
    if (!owner(instance) || text == NULL || length > 160 ||
        (!loaded_range(text, length, false) && !owned_range(text, length))) {
        return -1;
    }
    status.api_calls++;
    ESP_LOGI("rep_app", "instance=%" PRIu32 " %.*s", instance, (int)length, text);
    return 0;
}

static uint64_t app_monotonic(uint32_t instance)
{
    if (!owner(instance)) {
        return 0;
    }
    status.api_calls++;
    uint64_t milliseconds = (uint64_t)esp_timer_get_time() / 1000U;
    ESP_LOGI("rep_app", "instance=%" PRIu32 " uptime_ms=%" PRIu64, instance, milliseconds);
    return milliseconds;
}

static void *app_alloc(uint32_t instance, uint32_t bytes)
{
    if (!owner(instance) || bytes == 0 || bytes > 4096U - status.alloc_bytes) {
        return NULL;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (allocations[i].pointer == NULL) {
            void *pointer = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (pointer != NULL) {
                allocations[i].pointer = pointer;
                allocations[i].bytes = bytes;
                status.alloc_bytes += bytes;
                if (status.alloc_bytes > status.alloc_peak) {
                    status.alloc_peak = status.alloc_bytes;
                }
                status.api_calls++;
            }
            return pointer;
        }
    }
    return NULL;
}

static int32_t app_release(uint32_t instance, void *pointer)
{
    if (!owner(instance) || pointer == NULL) {
        return -1;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (allocations[i].pointer == pointer) {
            heap_caps_free(pointer);
            allocations[i].pointer = NULL;
            status.alloc_bytes -= allocations[i].bytes;
            allocations[i].bytes = 0;
            status.api_calls++;
            return 0;
        }
    }
    return -1;
}

static const rep_api_t api = {
    REP_ABI_VERSION, sizeof(rep_api_t), REP_CAP_LOG | REP_CAP_MONOTONIC | REP_CAP_ALLOC,
    app_log, app_monotonic, app_alloc, app_release,
};

const rep_api_t *rep_abi_v1(uint32_t required_version)
{
    if (required_version != REP_ABI_VERSION || !owner(status.instance)) {
        return NULL;
    }
    status.api_calls++;
    return &api;
}

static uintptr_t resolver(const char *name)
{
    if (!roundie_elf_import_allowed(name)) {
        ESP_LOGE("elf", "Rejected import: %s", name != NULL ? name : "(null)");
        return 0;
    }
    return (uintptr_t)rep_abi_v1;
}

bool roundie_elf_execute(const uint8_t *bytes, size_t size)
{
    if (executor == NULL || xTaskGetCurrentTaskHandle() != executor ||
        status.state != ROUNDIE_ELF_LOADING) {
        return false;
    }
    /* Only state() publishes the executor-owned work record to readers. */
    roundie_elf_info_t info;
    int64_t started = esp_timer_get_time();
    status.before = memory();
    bool initialized = false;
    if (!atomic_load(&allowed)) {
        snprintf(status.error, sizeof(status.error), "system-policy");
        goto cleanup;
    }
    if (!roundie_elf_validate(bytes, size, &info, status.error)) {
        goto cleanup;
    }
    status.code_bytes = info.code_bytes;
    status.data_bytes = info.data_bytes;
    if (status.before.internal_free < 49152U ||
        status.before.psram_free < 65536U + info.code_bytes + info.data_bytes ||
        status.before.exec_largest < info.code_bytes) {
        snprintf(status.error, sizeof(status.error), "executable-memory-reserve");
        goto cleanup;
    }
    /* Permanent strict hook; never reset to upstream's broad default tables. */
    elf_set_symbol_resolver(resolver);
    status.loader_result = esp_elf_init(&image);
    if (status.loader_result != 0) {
        snprintf(status.error, sizeof(status.error), "loader-init");
        goto cleanup;
    }
    initialized = true;
    status.loader_result = esp_elf_relocate(&image, bytes);
    if (status.loader_result != 0) {
        snprintf(status.error, sizeof(status.error), "loader-relocation");
        goto cleanup;
    }
    status.relocated = memory();
    ESP_LOGI("elf", "instance=%" PRIu32 " text_write=%p text_execute=%p data=%p code=%" PRIu32
             " data_allocation=%" PRIu32 " relocations=%" PRIu32,
             status.instance, image.ptext, (void *)image.entry, image.pdata,
             info.code_bytes, info.data_bytes, info.relocations);
    status.load_us = (uint32_t)(esp_timer_get_time() - started);
    state(ROUNDIE_ELF_READY);
    if (!atomic_load(&allowed)) {
        snprintf(status.error, sizeof(status.error), "system-policy");
        goto cleanup;
    }
    typedef const rep_app_descriptor_t *(*entry_t)(const rep_api_t *);
    started = esp_timer_get_time();
    entry_t entry;
    _Static_assert(sizeof(entry) == sizeof(image.entry), "Loader entry pointer representation");
    memcpy(&entry, &image.entry, sizeof(entry));
    const rep_app_descriptor_t *descriptor = entry(&api);
    if (((uintptr_t)descriptor & 3U) != 0 || !loaded_range(descriptor, sizeof(*descriptor), false) ||
        descriptor->abi_version != REP_ABI_VERSION || descriptor->struct_size != sizeof(*descriptor) ||
        !loaded_range((const void *)descriptor->on_start, 3, true)) {
        snprintf(status.error, sizeof(status.error), "application-descriptor");
        goto cleanup;
    }
    rep_app_context_t context = {REP_ABI_VERSION, sizeof(context), status.instance, REP_EVENT_START};
    if (!atomic_load(&allowed)) {
        snprintf(status.error, sizeof(status.error), "system-policy");
        goto cleanup;
    }
    state(ROUNDIE_ELF_RUNNING);
    status.during = memory();
    status.executed = true;
    status.app_result = descriptor->on_start(&context);
    status.execute_us = (uint32_t)(esp_timer_get_time() - started);
    status.over_budget = status.execute_us > 100000;
cleanup:
    state(ROUNDIE_ELF_STOPPING);
    started = esp_timer_get_time();
    for (unsigned i = 0; i < 4; i++) {
        if (allocations[i].pointer != NULL) {
            heap_caps_free(allocations[i].pointer);
        }
        allocations[i].pointer = NULL;
        allocations[i].bytes = 0;
    }
    status.alloc_bytes = 0;
    state(ROUNDIE_ELF_UNLOADING);
    if (initialized) {
        esp_elf_deinit(&image);
    }
    status.after = memory();
    status.unload_us = (uint32_t)(esp_timer_get_time() - started);
    status.completed++;
    state(ROUNDIE_ELF_UNLOADED);
    return true;
}
