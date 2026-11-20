/* ESP-IDF backend of the loader port (loader.h): espressif/elf_loader with a
 * strict symbol resolver, application heap in internal RAM. Moved unchanged
 * from manager.c. */
#include "../loader.h"
#include <inttypes.h>
#include <string.h>
#include "esp_elf.h"
#include "private/elf_platform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "roundie_elf_crash.h"
#include "sdkconfig.h"

static esp_elf_t image;
static roundie_elf_crash_context_t crash_context;

roundie_elf_memory_t roundie_elf_loader_memory(void)
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

bool roundie_elf_loader_reserve(const roundie_elf_memory_t *memory, const roundie_elf_info_t *info)
{
    return memory->internal_free >= 49152U &&
        memory->psram_free >= 65536U + info->code_bytes + info->data_bytes &&
        memory->exec_largest >= info->code_bytes;
}

int roundie_elf_loader_init(void)
{
    return esp_elf_init(&image);
}

int roundie_elf_loader_relocate(const uint8_t *bytes, size_t size, roundie_elf_resolver_t resolver)
{
    /* size was validated by the caller; esp_elf reads the headers */
    /* Permanent strict hook; never reset to upstream's broad default tables. */
    elf_set_symbol_resolver(resolver);
    memset(&crash_context, 0, sizeof(crash_context));
    if (!roundie_elf_crash_digest(bytes, size, crash_context.digest)) {
        memset(crash_context.digest, 0, sizeof(crash_context.digest)); /* recorded as unknown */
    }
    return esp_elf_relocate(&image, bytes);
}

void roundie_elf_loader_describe(uint32_t instance, const roundie_elf_info_t *info)
{
    ESP_LOGI("elf", "instance=%" PRIu32 " text_write=%p text_execute=%p data=%p code=%" PRIu32
             " data_allocation=%" PRIu32 " relocations=%" PRIu32,
             instance, image.ptext, (void *)image.entry, image.pdata,
             info->code_bytes, info->data_bytes, info->relocations);
    /* Retained for crash reports: identity and section map of this image. */
    crash_context.instance = instance;
    for (unsigned i = 0; i < ELF_SECS && i < ROUNDIE_ELF_CRASH_SECTIONS; i++) {
        crash_context.sections[i] = (roundie_elf_crash_section_t){
            (uint32_t)image.sec[i].v_addr, (uint32_t)image.sec[i].addr, (uint32_t)image.sec[i].size};
    }
    crash_context.text_exec = (uint32_t)image.sec[ELF_SEC_TEXT].addr;
#if CONFIG_ELF_LOADER_CACHE_OFFSET
    crash_context.text_exec = (uint32_t)elf_remap_text(&image, image.sec[ELF_SEC_TEXT].addr);
#endif
    roundie_elf_crash_loaded(&crash_context);
}

roundie_elf_entry_t roundie_elf_loader_entry(void)
{
    roundie_elf_entry_t entry;
    _Static_assert(sizeof(entry) == sizeof(image.entry), "Loader entry pointer representation");
    memcpy(&entry, &image.entry, sizeof(entry));
    return entry;
}

bool roundie_elf_loader_contains(const void *pointer, uint32_t bytes, roundie_elf_region_t region)
{
    bool code = region == ROUNDIE_ELF_REGION_CODE;
    uintptr_t address = (uintptr_t)pointer;
    for (unsigned i = 0; i < ELF_SECS; i++) {
        if ((i == ELF_SEC_TEXT) != code) {
            continue;
        }
        if (region == ROUNDIE_ELF_REGION_WRITABLE && i == ELF_SEC_RODATA) {
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

void roundie_elf_loader_unload(void)
{
    roundie_elf_crash_unloaded();
    esp_elf_deinit(&image);
}

void *roundie_elf_loader_alloc(uint32_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void roundie_elf_loader_free(void *pointer)
{
    heap_caps_free(pointer);
}
