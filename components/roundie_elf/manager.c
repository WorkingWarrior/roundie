#include "manager_private.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "loader.h"
#include "roundie_platform.h"
#include "rep/rep_app.h"
#include "rep/rep_app_v2.h"
#include "rep/rep_storage_v1.h"
#include "rep/rep_audio_v1.h"
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_AUDIO
#include "roundie_audio.h"
#endif
#include "roundie_app_ui.h"

static roundie_lock_t lock = ROUNDIE_LOCK_INITIALIZER;
static roundie_elf_status_t status;
static roundie_elf_status_t snapshot;
static atomic_bool allowed;
static roundie_thread_t executor = ROUNDIE_THREAD_NONE;
static bool callback_active;
static bool loader_initialized;
static bool lifecycle_initialized;
static bool lifecycle_started;
static bool lifecycle_stopped;
static rep_descriptor_v2_t lifecycle;
static bool ui_owned;
static bool round_ui; /* descriptor required REP3_CAP_ROUND_UI */
#if CONFIG_ROUNDIE_DEV_PACKAGES
static atomic_uint debug_delivery_delay_ms;

void roundie_elf_debug_delivery_delay(uint32_t ms)
{
    atomic_store(&debug_delivery_delay_ms, ms);
}

/* Timer posts and the longest gap between two of them; written only by the
 * executor, read and cleared by the diagnostic command. */
static atomic_uint debug_ticks;
static atomic_uint debug_tick_gap_max_us;
static int64_t debug_tick_last_us;

static void debug_tick(int64_t now_us)
{
    if (debug_tick_last_us != 0) {
        uint32_t gap = (uint32_t)(now_us - debug_tick_last_us);
        if (gap > atomic_load(&debug_tick_gap_max_us)) {
            atomic_store(&debug_tick_gap_max_us, gap);
        }
    }
    debug_tick_last_us = now_us;
    atomic_fetch_add(&debug_ticks, 1U);
}

void roundie_elf_debug_tick_gap(uint32_t *ticks, uint32_t *gap_max_us)
{
    *ticks = atomic_exchange(&debug_ticks, 0U);
    *gap_max_us = atomic_exchange(&debug_tick_gap_max_us, 0U);
}
#endif
#if CONFIG_ROUNDIE_AUDIO
/* rep_audio_v1: this instance's stream handle, 0 when none. Executor only. */
static uint32_t audio_stream;
#endif
/* REP3_CAP_CLOCK: executor-only state; the source is set once before start. */
static roundie_elf_clock_source_t clock_source;
static bool clock_app;
static bool clock_sent;
static uint32_t clock_revision;
static int64_t clock_check_us;
static int64_t clock_due_us;
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
/* Budget counters, under lock: this instance and every instance since boot. */
static roundie_elf_budget_t budget;
static roundie_elf_budget_t budget_boot;
static struct {
    void *pointer;
    uint32_t bytes;
    uint32_t generation;
} allocations[4];
/* App storage blob. The worker fills it while unloaded (storage_set) and takes
 * it back after unload (storage_take); between them only the executor touches
 * it, inside the instance's callbacks. state() publishes the hand-overs. */
static struct {
    bool available; /* this instance has storage; cleared at unload */
    bool dirty;     /* written since set; kept until the worker takes it */
    uint32_t length;
    uint8_t data[REP_STORAGE_CAPACITY];
} storage;

static roundie_elf_memory_t memory(void)
{
    return roundie_elf_loader_memory();
}

void roundie_elf_status(roundie_elf_status_t *out)
{
    roundie_lock(&lock);
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
    roundie_unlock(&lock);
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
    roundie_lock(&lock);
    status.state = next;
    snapshot = status;
    roundie_unlock(&lock);
}

bool roundie_elf_busy(void)
{
    roundie_elf_status_t current;
    roundie_elf_status(&current);
    return current.state != ROUNDIE_ELF_UNLOADED || roundie_elf_input_busy();
}

