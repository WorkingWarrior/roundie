#include <assert.h>
#include <elf.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rep/rep_app.h"
/* Compile actual manager, including private ownership checks, with a mocked
 * IDF/loader backend. No Xtensa code is executed on the host. */
#include "../components/roundie_elf/manager.c"
#include "../components/roundie_elf/executor.c"
#include "../components/roundie_elf/idf/loader.c"
#include "../components/roundie_elf/crash.c"

/* rep_audio_v1 backend: roundie_audio's stream calls, mocked. The ring takes
 * up to ROUNDIE_AUDIO_RING_BYTES; tests empty it as playback would. */
static struct {
    bool ready;
    uint32_t next;
    uint32_t handle;
    uint32_t owner;
    uint32_t rate;
    uint8_t channels;
    uint32_t buffered;
    uint32_t total;
    int32_t peak;
    uint32_t opens;
    uint32_t closes;
    uint32_t drained;
    uint32_t releases;
} mock_audio = {.ready = true};
void roundie_audio_status(roundie_audio_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->initialized = mock_audio.ready;
}
roundie_audio_stream_t roundie_audio_stream_open(uint32_t owner, uint32_t sample_rate, uint8_t channels,
                                                 uint8_t volume_percent)
{
    (void)volume_percent;
    if (!mock_audio.ready || mock_audio.handle != 0 || (channels != 1 && channels != 2)) {
        return 0;
    }
    mock_audio.handle = ++mock_audio.next;
    mock_audio.owner = owner;
    mock_audio.rate = sample_rate;
    mock_audio.channels = channels;
    mock_audio.buffered = 0;
    mock_audio.total = 0;
    mock_audio.peak = 0;
    mock_audio.opens++;
    return mock_audio.handle;
}
int32_t roundie_audio_stream_writable(roundie_audio_stream_t stream)
{
    return stream != 0 && stream == mock_audio.handle ? (int32_t)(ROUNDIE_AUDIO_RING_BYTES - mock_audio.buffered) : -1;
}
int32_t roundie_audio_stream_write(roundie_audio_stream_t stream, const void *pcm, uint32_t bytes)
{
    if (stream == 0 || stream != mock_audio.handle) {
        return -1;
    }
    uint32_t frame = mock_audio.channels * 2U;
    uint32_t take = bytes - bytes % frame;
    if (take > ROUNDIE_AUDIO_RING_BYTES - mock_audio.buffered) {
        take = ROUNDIE_AUDIO_RING_BYTES - mock_audio.buffered;
    }
    const int16_t *samples = pcm;
    for (uint32_t i = 0; i < take / 2U; i++) {
        int32_t value = samples[i] < 0 ? -samples[i] : samples[i];
        mock_audio.peak = value > mock_audio.peak ? value : mock_audio.peak;
    }
    mock_audio.buffered += take;
    mock_audio.total += take;
    return (int32_t)take;
}
void roundie_audio_stream_close(roundie_audio_stream_t stream, bool drain)
{
    if (stream != 0 && stream == mock_audio.handle) {
        mock_audio.handle = 0;
        mock_audio.closes++;
        mock_audio.drained += drain;
    }
}
bool roundie_audio_release_owner(uint32_t owner, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (mock_audio.handle != 0 && mock_audio.owner == owner) {
        mock_audio.handle = 0;
        mock_audio.releases++;
    }
    return true;
}

/* ABI 1/2 regression executable does not own GUI resources. ABI 3 exercises
 * the real bridge and LVGL backend in the dedicated UI integration harness. */
#ifndef ROUNDIE_UI_INTEGRATION
void roundie_ui_status(roundie_ui_status_t *out) { memset(out, 0, sizeof(*out)); }
void roundie_ui_close_admission(uint32_t generation) { (void)generation; }
bool roundie_ui_open(uint32_t generation, roundie_ui_event_sink_t sink)
{ (void)generation; (void)sink; return false; }
bool roundie_ui_transition(uint32_t generation, roundie_ui_transition_t operation)
{ (void)generation; (void)operation; return false; }
bool roundie_ui_retire(uint32_t generation) { (void)generation; return false; }
uint32_t roundie_ui_create(uint32_t generation, const rep_ui_widget_v2_t *widget)
{ (void)generation; (void)widget; return 0; }
bool roundie_ui_shadow(uint32_t generation, uint32_t widget, int32_t *committed,
                       int32_t *requested, uint32_t *pending)
{ (void)generation; (void)widget; (void)committed; (void)requested; (void)pending; return false; }
int32_t roundie_ui_update(uint32_t generation, const rep_ui_update_t *update)
{ (void)generation; (void)update; return -1; }
int32_t roundie_ui_destroy(uint32_t generation, uint32_t view)
{ (void)generation; (void)view; return -1; }
bool roundie_ui_event_valid(uint32_t generation, uint32_t type, uint32_t widget, uint32_t payload)
{ (void)generation; (void)type; (void)widget; (void)payload; return false; }
void roundie_ui_delivered(uint32_t generation, uint32_t latency_us)
{ (void)generation; (void)latency_us; }
#endif

static unsigned live_bytes;
static unsigned allocations_live;
static unsigned load_count;
static unsigned deinit_count;
static unsigned callback_count;
static unsigned mode;
static bool fail_alloc;
static bool fail_task;
static bool fail_queue;
static bool fail_send;
static TaskHandle_t current_task = (void *)1;
static uintptr_t (*resolve)(const char *);
static const rep_api_t *app_api;
static uint32_t previous_instance;
static int64_t mock_clock = 100000;
static int64_t mock_step = 1000; /* 0 freezes time for the budget tests. */
static uint32_t event_cost_us;   /* Mock time a USER event callback takes. */
static unsigned runtime_mode;
static const rep_api_v2_t *runtime_api;
static uint32_t runtime_callbacks[7];
static uint32_t values[32];
static unsigned value_count;
static uint32_t old_timer;
static void *runtime_memory;

static unsigned digests;
bool roundie_elf_crash_digest(const uint8_t *bytes, size_t size, uint8_t digest[32])
{
    digests++;
    for (unsigned i = 0; i < 32; i++) {
        digest[i] = (uint8_t)(bytes[i % size] + i);
    }
    return true;
}

void mock_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task; }
int64_t esp_timer_get_time(void) { return mock_clock += mock_step; }
void *heap_caps_malloc(size_t bytes, unsigned caps)
{
    (void)caps;
    if (fail_alloc) {
        return NULL;
    }
    size_t *allocation = calloc(1, bytes + sizeof(max_align_t));
    assert(allocation != NULL);
    *allocation = bytes;
    live_bytes += bytes;
    allocations_live++;
    return (char *)allocation + sizeof(max_align_t);
}
void heap_caps_free(void *pointer)
{
    if (pointer != NULL) {
        size_t *allocation = (size_t *)((char *)pointer - sizeof(max_align_t));
        live_bytes -= *allocation;
        allocations_live--;
        free(allocation);
    }
}
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 1000000 - live_bytes; }
size_t heap_caps_get_largest_free_block(unsigned caps) { return heap_caps_get_free_size(caps); }
void elf_set_symbol_resolver(uintptr_t (*function)(const char *)) { resolve = function; }
int esp_elf_init(esp_elf_t *elf)
{
    memset(elf, 0, sizeof(*elf));
    return mode == 3 ? -12 : 0;
}

