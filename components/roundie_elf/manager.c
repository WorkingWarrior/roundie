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
#include "rep/rep_app_v2.h"
#include "roundie_app_ui.h"

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static roundie_elf_status_t status;
static roundie_elf_status_t snapshot;
static atomic_bool allowed;
static esp_elf_t image;
static TaskHandle_t executor;
static bool callback_active;
static bool loader_initialized;
static bool lifecycle_initialized;
static bool lifecycle_started;
static bool lifecycle_stopped;
static rep_descriptor_v2_t lifecycle;
static bool ui_owned;
static bool round_ui; /* descriptor required REP3_CAP_ROUND_UI */
static uint32_t timer_serial;
static uint32_t timer_period_ms;
static int64_t timer_due_us;
/* Producers touch only this mailbox under lock. Lifecycle control has a
 * reserved slot independent of the ordinary FIFO. No callback runs locked. */
static struct {
    rep_event_v2_t events[ROUNDIE_ELF_EVENT_CAPACITY];
    uint32_t head;
    uint32_t count;
    uint32_t acquired;
    uint32_t released;
    uint32_t dropped;
    uint32_t coalesced;
    uint32_t rejected;
    uint32_t submitted;
    uint32_t completed;
    int32_t result;
    roundie_elf_control_t control;
    bool shutdown;
    bool admitting;
} mailbox;
static struct {
    void *pointer;
    uint32_t bytes;
    uint32_t generation;
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
    out->queue_depth = mailbox.count;
    out->events_dropped = mailbox.dropped;
    out->events_coalesced = mailbox.coalesced;
    out->events_rejected = mailbox.rejected;
    out->resources_acquired += mailbox.acquired;
    out->resources_released += mailbox.released;
    out->resources_outstanding = out->resources_acquired - out->resources_released - out->resources_reclaimed;
    out->control_submitted = mailbox.submitted;
    out->control_completed = mailbox.completed;
    out->control_result = mailbox.result;
    portEXIT_CRITICAL(&lock);
    roundie_ui_status_t ui;
    roundie_ui_status(&ui);
    if (out->abi_version == 3) {
        out->resources_acquired += ui.acquired;
        out->resources_released += ui.released;
        out->resources_outstanding += ui.outstanding;
    }
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
    return current.state != ROUNDIE_ELF_UNLOADED || roundie_elf_input_busy();
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
    memset(&mailbox, 0, sizeof(mailbox));
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
    portENTER_CRITICAL(&lock);
    atomic_store(&allowed, value);
    if (!value) {
        mailbox.admitting = false;
        mailbox.shutdown = true;
    }
    portEXIT_CRITICAL(&lock);
    if (!value) {
        roundie_ui_status_t ui;
        roundie_ui_status(&ui);
        roundie_ui_close_admission(ui.generation);
    }
}

void roundie_elf_bind_executor(TaskHandle_t task)
{
    executor = task;
}

static bool owner(uint32_t instance)
{
    return executor != NULL && xTaskGetCurrentTaskHandle() == executor && callback_active &&
        instance == status.instance;
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
        if (allocations[i].generation == status.instance && begin != 0 &&
            address >= begin && address - begin <= allocations[i].bytes &&
            bytes <= allocations[i].bytes - (address - begin)) {
            return true;
        }
    }
    return false;
}

static bool writable_range(const void *pointer, uint32_t bytes)
{
    uintptr_t address = (uintptr_t)pointer;
    for (unsigned i = 0; i < ELF_SECS; i++) {
        if (i == ELF_SEC_TEXT || i == ELF_SEC_RODATA) {
            continue;
        }
        const esp_elf_sec_t *part = &image.sec[i];
        if (part->size != 0 && address >= part->addr && address - part->addr <= part->size &&
            bytes <= part->size - (address - part->addr)) {
            return true;
        }
    }
    return owned_range(pointer, bytes);
}