bool roundie_elf_prepare(void)
{
    roundie_lock(&lock);
    if (!atomic_load(&allowed) || status.state != ROUNDIE_ELF_UNLOADED || status.instance == UINT32_MAX) {
        roundie_unlock(&lock);
        return false;
    }
    uint32_t instance = status.instance + 1;
    uint32_t completed = status.completed;
    memset(&status, 0, sizeof(status));
    memset(&mailbox, 0, sizeof(mailbox));
    memset(&budget, 0, sizeof(budget));
    budget.instance = instance;
    budget_boot.instance = instance;
    status.instance = instance;
    status.completed = completed;
    status.state = ROUNDIE_ELF_LOADING;
    snapshot = status;
    roundie_unlock(&lock);
    return true;
}

void roundie_elf_cancel_prepare(void)
{
    roundie_lock(&lock);
    if (status.state == ROUNDIE_ELF_LOADING) {
        snprintf(status.error, sizeof(status.error), "executor-start-failed");
        status.state = ROUNDIE_ELF_UNLOADED;
        snapshot = status;
    }
    roundie_unlock(&lock);
}

void roundie_elf_set_allowed(bool value)
{
    roundie_lock(&lock);
    atomic_store(&allowed, value);
    if (!value) {
        mailbox.admitting = false;
        mailbox.shutdown = true;
    }
    roundie_unlock(&lock);
    if (!value) {
        roundie_ui_status_t ui;
        roundie_ui_status(&ui);
        roundie_ui_close_admission(ui.generation);
    }
}

void roundie_elf_bind_executor(roundie_thread_t task)
{
    executor = task;
}

static bool owner(uint32_t instance)
{
    return executor != ROUNDIE_THREAD_NONE && roundie_thread_self() == executor && callback_active &&
        instance == status.instance;
}