static int32_t callback(const rep_app_context_t *context)
{
    callback_count++;
    assert(context->event == REP_EVENT_START && context->instance != previous_instance);
    if (previous_instance != 0) {
        assert(app_api->alloc(previous_instance, 4) == NULL);
    }
    previous_instance = context->instance;
    assert(!roundie_elf_prepare());
    assert(!roundie_elf_execute(NULL, 0));
    assert(app_api->log(context->instance, (void *)1, 5) == -1);
    assert(app_api->alloc(context->instance, 4097) == NULL);
    fail_alloc = true;
    assert(app_api->alloc(context->instance, 8) == NULL);
    fail_alloc = false;
    void *a = app_api->alloc(context->instance, 32);
    assert(a != NULL);
    memcpy(a, "hello", 5);
    assert(app_api->log(context->instance, a, 5) == 0);
    assert(app_api->monotonic_ms(context->instance) != 0);
    current_task = (void *)2;
    assert(app_api->alloc(context->instance, 4) == NULL);
    assert(app_api->release(context->instance, a) == -1);
    current_task = (void *)1;
    assert(app_api->release(context->instance, a) == 0);
    assert(app_api->release(context->instance, a) == -1);
    /* Intentionally retained SDK memory must be reclaimed after callback. */
    assert(app_api->alloc(context->instance, 64) != NULL);
    assert(app_api->alloc(context->instance, 64) != NULL);
    return mode == 6 ? -7 : 42;
}

static rep_app_descriptor_t descriptor = {REP_ABI_VERSION, sizeof(descriptor), callback};
static const rep_app_descriptor_t *entry(const rep_api_t *table)
{
    app_api = table;
    assert(rep_abi_v1(1) == table && rep_abi_v1(99) == NULL);
    if (mode == 4) {
        return NULL;
    }
    descriptor.abi_version = mode == 5 ? 99 : REP_ABI_VERSION;
    return &descriptor;
}

static int32_t runtime_init(const rep_context_v2_t *context)
{
    runtime_callbacks[0]++;
    assert(roundie_elf_crash_current_phase() == ROUNDIE_ELF_PHASE_CALLBACK);
    assert(context->abi_version == 2 && context->struct_size == sizeof(*context));
    assert(runtime_api->alloc(context->instance, 0) == NULL);
    assert(runtime_api->alloc(context->instance, UINT32_MAX) == NULL);
    assert(runtime_api->alloc(context->instance - 1, 1) == NULL);
    runtime_memory = runtime_api->alloc(context->instance, 32);
    assert(runtime_memory != NULL);
    assert(runtime_api->release(context->instance, (void *)1) == -1);
    assert(runtime_api->timer_cancel(context->instance, old_timer) == -1);
    uint32_t handle = runtime_api->timer_start(context->instance, 50);
    assert(handle != 0 && runtime_api->timer_start(context->instance, 50) == 0);
    assert(runtime_api->timer_cancel(context->instance - 1, handle) == -1);
    assert(runtime_api->timer_cancel(context->instance, handle) == 0);
    assert(runtime_api->timer_cancel(context->instance, handle) == -1);
    old_timer = handle;
    /* Exhaust both allocation-count and aggregate limits without overflow. */
    void *a = runtime_api->alloc(context->instance, 32);
    void *b = runtime_api->alloc(context->instance, 32);
    void *c = runtime_api->alloc(context->instance, 4000);
    assert(a != NULL && b != NULL && c != NULL);
    assert(runtime_api->alloc(context->instance, 1) == NULL);
    assert(runtime_api->release(context->instance, a) == 0);
    assert(runtime_api->release(context->instance, a) == -1);
    assert(runtime_api->release(context->instance, b) == 0);
    assert(runtime_api->release(context->instance, c) == 0);
    return runtime_mode == 1 ? -11 : 0;
}

static int32_t runtime_start(const rep_context_v2_t *context)
{
    runtime_callbacks[1]++;
    assert(roundie_elf_crash_current_phase() == ROUNDIE_ELF_PHASE_CALLBACK);
    assert(runtime_api->timer_start(context->instance, 250) != 0);
    return runtime_mode == 2 ? -12 : 0;
}

static int32_t runtime_event(const rep_context_v2_t *context, const rep_event_v2_t *event)
{
    runtime_callbacks[2]++;
    assert(callback_active);
    assert(roundie_elf_crash_current_phase() == ROUNDIE_ELF_PHASE_CALLBACK);
    /* ABI 2 knows only these; the event example returns -4 for any other. */
    assert(event->type == REP2_EVENT_TIMER || event->type == REP2_EVENT_USER);
    unsigned before = runtime_callbacks[2];
    roundie_elf_step(esp_timer_get_time()); /* Callback recursion is refused. */
    assert(before == runtime_callbacks[2]);
    if (event->type == REP2_EVENT_USER) {
        assert(value_count < 32);
        values[value_count++] = event->value;
        mock_clock += event_cost_us;
    }
    if (runtime_mode == 10) {
        unsigned deinit_before = deinit_count;
        roundie_elf_set_allowed(false);
        assert(roundie_elf_post(context->instance, 9) == ESP_ERR_INVALID_STATE);
        roundie_elf_step(esp_timer_get_time());
        assert(deinit_count == deinit_before && image.ptext != NULL);
    }
    return runtime_mode == 5 ? -15 : 0;
}

static int32_t runtime_suspend(const rep_context_v2_t *context)
{
    (void)context;
    runtime_callbacks[3]++;
    return runtime_mode == 6 ? -16 : 0;
}
static int32_t runtime_resume(const rep_context_v2_t *context)
{
    (void)context;
    runtime_callbacks[4]++;
    return runtime_mode == 7 ? -17 : 0;
}
static int32_t runtime_stop(const rep_context_v2_t *context)
{
    runtime_callbacks[5]++;
    assert(roundie_elf_crash_current_phase() == ROUNDIE_ELF_PHASE_CALLBACK);
    assert(runtime_api->alloc(context->instance, 8) == NULL);
    assert(roundie_elf_post(context->instance, 8) == ESP_ERR_INVALID_STATE);
    return runtime_mode == 8 ? -18 : 0;
}
static int32_t runtime_destroy(const rep_context_v2_t *context)
{
    runtime_callbacks[6]++;
    assert(roundie_elf_crash_current_phase() == ROUNDIE_ELF_PHASE_CALLBACK);
    if (runtime_memory != NULL && runtime_mode != 11) {
        assert(runtime_api->release(context->instance, runtime_memory) == 0);
    }
    runtime_memory = NULL;
    return runtime_mode == 9 ? -19 : 0;
}
static rep_descriptor_v2_t runtime_descriptor = {
    2, sizeof(runtime_descriptor), runtime_init, runtime_start, runtime_event,
    runtime_suspend, runtime_resume, runtime_stop, runtime_destroy,
};
static const rep_descriptor_v2_t *runtime_entry(const rep_api_v2_t *table)
{
    runtime_api = table;
    assert(rep_abi_v2(2) == table && rep_abi_v2(1) == NULL && rep_abi_v1(1) == NULL);
    runtime_descriptor.struct_size = runtime_mode == 3 ? 4 : sizeof(runtime_descriptor);
    runtime_descriptor.on_event = runtime_mode == 4 ? NULL : runtime_event;
    return &runtime_descriptor;
}

