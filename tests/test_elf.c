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
static unsigned runtime_mode;
static const rep_api_v2_t *runtime_api;
static uint32_t runtime_callbacks[7];
static uint32_t values[32];
static unsigned value_count;
static uint32_t old_timer;
static void *runtime_memory;

void mock_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task; }
int64_t esp_timer_get_time(void) { return mock_clock += 1000; }
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
    assert(runtime_api->timer_start(context->instance, 250) != 0);
    return runtime_mode == 2 ? -12 : 0;
}

static int32_t runtime_event(const rep_context_v2_t *context, const rep_event_v2_t *event)
{
    runtime_callbacks[2]++;
    assert(callback_active);
    unsigned before = runtime_callbacks[2];
    roundie_elf_step(esp_timer_get_time()); /* Callback recursion is refused. */
    assert(before == runtime_callbacks[2]);
    if (event->type == REP2_EVENT_USER) {
        assert(value_count < 32);
        values[value_count++] = event->value;
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
    assert(runtime_api->alloc(context->instance, 8) == NULL);
    assert(roundie_elf_post(context->instance, 8) == ESP_ERR_INVALID_STATE);
    return runtime_mode == 8 ? -18 : 0;
}
static int32_t runtime_destroy(const rep_context_v2_t *context)
{
    runtime_callbacks[6]++;
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
    elf->sec[ELF_SEC_TEXT] = (esp_elf_sec_t){first, last - first + 16};
    elf->sec[ELF_SEC_RODATA] = (esp_elf_sec_t){(uintptr_t)&descriptor, sizeof(descriptor)};
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
        elf->sec[ELF_SEC_TEXT] = (esp_elf_sec_t){first, last - first + 16};
        elf->sec[ELF_SEC_RODATA] = (esp_elf_sec_t){(uintptr_t)&runtime_descriptor, sizeof(runtime_descriptor)};
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
int xTaskCreate(void (*function)(void *), const char *name, unsigned stack, void *arg,
                 unsigned priority, TaskHandle_t *out)
{
    (void)function; (void)arg;
    assert(strcmp(name, "rep_executor") == 0 && stack == 12288 && priority == 1);
    *out = fail_task ? NULL : (void *)1;
    return !fail_task;
}
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

static void runtime_tests(const char *path)
{
    FILE *source = fopen(path, "rb");
    assert(source != NULL);
    fixture_size = fread(fixture, 1, sizeof(fixture), source);
    assert(fclose(source) == 0);
    assert(roundie_elf_submit(fixture, fixture_size) == ESP_ERR_INVALID_ARG);
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
    roundie_elf_set_allowed(false);
    assert(roundie_elf_submit(fixture, fixture_size) == ESP_ERR_INVALID_STATE);
    assert(!roundie_elf_busy() && allocations_live == 0);
    runtime_tests(argv[2]);
    printf("ELF production validation/lifecycle PASS; loads=%u deinit=%u callbacks=%u\n",
           load_count, deinit_count, callback_count);
    return 0;
}