static bool loaded_range(const void *pointer, uint32_t bytes, bool code)
{
    return roundie_elf_loader_contains(pointer, bytes, code ? ROUNDIE_ELF_REGION_CODE : ROUNDIE_ELF_REGION_DATA);
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
    if (roundie_elf_loader_contains(pointer, bytes, ROUNDIE_ELF_REGION_WRITABLE)) {
        return true;
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
    if (copy.property >= REP_UI_VALUE && !round_ui) {
        return -1; /* VALUE and COLOR belong to the Round UI extension */
    }
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
    uint64_t milliseconds = (uint64_t)roundie_time_us() / 1000U;
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
            void *pointer = roundie_elf_loader_alloc(bytes);
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
            roundie_elf_loader_free(pointer);
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
    timer_due_us = roundie_time_us() + (int64_t)period_ms * 1000;
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
    roundie_lock(&lock);
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
    roundie_unlock(&lock);
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
        REP2_CAP_TIMER | REP3_CAP_UI | REP3_CAP_ROUND_UI | REP3_CAP_CLOCK,
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

static bool storage_owner(uint32_t instance)
{
    return owner(instance) && status.abi_version == REP_ABI_V3 && storage.available;
}

static int32_t app_storage_read(uint32_t instance, void *buffer, uint32_t capacity)
{
    if (!storage_owner(instance) || capacity < storage.length) {
        return -1;
    }
    if (storage.length != 0) {
        if (buffer == NULL || !writable_range(buffer, storage.length)) {
            return -1;
        }
        memcpy(buffer, storage.data, storage.length);
    }
    status.api_calls++;
    return (int32_t)storage.length;
}

static int32_t app_storage_write(uint32_t instance, const void *data, uint32_t length)
{
    if (!storage_owner(instance) || length > REP_STORAGE_CAPACITY) {
        return -1;
    }
    if (length != 0) {
        if (data == NULL || (!loaded_range(data, length, false) && !owned_range(data, length))) {
            return -1;
        }
        memcpy(storage.data, data, length);
    }
    storage.length = length;
    storage.dirty = true;
    status.api_calls++;
    return 0;
}

static const rep_storage_v1_t storage_v1 = {
    REP_STORAGE_V1, sizeof(rep_storage_v1_t), REP_STORAGE_CAPACITY, app_storage_read, app_storage_write,
};

const rep_storage_v1_t *rep_storage_v1(uint32_t required_version)
{
    if (required_version != REP_STORAGE_V1 || !storage_owner(status.instance)) {
        return NULL;
    }
    status.api_calls++;
    return &storage_v1;
}

#if CONFIG_ROUNDIE_AUDIO
static bool audio_owner(uint32_t instance, uint32_t stream)
{
    return owner(instance) && status.abi_version == REP_ABI_V3 && stream != 0 && stream == audio_stream;
}

static uint32_t app_audio_open(uint32_t instance, uint32_t sample_rate, uint32_t channels, uint32_t volume_percent)
{
    if (!owner(instance) || status.abi_version != REP_ABI_V3 || audio_stream != 0 ||
        status.state == ROUNDIE_ELF_STOPPING || status.state == ROUNDIE_ELF_UNLOADING ||
        channels > 2U || volume_percent > 100U) {
        return 0;
    }
    audio_stream = roundie_audio_stream_open(instance, sample_rate, (uint8_t)channels, (uint8_t)volume_percent);
    if (audio_stream != 0) {
        status.resources_acquired++;
    }
    status.api_calls++;
    return audio_stream;
}

static int32_t app_audio_writable(uint32_t instance, uint32_t stream)
{
    if (!audio_owner(instance, stream)) {
        return -1;
    }
    return roundie_audio_stream_writable(stream);
}

static int32_t app_audio_write(uint32_t instance, uint32_t stream, const void *pcm, uint32_t bytes)
{
    if (!audio_owner(instance, stream)) {
        return -1;
    }
    if (bytes == 0) {
        return 0;
    }
    /* The ring copies the samples before this returns; no app pointer is kept. */
    if (pcm == NULL || (!loaded_range(pcm, bytes, false) && !owned_range(pcm, bytes))) {
        return -1;
    }
    status.api_calls++;
    return roundie_audio_stream_write(stream, pcm, bytes);
}

static int32_t app_audio_close(uint32_t instance, uint32_t stream, uint32_t drain)
{
    if (!audio_owner(instance, stream)) {
        return -1;
    }
    roundie_audio_stream_close(stream, drain != 0U);
    audio_stream = 0;
    status.resources_released++;
    status.api_calls++;
    return 0;
}

static const rep_audio_v1_t audio_v1 = {
    REP_AUDIO_V1, sizeof(rep_audio_v1_t), ROUNDIE_AUDIO_RING_BYTES,
    app_audio_open, app_audio_writable, app_audio_write, app_audio_close,
};
#endif

const rep_audio_v1_t *rep_audio_v1(uint32_t required_version)
{
#if CONFIG_ROUNDIE_AUDIO
    roundie_audio_status_t audio;
    roundie_audio_status(&audio);
    if (required_version != REP_AUDIO_V1 || !owner(status.instance) || status.abi_version != REP_ABI_V3 ||
        !audio.initialized) {
        return NULL;
    }
    status.api_calls++;
    return &audio_v1;
#else
    (void)required_version;
    return NULL;
#endif
}

/* Executor only: the instance's stream is discarded when it stops or unloads.
 * The ring holds copies, so nothing refers to app memory afterwards. */
static void release_audio(void)
{
#if CONFIG_ROUNDIE_AUDIO
    if (!roundie_audio_release_owner(status.instance, 200)) {
        ESP_LOGW("elf", "Audio stream of instance %" PRIu32 " not yet released", status.instance);
    }
    if (audio_stream != 0) {
        audio_stream = 0;
        status.resources_reclaimed++;
    }
#endif
}

bool roundie_elf_storage_set(const void *data, size_t length)
{
    if (roundie_elf_busy() || storage.dirty || length > REP_STORAGE_CAPACITY ||
        (length != 0 && data == NULL)) {
        return false;
    }
    if (length != 0) {
        memcpy(storage.data, data, length);
    }
    storage.length = (uint32_t)length;
    storage.available = true; /* published by the next submission's prepare */
    return true;
}

void roundie_elf_storage_clear(void)
{
    if (!roundie_elf_busy()) {
        storage.available = false;
    }
}

bool roundie_elf_storage_take(void *data, size_t *length)
{
    if (roundie_elf_busy() || !storage.dirty) {
        return false;
    }
    memcpy(data, storage.data, storage.length);
    *length = storage.length;
    storage.dirty = false;
    storage.length = 0;
    return true;
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
    if (strcmp(name, "rep_storage_v1") == 0 && status.abi_version == 3) {
        return (uintptr_t)rep_storage_v1;
    }
    if (strcmp(name, "rep_audio_v1") == 0 && status.abi_version == 3) {
        return (uintptr_t)rep_audio_v1;
    }
    return strcmp(name, "rep_abi_v3") == 0 && status.abi_version == 3 ? (uintptr_t)rep_abi_v3 : 0;
}

static unsigned budget_bucket(uint32_t elapsed_us)
{
    if (elapsed_us < 1000U) {
        return 0;
    }
    if (elapsed_us < 10000U) {
        return 1;
    }
    if (elapsed_us < ROUNDIE_ELF_CALLBACK_NEAR_US) {
        return 2;
    }
    if (elapsed_us <= ROUNDIE_ELF_CALLBACK_BUDGET_US) {
        return 3;
    }
    return 4;
}

static void budget_callback(roundie_elf_budget_t *counters, uint32_t elapsed_us)
{
    counters->callbacks++;
    counters->buckets[budget_bucket(elapsed_us)]++;
    if (elapsed_us > counters->callback_max_us) {
        counters->callback_max_us = elapsed_us;
    }
}

static void budget_latency(roundie_elf_budget_t *counters, uint32_t latency_us)
{
    if (latency_us > counters->latency_max_us) {
        counters->latency_max_us = latency_us;
    }
}

void roundie_elf_budget(roundie_elf_budget_t *instance, roundie_elf_budget_t *boot)
{
    roundie_lock(&lock);
    if (instance != NULL) {
        *instance = budget;
    }
    if (boot != NULL) {
        *boot = budget_boot;
    }
    roundie_unlock(&lock);
}

/* callback_active and the crash-record phase change together (M9.1). */
static void callback_state(bool active)
{
    callback_active = active;
    roundie_elf_loader_phase(active ? ROUNDIE_ELF_PHASE_CALLBACK : ROUNDIE_ELF_PHASE_IDLE);
}

static void callback_finished(int64_t started)
{
    uint32_t elapsed = (uint32_t)(roundie_time_us() - started);
    status.callbacks++;
    status.execute_us = elapsed;
    if (elapsed > status.callback_max_us) {
        status.callback_max_us = elapsed;
    }
    if (elapsed > ROUNDIE_ELF_CALLBACK_BUDGET_US) {
        status.over_budget = true;
        ESP_LOGW("elf", "Callback exceeded 100 ms: %" PRIu32 " us", elapsed);
    }
    roundie_lock(&lock);
    budget_callback(&budget, elapsed);
    budget_callback(&budget_boot, elapsed);
    roundie_unlock(&lock);
    callback_state(false);
}

static int32_t invoke(rep_callback_v2_t callback)
{
    rep_context_v2_t context = {status.abi_version, sizeof(context), status.instance, 0};
    int64_t started = roundie_time_us();
    callback_state(true);
    int32_t result = callback(&context);
    callback_finished(started);
    if (status.app_result == 0) {
        status.app_result = result;
    }
    return result;
}

static void clear_events(void)
{
    roundie_lock(&lock);
    mailbox.admitting = false;
    budget.discarded += mailbox.count;
    budget_boot.discarded += mailbox.count;
    mailbox.released += mailbox.count;
    mailbox.count = 0;
    mailbox.head = 0;
    roundie_unlock(&lock);
}

static bool stop_lifecycle(void)
{
    if (!lifecycle_stopped) {
        int64_t started = roundie_time_us();
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
        release_audio(); /* after stop, so the app may close its own stream there */
        lifecycle_stopped = true;
        status.shutdown_us = (uint32_t)(roundie_time_us() - started);
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
    int64_t started = roundie_time_us();
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
            roundie_elf_loader_free(allocations[i].pointer);
            status.resources_reclaimed++;
        }
        allocations[i].pointer = NULL;
        allocations[i].bytes = 0;
        allocations[i].generation = 0;
    }
    status.alloc_bytes = 0;
    /* A dirty blob stays for the worker; no later instance reaches it. */
    storage.available = false;
    release_audio(); /* ABI 1 and 2 never open one; ABI 3 released it at stop */
    if (loader_initialized) {
        roundie_elf_loader_phase(ROUNDIE_ELF_PHASE_UNLOAD);
        roundie_elf_loader_unload();
        roundie_elf_loader_phase(ROUNDIE_ELF_PHASE_IDLE);
        loader_initialized = false;
        status.resources_released++;
    }
    /* Per-instance record and copied callback references retire after destroy. */
    status.resources_released += 2;
    status.after = memory();
    status.unload_us = (uint32_t)(roundie_time_us() - started);
    status.completed++;
    status.completed_us = roundie_time_us();
    state(ROUNDIE_ELF_UNLOADED);
}

esp_err_t roundie_elf_control(uint32_t instance, roundie_elf_control_t operation)
{
    roundie_lock(&lock);
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
    roundie_unlock(&lock);
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
    roundie_lock(&lock);
    if (!atomic_load(&allowed) || !mailbox.admitting || instance != snapshot.instance ||
        snapshot.abi_version < 2 || snapshot.state != ROUNDIE_ELF_RUNNING) {
        mailbox.rejected++;
        budget.rejected++;
        budget_boot.rejected++;
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
            if (type == REP3_EVENT_CLOCK && queued->type == type) {
                /* The newest sample replaces the whole event, with its time. */
                queued->value = value;
                queued->reserved = origin;
                queued->posted_us = (uint64_t)posted_us;
                duplicate = true;
            }
        }
        if (duplicate) {
            mailbox.coalesced++;
            budget.coalesced++;
            budget_boot.coalesced++;
        } else if (mailbox.count == ROUNDIE_ELF_EVENT_CAPACITY) {
            mailbox.dropped++;
            budget.dropped++;
            budget_boot.dropped++;
            result = ESP_ERR_NO_MEM;
        } else {
            mailbox.events[(mailbox.head + mailbox.count) % ROUNDIE_ELF_EVENT_CAPACITY] =
                (rep_event_v2_t){type, sizeof(rep_event_v2_t), value, origin, (uint64_t)posted_us};
            mailbox.count++;
            mailbox.acquired++;
            if (mailbox.count > budget.queue_peak) {
                budget.queue_peak = mailbox.count;
            }
            if (mailbox.count > budget_boot.queue_peak) {
                budget_boot.queue_peak = mailbox.count;
            }
        }
    }
    roundie_unlock(&lock);
    return result;
}