int esp_elf_relocate(esp_elf_t *elf, const uint8_t *bytes)
{
    (void)bytes;
    load_count++;
    assert(roundie_elf_crash_current_phase() == ROUNDIE_ELF_PHASE_LOAD);
    assert(resolve(status.abi_version == 1 ? "rep_abi_v1" :
        status.abi_version == 2 ? "rep_abi_v2" : "rep_abi_v3") ==
        (status.abi_version == 1 ? (uintptr_t)rep_abi_v1 :
         status.abi_version == 2 ? (uintptr_t)rep_abi_v2 : (uintptr_t)rep_abi_v3));
    assert(resolve("malloc") == 0 && resolve("xTaskCreate") == 0 && resolve(NULL) == 0);
    assert(resolve("rep_storage_v1") == (status.abi_version == 3 ? (uintptr_t)rep_storage_v1 : 0));
    assert(resolve("rep_audio_v1") == (status.abi_version == 3 ? (uintptr_t)rep_audio_v1 : 0));
    elf->ptext = heap_caps_malloc(256, 0);
    if (elf->ptext == NULL) {
        return -12;
    }
    elf->pdata = heap_caps_malloc(128, 0);
    if (mode == 2) {
        return -22;
    }
#ifdef ROUNDIE_UI_INTEGRATION
    if (status.abi_version == 3) {
        extern void mock_ui_relocate(esp_elf_t *elf);
        mock_ui_relocate(elf);
        return 0;
    }
#endif
    uintptr_t first = (uintptr_t)entry < (uintptr_t)callback ? (uintptr_t)entry : (uintptr_t)callback;
    uintptr_t last = (uintptr_t)entry > (uintptr_t)callback ? (uintptr_t)entry : (uintptr_t)callback;
    elf->sec[ELF_SEC_TEXT] = (esp_elf_sec_t){first, last - first + 16, 0};
    elf->sec[ELF_SEC_RODATA] = (esp_elf_sec_t){(uintptr_t)&descriptor, sizeof(descriptor), 0};
    typedef const rep_app_descriptor_t *(*rep_entry_t)(const rep_api_t *);
    rep_entry_t address = entry;
    memcpy(&elf->entry, &address, sizeof(address));
    if (status.abi_version == 2) {
        const uintptr_t functions[] = {(uintptr_t)runtime_entry, (uintptr_t)runtime_init,
            (uintptr_t)runtime_start, (uintptr_t)runtime_event, (uintptr_t)runtime_suspend,
            (uintptr_t)runtime_resume, (uintptr_t)runtime_stop, (uintptr_t)runtime_destroy};
        first = functions[0];
        last = first;
        for (unsigned i = 1; i < sizeof(functions) / sizeof(functions[0]); i++) {
            if (functions[i] < first) {
                first = functions[i];
            }
            if (functions[i] > last) {
                last = functions[i];
            }
        }
        elf->sec[ELF_SEC_TEXT] = (esp_elf_sec_t){first, last - first + 16, 0};
        elf->sec[ELF_SEC_RODATA] = (esp_elf_sec_t){(uintptr_t)&runtime_descriptor, sizeof(runtime_descriptor), 0};
        typedef const rep_descriptor_v2_t *(*entry_v2_t)(const rep_api_v2_t *);
        entry_v2_t entry_v2 = runtime_entry;
        memcpy(&elf->entry, &entry_v2, sizeof(entry_v2));
    }
    if (mode == 7) {
        roundie_elf_set_allowed(false);
    }
    return 0;
}
void esp_elf_deinit(esp_elf_t *elf)
{
    deinit_count++;
    assert(roundie_elf_crash_current_phase() == ROUNDIE_ELF_PHASE_UNLOAD);
    heap_caps_free(elf->pdata);
    heap_caps_free(elf->ptext);
    memset(elf, 0, sizeof(*elf));
}

QueueHandle_t xQueueCreate(unsigned count, unsigned size)
{
    assert(count == 1 && size == sizeof(request_t));
    return fail_queue ? NULL : (void *)3;
}
void vQueueDelete(QueueHandle_t queue) { assert(queue == (void *)3); }
int xQueueSend(QueueHandle_t queue, const void *value, unsigned wait)
{
    (void)queue; (void)value; (void)wait;
    return !fail_send;
}
int xQueueReceive(QueueHandle_t queue, void *out, unsigned wait)
{
    (void)queue; (void)out; (void)wait;
    abort(); /* Not exercising a real FreeRTOS task on host. */
}
/* idf/executor_stack.c is target-only (MMU, assist-debug); M9.1 tests its
 * host-visible contract: creation can fail, arming can fail closed. */
bool roundie_elf_stack_create(TaskFunction_t entry, TaskHandle_t *task)
{
    assert(entry == execute);
    *task = fail_task ? NULL : (void *)1;
    return !fail_task;
}
bool roundie_elf_stack_arm(void) { return true; }
int esp_task_wdt_add(void *task) { (void)task; return 0; }
int esp_task_wdt_delete(void *task) { (void)task; return 0; }

static unsigned char fixture[ROUNDIE_ELF_MAX_FILE];
static size_t fixture_size;