static bool ui_argument(uint32_t instance, const void *pointer, uint32_t bytes)
{
    return owner(instance) && status.abi_version == REP_ABI_V3 && atomic_load(&allowed) &&
        (status.state == ROUNDIE_ELF_READY || status.state == ROUNDIE_ELF_STARTING ||
         status.state == ROUNDIE_ELF_RUNNING) && pointer != NULL &&
        ((uintptr_t)pointer & 3U) == 0 &&
        (loaded_range(pointer, bytes, false) || owned_range(pointer, bytes));
}

/* Version 2 extends version 1 by trailing fields; the prefix is checked first. */
static uint32_t app_ui_create(uint32_t instance, const rep_ui_widget_t *widget)
{
    if (!ui_argument(instance, widget, sizeof(*widget))) {
        return 0;
    }
    rep_ui_widget_v2_t copy = {0};
    if (widget->version == 2) {
        if (!round_ui || !ui_argument(instance, widget, sizeof(copy))) {
            return 0;
        }
        memcpy(&copy, widget, sizeof(copy));
    } else {
        copy.base = *widget;
    }
    status.api_calls++;
    return roundie_ui_create(instance, &copy);
}

static int32_t app_ui_update(uint32_t instance, const rep_ui_update_t *update)
{
    if (!ui_argument(instance, update, sizeof(*update))) {
        return -1;
    }
    rep_ui_update_t copy = *update;
    status.api_calls++;
    return roundie_ui_update(instance, &copy);
}

static int32_t app_ui_destroy(uint32_t instance, uint32_t view)
{
    if (!owner(instance) || status.abi_version != REP_ABI_V3 || !atomic_load(&allowed) ||
        status.state != ROUNDIE_ELF_RUNNING) {
        return -1;
    }
    status.api_calls++;
    return roundie_ui_destroy(instance, view);
}