void roundie_elf_set_clock_source(roundie_elf_clock_source_t source)
{
    clock_source = source;
}

/* Posts local time at start, on each local minute and after a sync or zone
 * change. The source is sampled at most once per second, outside callbacks. */
static void clock_step(int64_t now_us)
{
    if (!clock_app || clock_source == NULL || now_us < clock_check_us) {
        return;
    }
    clock_check_us = now_us + 1000000;
    roundie_elf_clock_t clock;
    if (!clock_source(&clock)) {
        return;
    }
    bool due = !clock_sent || clock.revision != clock_revision || clock.monotonic_us >= clock_due_us;
    if (due && post_with_origin(status.instance, REP3_EVENT_CLOCK, clock.days, clock.ms_of_day,
                                false, clock.monotonic_us) == ESP_OK) {
        clock_sent = true;
        clock_revision = clock.revision;
        clock_due_us = clock.monotonic_us + (int64_t)(60000U - clock.ms_of_day % 60000U) * 1000;
    }
    if (clock_sent && clock_due_us < clock_check_us) {
        clock_check_us = clock_due_us;
    }
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
    return post_event(instance, REP2_EVENT_USER, value, roundie_time_us());
}

void roundie_elf_step(int64_t now_us)
{
    if (executor == ROUNDIE_THREAD_NONE || roundie_thread_self() != executor || callback_active) {
        return;
    }
    roundie_lock(&lock);
    bool shutdown = mailbox.shutdown || !atomic_load(&allowed);
    roundie_elf_control_t control = mailbox.control;
    mailbox.control = 0;
    uint32_t serial = mailbox.submitted;
    roundie_unlock(&lock);
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
        int64_t started = roundie_time_us();
        result = invoke(lifecycle.suspend);
        status.suspend_us = (uint32_t)(roundie_time_us() - started);
        /* Already queued ordinary work is canceled, never replayed on resume. */
        clear_events();
        if (ui_owned && !roundie_ui_transition(status.instance, ROUNDIE_UI_HIDE)) {
            result = -1;
        }
        state(ROUNDIE_ELF_SUSPENDED);
    } else if (control == ROUNDIE_ELF_RESUME) {
        state(ROUNDIE_ELF_RESUMING);
        int64_t started = roundie_time_us();
        result = invoke(lifecycle.resume);
        if (result == 0 && ui_owned && !roundie_ui_transition(status.instance, ROUNDIE_UI_SHOW)) {
            result = -1;
        }
        status.resume_us = (uint32_t)(roundie_time_us() - started);
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
        roundie_lock(&lock);
        mailbox.result = result;
        mailbox.completed = serial;
        mailbox.admitting = atomic_load(&allowed) && status.state == ROUNDIE_ELF_RUNNING;
        roundie_unlock(&lock);
        state(status.state);
        return;
    }
    if (status.state != ROUNDIE_ELF_RUNNING || status.abi_version < 2 || shutdown) {
        return;
    }
    if (status.timer_handle != 0 && now_us >= timer_due_us) {
        post_event(status.instance, REP2_EVENT_TIMER, status.timer_handle, now_us);
        timer_due_us = now_us + (int64_t)timer_period_ms * 1000;
#if CONFIG_ROUNDIE_DEV_PACKAGES
        debug_tick(now_us);
#endif
    }
    clock_step(now_us);
    rep_event_v2_t event;
    bool dispatch = false;
    roundie_lock(&lock);
    /* Callback admission is linearized here against shutdown/control requests. */
    if (mailbox.admitting && atomic_load(&allowed) && mailbox.count != 0) {
        event = mailbox.events[mailbox.head];
        mailbox.head = (mailbox.head + 1) % ROUNDIE_ELF_EVENT_CAPACITY;
        mailbox.count--;
        mailbox.released++;
        dispatch = true;
    }
    roundie_unlock(&lock);
    if (dispatch) {
        bool ui_event = event.type == REP3_EVENT_BUTTON || event.type == REP3_EVENT_VALUE;
        if (ui_event && !roundie_ui_event_valid(status.instance, event.type, event.value, event.reserved)) {
            roundie_lock(&lock);
            mailbox.rejected++;
            budget.rejected++;
            budget_boot.rejected++;
            roundie_unlock(&lock);
            return;
        }
#if CONFIG_ROUNDIE_DEV_PACKAGES
        uint32_t delay_ms = atomic_load(&debug_delivery_delay_ms);
        if (ui_event && delay_ms != 0) {
            /* Development diagnostic only (E0 §8.6): widens the window between
             * taking a UI event from the mailbox and on_event, so a STOP in
             * that window is reproducible. No lock is held while sleeping. */
            roundie_sleep_ms(delay_ms);
        }
#endif
        rep_context_v2_t context = {status.abi_version, sizeof(context), status.instance, 0};
        int64_t started = roundie_time_us();
        uint32_t latency = (uint32_t)((uint64_t)started - event.posted_us);
        if (latency > status.latency_max_us) {
            status.latency_max_us = latency;
        }
        roundie_lock(&lock);
        budget_latency(&budget, latency);
        budget_latency(&budget_boot, latency);
        roundie_unlock(&lock);
        callback_state(true);
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
    if (executor == ROUNDIE_THREAD_NONE || roundie_thread_self() != executor ||
        status.state != ROUNDIE_ELF_LOADING || callback_active) {
        return false;
    }
    /* Only state() publishes the executor-owned work record to readers. */
    roundie_elf_info_t info;
    int64_t started = roundie_time_us();
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
    if (!roundie_elf_loader_reserve(&status.before, &info)) {
        snprintf(status.error, sizeof(status.error), "executable-memory-reserve");
        goto cleanup;
    }
    status.loader_result = roundie_elf_loader_init();
    if (status.loader_result != 0) {
        snprintf(status.error, sizeof(status.error), "loader-init");
        goto cleanup;
    }
    loader_initialized = true;
    status.resources_acquired++;
    roundie_elf_loader_phase(ROUNDIE_ELF_PHASE_LOAD);
    status.loader_result = roundie_elf_loader_relocate(bytes, size, resolver);
    roundie_elf_loader_phase(ROUNDIE_ELF_PHASE_IDLE);
    if (status.loader_result != 0) {
        snprintf(status.error, sizeof(status.error), "loader-relocation");
        goto cleanup;
    }
    status.relocated = memory();
    roundie_elf_loader_describe(status.instance, &info);
    status.load_us = (uint32_t)(roundie_time_us() - started);
    state(ROUNDIE_ELF_READY);
    if (!atomic_load(&allowed)) {
        snprintf(status.error, sizeof(status.error), "system-policy");
        goto cleanup;
    }
    started = roundie_time_us();
    if (status.abi_version >= 2) {
        typedef const rep_descriptor_v2_t *(*entry_v2_t)(const rep_api_v2_t *);
        entry_v2_t entry_v2;
        roundie_elf_entry_t loaded = roundie_elf_loader_entry();
        _Static_assert(sizeof(entry_v2) == sizeof(loaded), "Loader entry pointer");
        memcpy(&entry_v2, &loaded, sizeof(entry_v2));
        callback_state(true);
        const rep_descriptor_v2_t *descriptor;
        /* For every ABI: an ABI 2 application must not inherit the previous
         * application's clock subscription, since it knows no clock event. */
        round_ui = false;
        clock_app = false;
        clock_sent = false;
        clock_check_us = 0;
        if (status.abi_version == 3) {
            ui_owned = roundie_ui_open(status.instance, ui_input);
            if (!ui_owned) {
                callback_state(false);
                snprintf(status.error, sizeof(status.error), "ui-owner-unavailable");
                goto cleanup;
            }
            typedef const rep_descriptor_v3_t *(*entry_v3_t)(const rep_api_v3_t *);
            entry_v3_t entry_v3;
            memcpy(&entry_v3, &loaded, sizeof(entry_v3));
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
            clock_app = (v3->required_capabilities & REP3_CAP_CLOCK) != 0;
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
        started = roundie_time_us();
        int32_t result = invoke(lifecycle.init);
        status.init_us = (uint32_t)(roundie_time_us() - started);
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
        roundie_lock(&lock);
        mailbox.admitting = atomic_load(&allowed) && !mailbox.shutdown && mailbox.control == 0;
        roundie_unlock(&lock);
        return true;
    }
    typedef const rep_app_descriptor_t *(*entry_t)(const rep_api_t *);
    entry_t entry;
    roundie_elf_entry_t loaded = roundie_elf_loader_entry();
    _Static_assert(sizeof(entry) == sizeof(loaded), "Loader entry pointer representation");
    memcpy(&entry, &loaded, sizeof(entry));
    callback_state(true);
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
    started = roundie_time_us();
    callback_state(true);
    status.app_result = descriptor->on_start(&context);
    callback_finished(started);
cleanup:
    unload();
    return true;
}