static void validation_tests(void)
{
    uint8_t copy[ROUNDIE_ELF_MAX_FILE];
    roundie_elf_info_t info;
    char error[ROUNDIE_ELF_ERROR_SIZE];
    assert(roundie_elf_validate(fixture, fixture_size, &info, error));
    assert(info.imports == 1 && info.relocations > 0);
    for (size_t size = 0; size < fixture_size; size++) {
        assert(!roundie_elf_validate(fixture, size, &info, error));
    }
    const struct {unsigned offset; unsigned bytes; uint32_t value;} mutations[] = {
        {0, 1, 0}, {4, 1, 2}, {5, 1, 2}, {18, 2, EM_ARM},
        {16, 2, ET_EXEC}, {24, 4, 0}, {32, 4, 0xfffffffc},
        {28, 4, 0xfffffffc}, {48, 2, 129}, {50, 2, 0xffff},
    };
    for (unsigned i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) {
        memcpy(copy, fixture, fixture_size);
        memcpy(copy + mutations[i].offset, &mutations[i].value, mutations[i].bytes);
        assert(!roundie_elf_validate(copy, fixture_size, &info, error));
    }
    Elf32_Ehdr header;
    memcpy(&header, fixture, sizeof(header));
    for (unsigned i = 1; i < header.e_shnum; i++) {
        Elf32_Shdr sh;
        unsigned offset = header.e_shoff + i * sizeof(sh);
        memcpy(&sh, fixture + offset, sizeof(sh));
        if (sh.sh_type == SHT_PROGBITS && sh.sh_flags == SHF_ALLOC && sh.sh_size > 12) {
            /* A physically observed loader limitation: rounded text must not
             * absorb the beginning of adjacent readonly application data. */
            roundie_elf_info_t valid;
            assert(roundie_elf_validate(fixture, fixture_size, &valid, error));
            memcpy(copy, fixture, fixture_size);
            ((Elf32_Shdr *)(copy + offset))->sh_addr =
                valid.sections[0].address + valid.code_bytes - 1;
            assert(!roundie_elf_validate(copy, fixture_size, &info, error));
            assert(strcmp(error, "rounded-text-overlap") == 0);
        }
        memcpy(copy, fixture, fixture_size);
        ((Elf32_Shdr *)(copy + offset))->sh_offset = 0xfffffffc;
        if (sh.sh_type != SHT_NOBITS) {
            assert(!roundie_elf_validate(copy, fixture_size, &info, error));
        }
        if (sh.sh_type == SHT_RELA && sh.sh_size != 0) {
            memcpy(copy, fixture, fixture_size);
            ((Elf32_Rela *)(copy + sh.sh_offset))->r_info = 1; /* R_XTENSA_32 unsupported */
            assert(!roundie_elf_validate(copy, fixture_size, &info, error));
            memcpy(copy, fixture, fixture_size);
            ((Elf32_Rela *)(copy + sh.sh_offset))->r_offset = 0xfffffffc;
            assert(!roundie_elf_validate(copy, fixture_size, &info, error));
            memcpy(copy, fixture, fixture_size);
            ((Elf32_Rela *)(copy + sh.sh_offset))->r_info = 0xffffff05;
            assert(!roundie_elf_validate(copy, fixture_size, &info, error));
        }
    }
    assert(roundie_elf_import_allowed("rep_abi_v2"));
    assert(roundie_elf_import_allowed("rep_abi_v3"));
    assert(roundie_elf_import_allowed("rep_storage_v1"));
    assert(roundie_elf_import_allowed("rep_audio_v1"));
    const char *forbidden[] = {"malloc", "esp_restart", "xTaskCreate", "rep_abi_v4", "rep_abi_v1_suffix", "rep_storage_v2",
                               "rep_audio_v2", "roundie_audio_stream_open", "", NULL};
    for (unsigned i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        assert(!roundie_elf_import_allowed(forbidden[i]));
    }
    /* Deterministic bounded mutation corpus under ASan/UBSan. */
    uint32_t random = 12345;
    for (unsigned i = 0; i < 4000; i++) {
        memcpy(copy, fixture, fixture_size);
        random = random * 1664525U + 1013904223U;
        size_t offset = random % fixture_size;
        copy[offset] ^= (uint8_t)(random >> 24);
        roundie_elf_validate(copy, fixture_size, &info, error);
    }
}

static void control_step(roundie_elf_control_t operation)
{
    assert(roundie_elf_control(status.instance, operation) == ESP_OK);
    assert(roundie_elf_control(status.instance, operation) == ESP_ERR_INVALID_STATE);
    roundie_elf_step(esp_timer_get_time());
    roundie_elf_status_t result;
    roundie_elf_status(&result);
    assert(result.control_submitted == result.control_completed);
}

static bool clock_now(roundie_elf_clock_t *out)
{
    *out = (roundie_elf_clock_t){esp_timer_get_time(), 20000, 0, 1};
    return true;
}

/* As after an ABI 3 clock application (the watch home): its clock
 * subscription must not carry over to a following ABI 2 application.
 * Firmware 0.3.53 delivered a clock event to the event example on the 1.85B
 * after a time sync (M8), and the example returned -4. */
static void clock_leak_tests(void)
{
    runtime_mode = 0;
    memset(runtime_callbacks, 0, sizeof(runtime_callbacks));
    clock_app = true;
    clock_sent = false;
    clock_check_us = 0;
    roundie_elf_set_clock_source(clock_now);
    roundie_elf_set_allowed(true);
    assert(roundie_elf_prepare());
    assert(roundie_elf_execute(fixture, fixture_size));
    assert(status.state == ROUNDIE_ELF_RUNNING);
    for (unsigned i = 0; i < 4; i++) {
        roundie_elf_step(esp_timer_get_time());
    }
    assert(status.state == ROUNDIE_ELF_RUNNING && mailbox.count == 0);
    roundie_elf_set_clock_source(NULL);
    control_step(ROUNDIE_ELF_STOP);
    control_step(ROUNDIE_ELF_UNLOAD);
    assert(status.state == ROUNDIE_ELF_UNLOADED && live_bytes == 0 && allocations_live == 0);
}

static roundie_elf_budget_t budget_now(void)
{
    roundie_elf_budget_t counters;
    roundie_elf_budget(&counters, NULL);
    return counters;
}

/* Posts one USER event costing cost_us and steps until it is dispatched; a
 * TIMER event queued ahead of it is dispatched first at no cost. */
static void budget_dispatch(uint32_t generation, uint32_t value, uint32_t cost_us)
{
    unsigned before = value_count;
    event_cost_us = cost_us;
    assert(roundie_elf_post(generation, value) == ESP_OK);
    for (unsigned i = 0; i < 4 && value_count == before; i++) {
        roundie_elf_step(esp_timer_get_time());
    }
    assert(value_count == before + 1 && values[before] == value);
    event_cost_us = 0;
}

/* Deterministic callback budget and mailbox cases (M9): mock time advances
 * only by the cost a callback declares, so each duration is exact. */