static int32_t app_ui_result(uint32_t instance, rep_ui_result_t *result)
{
    if (!owner(instance) || status.abi_version != REP_ABI_V3 || result == NULL ||
        ((uintptr_t)result & 3U) != 0 || !writable_range(result, sizeof(*result))) {
        return -1;
    }
    /* Version 2 additionally reads one widget's shadow state. */
    bool shadow = result->version == 2;
    uint32_t widget = 0;
    int32_t committed = 0;
    int32_t requested = 0;
    uint32_t pending = 0;
    if (shadow) {
        rep_ui_result_v2_t *extended = (rep_ui_result_v2_t *)result;
        if (!round_ui || result->struct_size != sizeof(*extended) ||
            !writable_range(result, sizeof(*extended))) {
            return -1;
        }
        widget = extended->widget;
        if (widget != 0 && !roundie_ui_shadow(instance, widget, &committed, &requested, &pending)) {
            return -1;
        }
    }
    roundie_ui_status_t ui;
    roundie_ui_status(&ui);
    *result = (rep_ui_result_t){shadow ? 2 : 1, shadow ? sizeof(rep_ui_result_v2_t) : sizeof(*result),
        ui.submitted, ui.applied, ui.failed, ui.pending, ui.error, ui.view};
    if (shadow) {
        rep_ui_result_v2_t *extended = (rep_ui_result_v2_t *)result;
        extended->widget = widget;
        extended->committed = committed;
        extended->requested = requested;
        extended->pending = pending;
    }
    return 0;
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
    if (!owner(instance) || !atomic_load(&allowed) || status.state == ROUNDIE_ELF_STOPPING ||
        status.state == ROUNDIE_ELF_UNLOADING || bytes == 0 || bytes > 4096U - status.alloc_bytes) {
        return NULL;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (allocations[i].pointer == NULL) {
            void *pointer = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (pointer != NULL) {
                allocations[i].pointer = pointer;
                allocations[i].bytes = bytes;
                allocations[i].generation = instance;
                status.alloc_bytes += bytes;
                if (status.alloc_bytes > status.alloc_peak) {
                    status.alloc_peak = status.alloc_bytes;
                }
                status.api_calls++;
                status.resources_acquired++;
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
        if (allocations[i].pointer == pointer && allocations[i].generation == instance) {
            heap_caps_free(pointer);
            allocations[i].pointer = NULL;
            status.alloc_bytes -= allocations[i].bytes;
            allocations[i].bytes = 0;
            allocations[i].generation = 0;
            status.api_calls++;
            status.resources_released++;
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
    if (required_version != REP_ABI_VERSION || status.abi_version != 1 || !owner(status.instance)) {
        return NULL;
    }
    status.api_calls++;
    return &api;
}

static uint32_t app_timer_start(uint32_t instance, uint32_t period_ms)
{
    if (!owner(instance) || !atomic_load(&allowed) || status.timer_handle != 0 ||
        status.state == ROUNDIE_ELF_STOPPING || status.state == ROUNDIE_ELF_UNLOADING ||
        period_ms < 50 || period_ms > 10000 || timer_serial == UINT32_MAX) {
        return 0;
    }
    status.timer_handle = ++timer_serial;
    timer_period_ms = period_ms;
    timer_due_us = esp_timer_get_time() + (int64_t)period_ms * 1000;
    status.resources_acquired++;
    return status.timer_handle;
}

static int32_t app_timer_cancel(uint32_t instance, uint32_t handle)
{
    if (!owner(instance) || handle == 0 || status.timer_handle != handle) {
        return -1;
    }
    status.timer_handle = 0;
    timer_period_ms = 0;
    timer_due_us = 0;
    status.resources_released++;
    portENTER_CRITICAL(&lock);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < mailbox.count; i++) {
        rep_event_v2_t event = mailbox.events[(mailbox.head + i) % ROUNDIE_ELF_EVENT_CAPACITY];
        if (event.type == REP2_EVENT_TIMER && event.value == handle) {
            mailbox.released++;
        } else {
            mailbox.events[(mailbox.head + kept) % ROUNDIE_ELF_EVENT_CAPACITY] = event;
            kept++;
        }
    }
    mailbox.count = kept;
    portEXIT_CRITICAL(&lock);
    return 0;
}

static const rep_api_v2_t api_v2 = {
    REP_ABI_V2, sizeof(rep_api_v2_t), REP_CAP_LOG | REP_CAP_MONOTONIC | REP_CAP_ALLOC | REP2_CAP_TIMER,
    app_log, app_monotonic, app_alloc, app_release, app_timer_start, app_timer_cancel,
};

const rep_api_v2_t *rep_abi_v2(uint32_t required_version)
{
    if (required_version != REP_ABI_V2 || status.abi_version != 2 || !owner(status.instance)) {
        return NULL;
    }
    status.api_calls++;
    return &api_v2;
}

static const rep_api_v3_t api_v3 = {
    REP_ABI_V3, sizeof(rep_api_v3_t), REP_CAP_LOG | REP_CAP_MONOTONIC | REP_CAP_ALLOC |
        REP2_CAP_TIMER | REP3_CAP_UI | REP3_CAP_ROUND_UI,
    app_log, app_monotonic, app_alloc, app_release, app_timer_start, app_timer_cancel,
    app_ui_create, app_ui_update, app_ui_destroy, app_ui_result,
};

const rep_api_v3_t *rep_abi_v3(uint32_t required_version)
{
    if (required_version != REP_ABI_V3 || status.abi_version != REP_ABI_V3 || !owner(status.instance)) {
        return NULL;
    }
    status.api_calls++;
    return &api_v3;
}

static uintptr_t resolver(const char *name)
{
    if (!roundie_elf_import_allowed(name)) {
        ESP_LOGE("elf", "Rejected import: %s", name != NULL ? name : "(null)");
        return 0;
    }
    if (strcmp(name, "rep_abi_v1") == 0 && status.abi_version == 1) {
        return (uintptr_t)rep_abi_v1;
    }
    if (strcmp(name, "rep_abi_v2") == 0 && status.abi_version == 2) {
        return (uintptr_t)rep_abi_v2;
    }
    return strcmp(name, "rep_abi_v3") == 0 && status.abi_version == 3 ? (uintptr_t)rep_abi_v3 : 0;
}

static void callback_finished(int64_t started)
{
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
    status.callbacks++;
    status.execute_us = elapsed;
    if (elapsed > status.callback_max_us) {
        status.callback_max_us = elapsed;
    }
    if (elapsed > 100000) {
        status.over_budget = true;
        ESP_LOGW("elf", "Callback exceeded 100 ms: %" PRIu32 " us", elapsed);
    }
    callback_active = false;
}

static int32_t invoke(rep_callback_v2_t callback)
{
    rep_context_v2_t context = {status.abi_version, sizeof(context), status.instance, 0};
    int64_t started = esp_timer_get_time();
    callback_active = true;
    int32_t result = callback(&context);
    callback_finished(started);
    if (status.app_result == 0) {
        status.app_result = result;
    }
    return result;
}

static void clear_events(void)
{
    portENTER_CRITICAL(&lock);
    mailbox.admitting = false;
    mailbox.released += mailbox.count;
    mailbox.count = 0;
    mailbox.head = 0;
    portEXIT_CRITICAL(&lock);
}

static bool stop_lifecycle(void)
{
    if (!lifecycle_stopped) {
        int64_t started = esp_timer_get_time();
        clear_events();
        if (status.timer_handle != 0) {
            status.timer_handle = 0;
            timer_period_ms = 0;
            timer_due_us = 0;
            status.resources_reclaimed++;
        }
        if (status.subscriptions != 0) {
            status.subscriptions = 0;
            status.resources_released++;
        }
        state(ROUNDIE_ELF_STOPPING);
        if (ui_owned) {
            roundie_ui_close_admission(status.instance);
        }
        if (lifecycle_started && invoke(lifecycle.stop) != 0 && status.error[0] == '\0') {
            snprintf(status.error, sizeof(status.error), "callback-stop");
        }
        lifecycle_stopped = true;
        status.shutdown_us = (uint32_t)(esp_timer_get_time() - started);
    }
    if (ui_owned && !roundie_ui_transition(status.instance, ROUNDIE_UI_DESTROY)) {
        snprintf(status.error, sizeof(status.error), "ui-teardown-pending");
        state(ROUNDIE_ELF_STOPPING);
        return false;
    }
    state(ROUNDIE_ELF_STOPPED);
    return true;
}

static void unload(void)
{
    clear_events();
    if (status.abi_version >= 2 && !stop_lifecycle()) {
        return;
    }
    int64_t started = esp_timer_get_time();
    state(ROUNDIE_ELF_UNLOADING);
    if (lifecycle_initialized) {
        if (invoke(lifecycle.destroy) != 0 && status.error[0] == '\0') {
            snprintf(status.error, sizeof(status.error), "callback-destroy");
        }
    }
    lifecycle_initialized = false;
    if (ui_owned) {
        if (!roundie_ui_retire(status.instance)) {
            snprintf(status.error, sizeof(status.error), "ui-retirement-pending");
            state(ROUNDIE_ELF_UNLOADING);
            return;
        }
        ui_owned = false;
    }
    lifecycle_started = false;
    memset(&lifecycle, 0, sizeof(lifecycle));
    for (unsigned i = 0; i < 4; i++) {
        if (allocations[i].pointer != NULL) {
            heap_caps_free(allocations[i].pointer);
            status.resources_reclaimed++;
        }
        allocations[i].pointer = NULL;
        allocations[i].bytes = 0;
        allocations[i].generation = 0;
    }
    status.alloc_bytes = 0;
    if (loader_initialized) {
        esp_elf_deinit(&image);
        loader_initialized = false;
        status.resources_released++;
    }
    /* Per-instance record and copied callback references retire after destroy. */
    status.resources_released += 2;
    status.after = memory();
    status.unload_us = (uint32_t)(esp_timer_get_time() - started);
    status.completed++;
    status.completed_us = esp_timer_get_time();
    state(ROUNDIE_ELF_UNLOADED);
}

esp_err_t roundie_elf_control(uint32_t instance, roundie_elf_control_t operation)
{
    portENTER_CRITICAL(&lock);
    roundie_elf_state_t current = snapshot.state;
    bool valid = instance != 0 && instance == snapshot.instance && snapshot.abi_version >= 2 &&
        operation >= ROUNDIE_ELF_SUSPEND && operation <= ROUNDIE_ELF_UNLOAD &&
        mailbox.submitted != UINT32_MAX && mailbox.submitted == mailbox.completed;
    valid = valid && ((operation == ROUNDIE_ELF_SUSPEND && current == ROUNDIE_ELF_RUNNING) ||
        (operation == ROUNDIE_ELF_RESUME && current == ROUNDIE_ELF_SUSPENDED) ||
        ((operation == ROUNDIE_ELF_STOP || operation == ROUNDIE_ELF_UNLOAD) &&
         (current == ROUNDIE_ELF_RUNNING || current == ROUNDIE_ELF_SUSPENDED ||
          current == ROUNDIE_ELF_STOPPED || current == ROUNDIE_ELF_UNLOADED)));
    if (valid) {
        mailbox.control = operation;
        mailbox.submitted++;
        mailbox.admitting = false;
    }
    portEXIT_CRITICAL(&lock);
    if (valid) {
        roundie_ui_close_admission(instance);
    }
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* A single copied 32-bit payload. Generation is checked under the same lock as
 * admission; queue contents can only belong to the current loaded instance. */
/* coalesce: an undelivered VALUE event of the same widget takes the newer
 * payload in place; it keeps its queue position and original post time. */
static esp_err_t post_with_origin(uint32_t instance, uint32_t type, uint32_t value,
                                  uint32_t origin, bool coalesce, int64_t posted_us)
{
    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&lock);
    if (!atomic_load(&allowed) || !mailbox.admitting || instance != snapshot.instance ||
        snapshot.abi_version < 2 || snapshot.state != ROUNDIE_ELF_RUNNING) {
        mailbox.rejected++;
        result = ESP_ERR_INVALID_STATE;
    } else {
        bool duplicate = false;
        for (uint32_t i = 0; i < mailbox.count; i++) {
            rep_event_v2_t *queued = &mailbox.events[(mailbox.head + i) % ROUNDIE_ELF_EVENT_CAPACITY];
            if (type == REP2_EVENT_TIMER && queued->type == type) {
                duplicate = true;
            }
            if (coalesce && type == REP3_EVENT_VALUE && queued->type == type && queued->value == value) {
                queued->reserved = origin;
                duplicate = true;
            }
        }
        if (duplicate) {
            mailbox.coalesced++;
        } else if (mailbox.count == ROUNDIE_ELF_EVENT_CAPACITY) {
            mailbox.dropped++;
            result = ESP_ERR_NO_MEM;
        } else {
            mailbox.events[(mailbox.head + mailbox.count) % ROUNDIE_ELF_EVENT_CAPACITY] =
                (rep_event_v2_t){type, sizeof(rep_event_v2_t), value, origin, (uint64_t)posted_us};
            mailbox.count++;
            mailbox.acquired++;
        }
    }
    portEXIT_CRITICAL(&lock);
    return result;
}

static esp_err_t post_event(uint32_t instance, uint32_t type, uint32_t value, int64_t posted_us)
{
    return post_with_origin(instance, type, value, 0, false, posted_us);
}

static bool ui_input(uint32_t instance, uint32_t type, uint32_t widget, uint32_t payload,
                     bool coalesce, int64_t posted_us)
{
    return post_with_origin(instance, type, widget, payload, coalesce, posted_us) == ESP_OK;
}

esp_err_t roundie_elf_post(uint32_t instance, uint32_t value)
{
    return post_event(instance, REP2_EVENT_USER, value, esp_timer_get_time());
}

void roundie_elf_step(int64_t now_us)
{
    if (executor == NULL || xTaskGetCurrentTaskHandle() != executor || callback_active) {
        return;
    }
    portENTER_CRITICAL(&lock);
    bool shutdown = mailbox.shutdown || !atomic_load(&allowed);
    roundie_elf_control_t control = mailbox.control;
    mailbox.control = 0;
    uint32_t serial = mailbox.submitted;
    portEXIT_CRITICAL(&lock);
    int32_t result = 0;
    roundie_ui_status_t ui;
    roundie_ui_status(&ui);
    if ((ui_owned && ui.error != 0) || status.state == ROUNDIE_ELF_STOPPING ||
        status.state == ROUNDIE_ELF_UNLOADING) {
        if (status.error[0] == '\0') {
            snprintf(status.error, sizeof(status.error), "ui-gui-failure");
        }
        unload();
        result = -1;
    } else if (shutdown && status.state != ROUNDIE_ELF_UNLOADED && status.state != ROUNDIE_ELF_LOADING) {
        unload();
        result = -1;
    } else if (control == ROUNDIE_ELF_UNLOAD && status.state != ROUNDIE_ELF_UNLOADED) {
        unload();
    } else if (control == ROUNDIE_ELF_STOP && status.state != ROUNDIE_ELF_UNLOADED) {
        stop_lifecycle();
    } else if (control == ROUNDIE_ELF_SUSPEND) {
        state(ROUNDIE_ELF_SUSPENDING);
        int64_t started = esp_timer_get_time();
        result = invoke(lifecycle.suspend);
        status.suspend_us = (uint32_t)(esp_timer_get_time() - started);
        /* Already queued ordinary work is canceled, never replayed on resume. */
        clear_events();
        if (ui_owned && !roundie_ui_transition(status.instance, ROUNDIE_UI_HIDE)) {
            result = -1;
        }
        state(ROUNDIE_ELF_SUSPENDED);
    } else if (control == ROUNDIE_ELF_RESUME) {
        state(ROUNDIE_ELF_RESUMING);
        int64_t started = esp_timer_get_time();
        result = invoke(lifecycle.resume);
        if (result == 0 && ui_owned && !roundie_ui_transition(status.instance, ROUNDIE_UI_SHOW)) {
            result = -1;
        }
        status.resume_us = (uint32_t)(esp_timer_get_time() - started);
        timer_due_us = now_us + (int64_t)timer_period_ms * 1000;
        state(ROUNDIE_ELF_RUNNING);
    }
    if (control != 0) {
        if ((result != 0 || status.error[0] != '\0') && status.state != ROUNDIE_ELF_UNLOADED) {
            if (status.error[0] == '\0') {
                snprintf(status.error, sizeof(status.error), "callback-control");
            }
            unload();
        }
        if (status.error[0] != '\0') {
            result = -1;
        }
        portENTER_CRITICAL(&lock);
        mailbox.result = result;
        mailbox.completed = serial;
        mailbox.admitting = atomic_load(&allowed) && status.state == ROUNDIE_ELF_RUNNING;
        portEXIT_CRITICAL(&lock);
        state(status.state);
        return;
    }
    if (status.state != ROUNDIE_ELF_RUNNING || status.abi_version < 2 || shutdown) {
        return;
    }
    if (status.timer_handle != 0 && now_us >= timer_due_us) {
        post_event(status.instance, REP2_EVENT_TIMER, status.timer_handle, now_us);
        timer_due_us = now_us + (int64_t)timer_period_ms * 1000;
    }
    rep_event_v2_t event;
    bool dispatch = false;
    portENTER_CRITICAL(&lock);
    /* Callback admission is linearized here against shutdown/control requests. */
    if (mailbox.admitting && atomic_load(&allowed) && mailbox.count != 0) {
        event = mailbox.events[mailbox.head];
        mailbox.head = (mailbox.head + 1) % ROUNDIE_ELF_EVENT_CAPACITY;
        mailbox.count--;
        mailbox.released++;
        dispatch = true;
    }
    portEXIT_CRITICAL(&lock);
    if (dispatch) {
        bool ui_event = event.type == REP3_EVENT_BUTTON || event.type == REP3_EVENT_VALUE;
        if (ui_event && !roundie_ui_event_valid(status.instance, event.type, event.value, event.reserved)) {
            portENTER_CRITICAL(&lock);
            mailbox.rejected++;
            portEXIT_CRITICAL(&lock);
            return;
        }
        rep_context_v2_t context = {status.abi_version, sizeof(context), status.instance, 0};
        int64_t started = esp_timer_get_time();
        uint32_t latency = (uint32_t)((uint64_t)started - event.posted_us);
        if (latency > status.latency_max_us) {
            status.latency_max_us = latency;
        }
        callback_active = true;
        result = lifecycle.on_event(&context, &event);
        callback_finished(started);
        status.events_delivered++;
        status.timers_delivered += event.type == REP2_EVENT_TIMER;
        status.users_delivered += event.type == REP2_EVENT_USER;
        if (ui_event) {
            roundie_ui_delivered(status.instance, latency);
            status.ui_delivered++;
        }
        if (event.type == REP2_EVENT_USER) {
            status.last_value = event.value;
        }
        if (result != 0) {
            status.app_result = result;
            snprintf(status.error, sizeof(status.error), "callback-event");
            unload();
        } else {
            state(status.state);
        }
    }
}

bool roundie_elf_execute(const uint8_t *bytes, size_t size)
{
    if (executor == NULL || xTaskGetCurrentTaskHandle() != executor ||
        status.state != ROUNDIE_ELF_LOADING || callback_active) {
        return false;
    }
    /* Only state() publishes the executor-owned work record to readers. */
    roundie_elf_info_t info;
    int64_t started = esp_timer_get_time();
    status.before = memory();
    loader_initialized = false;
    lifecycle_initialized = false;
    lifecycle_started = false;
    lifecycle_stopped = false;
    ui_owned = false;
    memset(&lifecycle, 0, sizeof(lifecycle));
    status.resources_acquired = 2; /* instance and callback-reference record */
    if (!atomic_load(&allowed)) {
        snprintf(status.error, sizeof(status.error), "system-policy");
        goto cleanup;
    }
    if (!roundie_elf_validate(bytes, size, &info, status.error)) {
        goto cleanup;
    }
    status.code_bytes = info.code_bytes;
    status.data_bytes = info.data_bytes;
    status.abi_version = info.abi_version;
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
    loader_initialized = true;
    status.resources_acquired++;
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
    started = esp_timer_get_time();
    if (status.abi_version >= 2) {
        typedef const rep_descriptor_v2_t *(*entry_v2_t)(const rep_api_v2_t *);
        entry_v2_t entry_v2;
        _Static_assert(sizeof(entry_v2) == sizeof(image.entry), "Loader entry pointer");
        memcpy(&entry_v2, &image.entry, sizeof(entry_v2));
        callback_active = true;
        const rep_descriptor_v2_t *descriptor;
        if (status.abi_version == 3) {
            round_ui = false;
            ui_owned = roundie_ui_open(status.instance, ui_input);
            if (!ui_owned) {
                callback_active = false;
                snprintf(status.error, sizeof(status.error), "ui-owner-unavailable");
                goto cleanup;
            }
            typedef const rep_descriptor_v3_t *(*entry_v3_t)(const rep_api_v3_t *);
            entry_v3_t entry_v3;
            memcpy(&entry_v3, &image.entry, sizeof(entry_v3));
            const rep_descriptor_v3_t *v3 = entry_v3(&api_v3);
            callback_finished(started);
            if (((uintptr_t)v3 & 3U) != 0 || !loaded_range(v3, sizeof(*v3), false) ||
                v3->abi_version != 3 || v3->struct_size != sizeof(*v3) ||
                (v3->required_capabilities & ~api_v3.capabilities) != 0 ||
                (v3->required_capabilities & REP3_CAP_UI) == 0) {
                snprintf(status.error, sizeof(status.error), "application-ui-descriptor");
                goto cleanup;
            }
            /* Same callback signatures, with explicit ABI 3 context version. */
            round_ui = (v3->required_capabilities & REP3_CAP_ROUND_UI) != 0;
            lifecycle = (rep_descriptor_v2_t){3, sizeof(*v3), v3->init, v3->start, v3->on_event,
                v3->suspend, v3->resume, v3->stop, v3->destroy};
            descriptor = &lifecycle;
        } else {
            descriptor = entry_v2(&api_v2);
            callback_finished(started);
        }
        if (status.abi_version == 2 && (((uintptr_t)descriptor & 3U) != 0 || !loaded_range(descriptor, sizeof(*descriptor), false) ||
            descriptor->abi_version != REP_ABI_V2 || descriptor->struct_size != sizeof(*descriptor))) {
            snprintf(status.error, sizeof(status.error), "application-descriptor");
            goto cleanup;
        }
        lifecycle = *descriptor;
        const rep_callback_v2_t callbacks[] = {lifecycle.init, lifecycle.start, lifecycle.suspend,
            lifecycle.resume, lifecycle.stop, lifecycle.destroy};
        for (unsigned i = 0; i < sizeof(callbacks) / sizeof(callbacks[0]); i++) {
            if (!loaded_range((const void *)callbacks[i], 3, true)) {
                snprintf(status.error, sizeof(status.error), "application-callback");
                goto cleanup;
            }
        }
        if (!loaded_range((const void *)lifecycle.on_event, 3, true) || !atomic_load(&allowed)) {
            snprintf(status.error, sizeof(status.error), "application-event-or-policy");
            goto cleanup;
        }
        /* Destroy is required even after a failed init to release partial state. */
        lifecycle_initialized = true;
        started = esp_timer_get_time();
        int32_t result = invoke(lifecycle.init);
        status.init_us = (uint32_t)(esp_timer_get_time() - started);
        if (result != 0) {
            snprintf(status.error, sizeof(status.error), "callback-init");
            goto cleanup;
        }
        state(ROUNDIE_ELF_INITIALIZED);
        if (!atomic_load(&allowed)) {
            snprintf(status.error, sizeof(status.error), "system-policy");
            goto cleanup;
        }
        state(ROUNDIE_ELF_STARTING);
        lifecycle_started = true; /* stop also cleans a partially failed start */
        if (invoke(lifecycle.start) != 0 || !atomic_load(&allowed)) {
            snprintf(status.error, sizeof(status.error), "callback-start-or-policy");
            goto cleanup;
        }
        if (ui_owned && !roundie_ui_transition(status.instance, ROUNDIE_UI_SHOW)) {
            snprintf(status.error, sizeof(status.error), "ui-start-barrier");
            goto cleanup;
        }
        status.executed = true;
        status.during = memory();
        status.subscriptions = 1;
        status.resources_acquired++;
        state(ROUNDIE_ELF_RUNNING);
        portENTER_CRITICAL(&lock);
        mailbox.admitting = atomic_load(&allowed) && !mailbox.shutdown && mailbox.control == 0;
        portEXIT_CRITICAL(&lock);
        return true;
    }
    typedef const rep_app_descriptor_t *(*entry_t)(const rep_api_t *);
    entry_t entry;
    _Static_assert(sizeof(entry) == sizeof(image.entry), "Loader entry pointer representation");
    memcpy(&entry, &image.entry, sizeof(entry));
    callback_active = true;
    const rep_app_descriptor_t *descriptor = entry(&api);
    callback_finished(started);
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
    started = esp_timer_get_time();
    callback_active = true;
    status.app_result = descriptor->on_start(&context);
    callback_finished(started);
cleanup:
    unload();
    return true;
}
