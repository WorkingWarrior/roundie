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

void mock_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task; }
int64_t esp_timer_get_time(void) { static int64_t clock = 100000; return clock += 1000; }
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

int esp_elf_relocate(esp_elf_t *elf, const uint8_t *bytes)
{
    (void)bytes;
    load_count++;
    assert(resolve("rep_abi_v1") == (uintptr_t)rep_abi_v1);
    assert(resolve("malloc") == 0 && resolve("xTaskCreate") == 0 && resolve(NULL) == 0);
    elf->ptext = heap_caps_malloc(256, 0);
    if (elf->ptext == NULL) {
        return -12;
    }
    elf->pdata = heap_caps_malloc(128, 0);
    if (mode == 2) {
        return -22;
    }
    uintptr_t first = (uintptr_t)entry < (uintptr_t)callback ? (uintptr_t)entry : (uintptr_t)callback;
    uintptr_t last = (uintptr_t)entry > (uintptr_t)callback ? (uintptr_t)entry : (uintptr_t)callback;
    elf->sec[ELF_SEC_TEXT] = (esp_elf_sec_t){first, last - first + 16};
    elf->sec[ELF_SEC_RODATA] = (esp_elf_sec_t){(uintptr_t)&descriptor, sizeof(descriptor)};
    typedef const rep_app_descriptor_t *(*rep_entry_t)(const rep_api_t *);
    rep_entry_t address = entry;
    memcpy(&elf->entry, &address, sizeof(address));
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
    const char *forbidden[] = {"malloc", "esp_restart", "xTaskCreate", "rep_abi_v2", "rep_abi_v1_suffix", "", NULL};
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

int main(int argc, char **argv)
{
    assert(argc == 2);
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
    printf("ELF production validation/lifecycle PASS; loads=%u deinit=%u callbacks=%u\n",
           load_count, deinit_count, callback_count);
}