static void budget_tests(void)
{
    runtime_mode = 0;
    memset(runtime_callbacks, 0, sizeof(runtime_callbacks));
    value_count = 0;
    roundie_elf_budget_t boot_before;
    roundie_elf_budget(NULL, &boot_before);
    mock_step = 0;
    roundie_elf_set_allowed(true);
    assert(roundie_elf_prepare());
    uint32_t generation = status.instance;
    assert(roundie_elf_execute(fixture, fixture_size));
    assert(status.state == ROUNDIE_ELF_RUNNING);
    roundie_elf_budget_t counters = budget_now();
    assert(counters.instance == generation && counters.dropped == 0 && counters.discarded == 0);
    for (unsigned i = 1; i < ROUNDIE_ELF_BUDGET_BUCKETS; i++) {
        assert(counters.buckets[i] == 0);
    }

    /* 1-3: in budget, at the near threshold and at the limit, over by 1 us. */
    const uint32_t costs[] = {500, 5000, 79999, 80000, 100000, 100001};
    const unsigned expected[] = {0, 1, 2, 3, 3, 4};
    for (unsigned i = 0; i < sizeof(costs) / sizeof(costs[0]); i++) {
        roundie_elf_budget_t previous = budget_now();
        budget_dispatch(generation, i, costs[i]);
        counters = budget_now();
        assert(counters.buckets[expected[i]] == previous.buckets[expected[i]] + 1);
        for (unsigned bucket = 1; bucket < ROUNDIE_ELF_BUDGET_BUCKETS; bucket++) {
            if (bucket != expected[i]) {
                assert(counters.buckets[bucket] == previous.buckets[bucket]);
            }
        }
        assert(status.over_budget == (costs[i] > ROUNDIE_ELF_CALLBACK_BUDGET_US));
        assert(status.state == ROUNDIE_ELF_RUNNING);
    }
    assert(counters.callback_max_us == 100001 && status.callback_max_us == 100001);

    /* 4: a series of expensive callbacks is counted; the budget is soft and
     * the application keeps running (a native callback cannot be preempted). */
    for (uint32_t i = 0; i < 5; i++) {
        budget_dispatch(generation, 10 + i, 90000);
    }
    for (uint32_t i = 0; i < 3; i++) {
        budget_dispatch(generation, 20 + i, 150000);
    }
    counters = budget_now();
    assert(counters.buckets[3] == 2 + 5 && counters.buckets[4] == 1 + 3);
    assert(counters.callback_max_us == 150000 && status.state == ROUNDIE_ELF_RUNNING);
    assert(counters.callbacks == status.callbacks);

    /* Drain a due timer with time frozen, so nothing else enters the queue. */
    for (unsigned i = 0; i < 4; i++) {
        roundie_elf_step(esp_timer_get_time());
    }
    assert(mailbox.count == 0);

    /* 5: overflow refuses the newest event and keeps the queued ones. */
    value_count = 0;
    for (uint32_t i = 0; i < ROUNDIE_ELF_EVENT_CAPACITY; i++) {
        assert(roundie_elf_post(generation, 100 + i) == ESP_OK);
    }
    assert(roundie_elf_post(generation, 199) == ESP_ERR_NO_MEM);
    counters = budget_now();
    assert(counters.queue_peak == ROUNDIE_ELF_EVENT_CAPACITY && counters.dropped == 1);

    /* 6: backpressure clears after one dispatch; the queue keeps FIFO order. */
    roundie_elf_step(esp_timer_get_time());
    assert(value_count == 1 && values[0] == 100);
    assert(roundie_elf_post(generation, 108) == ESP_OK);
    assert(roundie_elf_post(generation, 199) == ESP_ERR_NO_MEM);
    for (unsigned i = 0; i < ROUNDIE_ELF_EVENT_CAPACITY && mailbox.count != 0; i++) {
        roundie_elf_step(esp_timer_get_time());
    }
    assert(mailbox.count == 0 && value_count == ROUNDIE_ELF_EVENT_CAPACITY + 1);
    for (unsigned i = 0; i < value_count; i++) {
        assert(values[i] == 100 + i);
    }
    counters = budget_now();
    assert(counters.dropped == 2 && counters.queue_peak == ROUNDIE_ELF_EVENT_CAPACITY);

    /* Latency: the third of three queued 30 ms events waits for two. */
    value_count = 0;
    event_cost_us = 30000;
    for (uint32_t i = 0; i < 3; i++) {
        assert(roundie_elf_post(generation, i) == ESP_OK);
    }
    for (unsigned i = 0; i < 3; i++) {
        roundie_elf_step(esp_timer_get_time());
    }
    event_cost_us = 0;
    assert(value_count == 3 && budget_now().latency_max_us >= 60000);

    /* 7: an event for a previous instance is rejected as stale. */
    uint32_t rejected = budget_now().rejected;
    assert(roundie_elf_post(generation - 1, 0) == ESP_ERR_INVALID_STATE);
    assert(budget_now().rejected == rejected + 1);

    /* 8: stop with pending events discards them without a callback. */
    uint32_t event_callbacks = runtime_callbacks[2];
    for (uint32_t i = 0; i < 3; i++) {
        assert(roundie_elf_post(generation, 200 + i) == ESP_OK);
    }
    control_step(ROUNDIE_ELF_STOP);
    assert(status.state == ROUNDIE_ELF_STOPPED && runtime_callbacks[2] == event_callbacks);
    counters = budget_now();
    assert(counters.discarded == 3);
    /* Refused: the post from runtime_stop and this one after stop. */
    assert(roundie_elf_post(generation, 1) == ESP_ERR_INVALID_STATE);
    assert(budget_now().rejected == rejected + 3);
    control_step(ROUNDIE_ELF_UNLOAD);
    assert(status.state == ROUNDIE_ELF_UNLOADED && live_bytes == 0 && allocations_live == 0);
    assert(budget_now().discarded == 3);

    /* Boot totals accumulate across instances; the next instance resets. */
    roundie_elf_budget_t boot;
    roundie_elf_budget(&counters, &boot);
    assert(boot.callbacks >= boot_before.callbacks + counters.callbacks);
    assert(boot.dropped == boot_before.dropped + 2 && boot.discarded == boot_before.discarded + 3);
    assert(boot.callback_max_us >= 150000);
    mock_step = 1000;
    value_count = 0;
    puts("Executor budget/mailbox PASS: 8 deterministic cases with exact boundaries; soft budget");
}

/* M9.1 origin classification: overflow first, then PC in the loaded text,
 * then the executor stack and the phase. Attribution to the application
 * needs an image loaded and application code running (load or callback). */
static roundie_elf_crash_report_t classify_case(bool loaded, uint32_t phase, uint32_t trigger, uint32_t pc,
                                                uint32_t sp, uint32_t excvaddr)
{
    roundie_elf_crash_report_t report = {.loaded = loaded, .fault = true};
    report.context = (roundie_elf_crash_context_t){.text_exec = 0x42010000,
        .sections = {{0x0, 0x3c010000, 0x200}, {0x400, 0x3fc90000, 0x40}}};
    report.details.pc = pc;
    report.details.sp = sp;
    report.details.excvaddr = excvaddr;
    report.details.phase = phase;
    report.details.trigger = trigger;
    report.details.stack = (roundie_elf_crash_stack_t){
        .guard_low = 0x3c200000, .stack_low = 0x3c210000, .limit = 0x3c21a000, .stack_high = 0x3c220000, .armed = 1};
    return report;
}

static void crash_classify_tests(void)
{
    const uint32_t app_pc = 0x42010010;
    const uint32_t core_pc = 0x40380000;
    const uint32_t executor_sp = 0x3c21f000;
    const uint32_t red_zone_sp = 0x3c215000;
    const uint32_t guard_sp = 0x3c20fff0;
    const uint32_t other_sp = 0x3fcb0000;
    struct {
        bool loaded;
        uint32_t phase;
        uint32_t trigger;
        uint32_t pc;
        uint32_t sp;
        uint32_t excvaddr;
        roundie_elf_crash_origin_t origin;
    } cases[] = {
        /* 1: the monitor fired while an application callback ran */
        {true, ROUNDIE_ELF_PHASE_CALLBACK, ROUNDIE_ELF_TRIGGER_MONITOR, core_pc, other_sp, 0,
         ROUNDIE_ELF_ORIGIN_APP_STACK_OVERFLOW},
        /* 2: SP in the red zone or the guard page without the monitor (masked) */
        {true, ROUNDIE_ELF_PHASE_CALLBACK, ROUNDIE_ELF_TRIGGER_EXCEPTION, app_pc, red_zone_sp, 0,
         ROUNDIE_ELF_ORIGIN_APP_STACK_OVERFLOW},
        {true, ROUNDIE_ELF_PHASE_LOAD, ROUNDIE_ELF_TRIGGER_EXCEPTION, core_pc, guard_sp, 0,
         ROUNDIE_ELF_ORIGIN_APP_STACK_OVERFLOW},
        /* 3: a load or store into the guard page */
        {true, ROUNDIE_ELF_PHASE_CALLBACK, ROUNDIE_ELF_TRIGGER_EXCEPTION, app_pc, executor_sp, 0x3c208000,
         ROUNDIE_ELF_ORIGIN_APP_STACK_OVERFLOW},
        /* 4: executor overflow with no application code running is the core's */
        {true, ROUNDIE_ELF_PHASE_IDLE, ROUNDIE_ELF_TRIGGER_MONITOR, core_pc, other_sp, 0,
         ROUNDIE_ELF_ORIGIN_EXECUTOR_STACK_OVERFLOW},
        {false, ROUNDIE_ELF_PHASE_LOAD, ROUNDIE_ELF_TRIGGER_MONITOR, core_pc, other_sp, 0,
         ROUNDIE_ELF_ORIGIN_EXECUTOR_STACK_OVERFLOW},
        /* 5: PC in the loaded text, either bus */
        {true, ROUNDIE_ELF_PHASE_CALLBACK, ROUNDIE_ELF_TRIGGER_EXCEPTION, app_pc, executor_sp, 0,
         ROUNDIE_ELF_ORIGIN_APP_CODE},
        {true, ROUNDIE_ELF_PHASE_CALLBACK, ROUNDIE_ELF_TRIGGER_EXCEPTION, 0x3c010010, executor_sp, 0,
         ROUNDIE_ELF_ORIGIN_APP_CODE},
        /* 6: firmware code on the executor during a callback (an API call) */
        {true, ROUNDIE_ELF_PHASE_CALLBACK, ROUNDIE_ELF_TRIGGER_EXCEPTION, core_pc, executor_sp, 0,
         ROUNDIE_ELF_ORIGIN_APP_CALLBACK},
        /* 7: another task or an interrupt while a callback ran: not attributed */
        {true, ROUNDIE_ELF_PHASE_CALLBACK, ROUNDIE_ELF_TRIGGER_EXCEPTION, core_pc, other_sp, 0,
         ROUNDIE_ELF_ORIGIN_CORE_WHILE_APP_ACTIVE},
        /* 8: the monitor fired under another task */
        {true, ROUNDIE_ELF_PHASE_CALLBACK, ROUNDIE_ELF_TRIGGER_FOREIGN, core_pc, other_sp, 0,
         ROUNDIE_ELF_ORIGIN_CORE_WHILE_APP_ACTIVE},
        /* 9: executor between callbacks or unloading */
        {true, ROUNDIE_ELF_PHASE_IDLE, ROUNDIE_ELF_TRIGGER_EXCEPTION, core_pc, executor_sp, 0,
         ROUNDIE_ELF_ORIGIN_EXECUTOR_CORE},
        {true, ROUNDIE_ELF_PHASE_UNLOAD, ROUNDIE_ELF_TRIGGER_EXCEPTION, core_pc, executor_sp, 0,
         ROUNDIE_ELF_ORIGIN_EXECUTOR_CORE},
        /* 10: no image: an application PC is just an address */
        {false, ROUNDIE_ELF_PHASE_CALLBACK, ROUNDIE_ELF_TRIGGER_EXCEPTION, app_pc, executor_sp, 0,
         ROUNDIE_ELF_ORIGIN_EXECUTOR_CORE},
        {false, ROUNDIE_ELF_PHASE_IDLE, ROUNDIE_ELF_TRIGGER_EXCEPTION, core_pc, other_sp, 0,
         ROUNDIE_ELF_ORIGIN_CORE},
        /* 11: stack top is exclusive, the limit is inclusive of usable stack */
        {true, ROUNDIE_ELF_PHASE_CALLBACK, ROUNDIE_ELF_TRIGGER_EXCEPTION, core_pc, 0x3c220000, 0,
         ROUNDIE_ELF_ORIGIN_CORE_WHILE_APP_ACTIVE},
        {true, ROUNDIE_ELF_PHASE_CALLBACK, ROUNDIE_ELF_TRIGGER_EXCEPTION, core_pc, 0x3c21a000, 0,
         ROUNDIE_ELF_ORIGIN_APP_CALLBACK},
        {true, ROUNDIE_ELF_PHASE_CALLBACK, ROUNDIE_ELF_TRIGGER_EXCEPTION, core_pc, 0x3c219ff0, 0,
         ROUNDIE_ELF_ORIGIN_APP_STACK_OVERFLOW},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        roundie_elf_crash_report_t report = classify_case(cases[i].loaded, cases[i].phase, cases[i].trigger,
                                                          cases[i].pc, cases[i].sp, cases[i].excvaddr);
        roundie_elf_crash_origin_t origin = roundie_elf_crash_classify(&report);
        if (origin != cases[i].origin) {
            fprintf(stderr, "classify case %u: %s, expected %s\n", i, roundie_elf_crash_origin_name(origin),
                    roundie_elf_crash_origin_name(cases[i].origin));
        }
        assert(origin == cases[i].origin);
    }
    /* Without a guarded stack (zero layout) nothing is an overflow. */
    roundie_elf_crash_report_t plain = classify_case(true, ROUNDIE_ELF_PHASE_CALLBACK, ROUNDIE_ELF_TRIGGER_EXCEPTION,
                                                     core_pc, 0x3c215000, 0x3c208000);
    plain.details.stack = (roundie_elf_crash_stack_t){0};
    assert(roundie_elf_crash_classify(&plain) == ROUNDIE_ELF_ORIGIN_CORE_WHILE_APP_ACTIVE);
    plain.fault = false;
    assert(roundie_elf_crash_classify(&plain) == ROUNDIE_ELF_ORIGIN_NONE);
    assert(strcmp(roundie_elf_crash_origin_name(ROUNDIE_ELF_ORIGIN_APP_STACK_OVERFLOW), "app_stack_overflow") == 0);
    assert(strcmp(roundie_elf_crash_phase_name(ROUNDIE_ELF_PHASE_UNLOAD), "unload") == 0);
    assert(strcmp(roundie_elf_crash_phase_name(9), "unknown") == 0);

    /* The panic hook path stores the live phase, monitor and layout; a v1
     * record (M9 firmware) reads as absent after the update. */
    roundie_elf_crash_stack_t layout = {0x3c200000, 0x3c210000, 0x3c21a000, 0x3c220000, 1};
    roundie_elf_crash_executor(&layout);
    roundie_elf_crash_phase(ROUNDIE_ELF_PHASE_CALLBACK);
    /* An ignored monitor hit is only counted: it marks no fault. */
    uint32_t spurious = roundie_elf_crash_monitor_spurious_count();
    roundie_elf_crash_monitor_spurious();
    assert(roundie_elf_crash_monitor_spurious_count() == spurious + 1U);
    assert(crash_trigger_now == ROUNDIE_ELF_TRIGGER_EXCEPTION && crash_monitor_pc == 0);
    roundie_elf_crash_monitor(0x42010020, true);
    roundie_elf_crash_fault_t fault = {.pc = 0x40380000, .sp = 0x3fcb0000, .phase = 3, .trigger = 0};
    roundie_elf_crash_fault(&fault);
    assert(crash_valid() && crash_retained.details.phase == ROUNDIE_ELF_PHASE_CALLBACK &&
           crash_retained.details.trigger == ROUNDIE_ELF_TRIGGER_MONITOR &&
           crash_retained.details.monitor_pc == 0x42010020 && crash_retained.details.stack.limit == 0x3c21a000);
    roundie_elf_crash_phase(ROUNDIE_ELF_PHASE_IDLE);
    crash_monitor_pc = 0;
    crash_trigger_now = ROUNDIE_ELF_TRIGGER_EXCEPTION;
    crash_retained.version = 1U;
    crash_retained.crc = crash_checksum(&crash_retained);
    roundie_elf_crash_boot();
    roundie_elf_crash_report_t report;
    assert(!roundie_elf_crash_last(&report) && crash_valid() && crash_retained.version == CRASH_VERSION);
    crash_stack_now = (roundie_elf_crash_stack_t){0};
}

/* Retained crash record (M9): load context, panic details, persistence across
 * simulated restarts until the next load, torn records, address mapping. */
static void crash_tests(void)
{
    memset(&crash_retained, 0xa5, sizeof(crash_retained)); /* power-on garbage */
    roundie_elf_crash_boot();
    roundie_elf_crash_report_t report;
    assert(!roundie_elf_crash_last(&report) && crash_valid() && crash_retained.loaded == 0);

    runtime_mode = 0;
    memset(runtime_callbacks, 0, sizeof(runtime_callbacks));
    value_count = 0;
    roundie_elf_set_allowed(true);
    assert(roundie_elf_prepare());
    uint32_t generation = status.instance;
    unsigned digests_before = digests;
    assert(roundie_elf_execute(fixture, fixture_size));
    assert(status.state == ROUNDIE_ELF_RUNNING && digests == digests_before + 1);
    assert(crash_valid() && crash_retained.loaded == 1 && crash_retained.fault == 0);
    const roundie_elf_crash_context_t *context = &crash_retained.context;
    assert(context->instance == generation && context->digest[1] == (uint8_t)(fixture[1] + 1));
    assert(context->sections[0].addr == (uint32_t)image.sec[ELF_SEC_TEXT].addr);
    assert(context->sections[0].size == image.sec[ELF_SEC_TEXT].size && context->text_exec == context->sections[0].addr);

    /* A panic inside application text, then a restart. */
    roundie_elf_crash_fault_t fault = {.pc = context->sections[0].addr + 4, .exccause = 29, .excvaddr = 0x10,
        .sp = 0x3fc90000, .exception = 4, .core = 1, .depth = 99,
        .frames = {context->sections[0].addr + 4, 0x40380000}};
    roundie_elf_crash_fault(&fault);
    roundie_elf_crash_boot();
    assert(roundie_elf_crash_last(&report) && report.loaded && report.fault);
    assert(report.details.pc == fault.pc && report.details.exccause == 29 && report.details.excvaddr == 0x10);
    assert(report.details.depth == ROUNDIE_ELF_CRASH_FRAMES && report.context.instance == generation);
    uint32_t vaddr = 0;
    assert(roundie_elf_crash_locate(&report.context, report.details.pc, &vaddr) == 0 && vaddr == 4);
    assert(roundie_elf_crash_locate(&report.context, report.details.frames[1], &vaddr) == -1);
    roundie_elf_crash_boot(); /* Safe Mode, then RS1 REBOOT: still reported. */
    assert(roundie_elf_crash_last(&report) && report.fault && report.context.instance == generation);

    /* Unload keeps the fault; the next load clears it. */
    control_step(ROUNDIE_ELF_STOP);
    control_step(ROUNDIE_ELF_UNLOAD);
    assert(status.state == ROUNDIE_ELF_UNLOADED && crash_valid() && crash_retained.loaded == 0 &&
           crash_retained.fault == 1);
    assert(roundie_elf_prepare());
    assert(roundie_elf_execute(fixture, fixture_size));
    assert(crash_retained.loaded == 1 && crash_retained.fault == 0 &&
           crash_retained.context.instance == status.instance);
    control_step(ROUNDIE_ELF_STOP);
    control_step(ROUNDIE_ELF_UNLOAD);
    assert(status.state == ROUNDIE_ELF_UNLOADED && live_bytes == 0 && allocations_live == 0);

    /* A torn record is absent; a panic on a torn record keeps only the details. */
    crash_retained.context.text_exec ^= 1U;
    roundie_elf_crash_boot();
    assert(!roundie_elf_crash_last(&report) && crash_valid() && crash_retained.fault == 0);
    crash_retained.context.instance = 77; /* torn, not resealed */
    roundie_elf_crash_fault(&fault);
    roundie_elf_crash_boot();
    assert(roundie_elf_crash_last(&report) && report.fault && !report.loaded && report.context.instance == 0);

    /* Text matches its data-bus and instruction-bus addresses; ends are exclusive. */
    roundie_elf_crash_context_t map = {.text_exec = 0x42000000,
        .sections = {{0x0, 0x3c000000, 0x100}, {0x300, 0x3fc80000, 0}, {0x280, 0x3fc80100, 0x40},
                     {0x200, 0x3c100000, 0x20}}};
    assert(roundie_elf_crash_locate(&map, 0x42000010, &vaddr) == 0 && vaddr == 0x10);
    assert(roundie_elf_crash_locate(&map, 0x3c000010, &vaddr) == 0 && vaddr == 0x10);
    assert(roundie_elf_crash_locate(&map, 0x3c100004, &vaddr) == 3 && vaddr == 0x204);
    assert(roundie_elf_crash_locate(&map, 0x3fc8013f, &vaddr) == 2 && vaddr == 0x2bf);
    assert(roundie_elf_crash_locate(&map, 0x42000100, &vaddr) == -1);
    assert(roundie_elf_crash_locate(&map, 0x3fc80000, &vaddr) == -1); /* empty bss */
    assert(roundie_elf_crash_locate(&map, 0, &vaddr) == -1);
    crash_classify_tests();
    puts("ELF crash record PASS: context, panic details, restart persistence, torn record, address map, "
         "phase, origin classification");
}

static void runtime_tests(const char *path)
{
    FILE *source = fopen(path, "rb");
    assert(source != NULL);
    fixture_size = fread(fixture, 1, sizeof(fixture), source);
    assert(fclose(source) == 0);
    assert(roundie_elf_submit(fixture, fixture_size) == ESP_ERR_INVALID_ARG);
    clock_leak_tests();
    budget_tests();
    crash_tests();
    for (runtime_mode = 0; runtime_mode <= 11; runtime_mode++) {
        memset(runtime_callbacks, 0, sizeof(runtime_callbacks));
        value_count = 0;
        roundie_elf_set_allowed(true);
        assert(roundie_elf_prepare());
        uint32_t generation = status.instance;
        assert(roundie_elf_execute(fixture, fixture_size));
        if (runtime_mode >= 1 && runtime_mode <= 4) {
            assert(status.state == ROUNDIE_ELF_UNLOADED && status.error[0] != '\0');
            assert(runtime_callbacks[6] == (runtime_mode <= 2 ? 1U : 0U));
            assert(runtime_callbacks[5] == (runtime_mode == 2 ? 1U : 0U));
        } else {
            assert(status.state == ROUNDIE_ELF_RUNNING && live_bytes != 0);
            assert(runtime_callbacks[0] == 1 && runtime_callbacks[1] == 1);
            assert(roundie_elf_control(generation, ROUNDIE_ELF_RESUME) == ESP_ERR_INVALID_STATE);
            assert(roundie_elf_post(generation - 1, 0) == ESP_ERR_INVALID_STATE);
            for (uint32_t i = 0; i < ROUNDIE_ELF_EVENT_CAPACITY; i++) {
                assert(roundie_elf_post(generation, i) == ESP_OK);
            }
            assert(roundie_elf_post(generation, 9) == ESP_ERR_NO_MEM);
            current_task = (void *)2;
            roundie_elf_step(esp_timer_get_time());
            assert(runtime_callbacks[2] == 0);
            current_task = (void *)1;
            for (uint32_t i = 0; i < ROUNDIE_ELF_EVENT_CAPACITY; i++) {
                roundie_elf_step(esp_timer_get_time());
            }
            if (runtime_mode == 5 || runtime_mode == 10) {
                assert(status.state == ROUNDIE_ELF_UNLOADED);
                assert(value_count == 1);
            } else {
                assert(value_count == ROUNDIE_ELF_EVENT_CAPACITY);
                for (unsigned i = 0; i < value_count; i++) {
                    assert(values[i] == i);
                }
                assert(post_event(generation, REP2_EVENT_TIMER, status.timer_handle, mock_clock) == ESP_OK);
                assert(post_event(generation, REP2_EVENT_TIMER, status.timer_handle, mock_clock) == ESP_OK);
                assert(mailbox.count == 1 && mailbox.coalesced == 1);
                assert(roundie_elf_post(generation, 42) == ESP_OK);
                control_step(ROUNDIE_ELF_SUSPEND); /* Serviceable with queued events. */
                if (runtime_mode == 6) {
                    assert(status.state == ROUNDIE_ELF_UNLOADED);
                } else {
                    assert(status.state == ROUNDIE_ELF_SUSPENDED && mailbox.count == 0);
                    assert(roundie_elf_control(generation, ROUNDIE_ELF_SUSPEND) == ESP_ERR_INVALID_STATE);
                    assert(roundie_elf_post(generation, 8) == ESP_ERR_INVALID_STATE);
                    unsigned callbacks_before = status.callbacks;
                    mock_clock += 1000000;
                    roundie_elf_step(esp_timer_get_time());
                    assert(status.callbacks == callbacks_before);
                    if (runtime_mode != 11) {
                        control_step(ROUNDIE_ELF_RESUME);
                    }
                    if (runtime_mode == 7) {
                        assert(status.state == ROUNDIE_ELF_UNLOADED);
                    } else {
                        if (runtime_mode != 11) {
                            mock_clock += 300000;
                            roundie_elf_step(esp_timer_get_time());
                            assert(status.timers_delivered == 1);
                        }
                        control_step(ROUNDIE_ELF_STOP); /* Both running and suspended. */
                        assert(status.state == (runtime_mode == 8 ? ROUNDIE_ELF_UNLOADED : ROUNDIE_ELF_STOPPED));
                        assert(roundie_elf_post(generation, 1) == ESP_ERR_INVALID_STATE);
                        if (status.state != ROUNDIE_ELF_UNLOADED) {
                            control_step(ROUNDIE_ELF_STOP); /* Idempotent, no callback duplication. */
                            assert(runtime_callbacks[5] == 1);
                            control_step(ROUNDIE_ELF_UNLOAD);
                        }
                    }
                }
            }
        }
        assert(status.state == ROUNDIE_ELF_UNLOADED && live_bytes == 0 && allocations_live == 0);
        assert(rep_abi_v2(2) == NULL && runtime_api->timer_cancel(generation, old_timer) == -1);
        unsigned callbacks_before = status.callbacks;
        control_step(ROUNDIE_ELF_UNLOAD);
        assert(status.callbacks == callbacks_before);
        roundie_elf_status_t result;
        roundie_elf_status(&result);
        assert(result.resources_outstanding == 0 && result.queue_depth == 0);
        assert(result.timer_handle == 0 && result.subscriptions == 0);
        assert(result.before.internal_free == result.after.internal_free);
        assert(roundie_elf_post(generation, 42) == ESP_ERR_INVALID_STATE);
        roundie_elf_step(esp_timer_get_time());
        assert(status.callbacks == callbacks_before);
    }
    puts("ABI2 production lifecycle/events/resources PASS: 12 bounded modes; no Xtensa execution on host");
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    FILE *source = fopen(argv[1], "rb");
    assert(source != NULL);
    fixture_size = fread(fixture, 1, sizeof(fixture), source);
    assert(fclose(source) == 0);
    validation_tests();
    assert(!roundie_elf_prepare()); /* Default denied: Safe Mode/pending boot. */
    roundie_elf_set_allowed(true);
    roundie_elf_bind_executor((void *)1);
    for (mode = 0; mode <= 7; mode++) {
        roundie_elf_set_allowed(true);
        assert(roundie_elf_prepare());
        assert(!roundie_elf_prepare());
        current_task = (void *)2;
        assert(!roundie_elf_execute(fixture, fixture_size));
        current_task = (void *)1;
        fail_alloc = mode == 1;
        assert(roundie_elf_execute(fixture, fixture_size));
        fail_alloc = false;
        assert(!roundie_elf_busy() && allocations_live == 0 && live_bytes == 0);
        assert(!roundie_elf_execute(fixture, fixture_size)); /* double execution/unload */
        roundie_elf_status_t result;
        roundie_elf_status(&result);
        assert(result.alloc_bytes == 0);
        if (mode == 0 || mode == 6) {
            assert(result.executed && result.app_result == (mode == 6 ? -7 : 42));
            assert(result.alloc_peak == 128 && result.error[0] == 0);
            assert(result.before.internal_free == result.after.internal_free);
        } else {
            assert(!result.executed && result.error[0] != 0);
        }
    }
    mode = 0;
    roundie_elf_set_allowed(true);
    for (unsigned i = 0; i < 5; i++) {
        assert(roundie_elf_prepare());
        assert(roundie_elf_execute(fixture, fixture_size));
        assert(live_bytes == 0 && allocations_live == 0);
    }
    assert(rep_abi_v1(1) == NULL && app_api->release(previous_instance, (void *)1) == -1);
    assert(roundie_elf_prepare());
    assert(roundie_elf_execute(NULL, 0));
    assert(!roundie_elf_busy());
    fail_queue = true;
    assert(roundie_elf_submit(fixture, fixture_size) == ESP_ERR_NO_MEM);
    fail_queue = false;
    fail_task = true;
    assert(roundie_elf_submit(fixture, fixture_size) == ESP_ERR_NO_MEM);
    fail_task = false;
    fail_send = true;
    assert(roundie_elf_submit(fixture, fixture_size) == ESP_ERR_INVALID_STATE);
    fail_send = false;
    /* M9.1: an executor whose stack guard did not arm refuses every load. */
    atomic_store(&guard_state, GUARD_FAILED);
    assert(roundie_elf_submit(fixture, fixture_size) == ESP_ERR_NOT_SUPPORTED);
    assert(!roundie_elf_busy() && !roundie_elf_input_busy());
    atomic_store(&guard_state, GUARD_PENDING);
    roundie_elf_set_allowed(false);
    assert(roundie_elf_submit(fixture, fixture_size) == ESP_ERR_INVALID_STATE);
    assert(!roundie_elf_busy() && allocations_live == 0);
    runtime_tests(argv[2]);
    printf("ELF production validation/lifecycle PASS; loads=%u deinit=%u callbacks=%u\n",
           load_count, deinit_count, callback_count);
    return 0;
}
