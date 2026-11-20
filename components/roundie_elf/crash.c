#include "roundie_elf_crash.h"

#include <stddef.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define CRASH_IRAM IRAM_ATTR
#else
#define RTC_NOINIT_ATTR
#define CRASH_IRAM
#endif

#define CRASH_MAGIC UINT32_C(0x52435231) /* "RCR1" */
#define CRASH_VERSION 1U

/* Private, same-image RTC record; separate from the Boot Guard record. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t loaded;
    uint32_t fault;
    roundie_elf_crash_context_t context;
    roundie_elf_crash_fault_t details;
    uint32_t crc;
} crash_record_t;

static RTC_NOINIT_ATTR crash_record_t crash_retained;
static roundie_elf_crash_report_t crash_last;
static bool crash_last_valid;

/* Byte loops instead of libc: the panic path may run with Flash cache off. */
static CRASH_IRAM uint32_t crash_checksum(const crash_record_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < offsetof(crash_record_t, crc); i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

static CRASH_IRAM void crash_copy(void *destination, const void *source, size_t size)
{
    uint8_t *to = destination;
    const uint8_t *from = source;
    for (size_t i = 0; i < size; i++) {
        to[i] = from[i];
    }
}

static bool crash_valid(void)
{
    return crash_retained.magic == CRASH_MAGIC && crash_retained.version == CRASH_VERSION &&
        crash_retained.loaded <= 1U && crash_retained.fault <= 1U && crash_retained.details.depth <= ROUNDIE_ELF_CRASH_FRAMES &&
        crash_retained.crc == crash_checksum(&crash_retained);
}

static void crash_reset(void)
{
    crash_record_t empty = {0};
    crash_retained = empty;
    crash_retained.magic = CRASH_MAGIC;
    crash_retained.version = CRASH_VERSION;
    crash_retained.crc = crash_checksum(&crash_retained);
}

void roundie_elf_crash_boot(void)
{
    crash_last_valid = crash_valid();
    if (crash_last_valid) {
        crash_last.loaded = crash_retained.loaded != 0U;
        crash_last.fault = crash_retained.fault != 0U;
        crash_last.context = crash_retained.context;
        crash_last.details = crash_retained.details;
    } else {
        crash_reset();
    }
}

void roundie_elf_crash_loaded(const roundie_elf_crash_context_t *context)
{
    crash_reset();
    crash_retained.loaded = 1U;
    crash_retained.context = *context;
    crash_retained.crc = crash_checksum(&crash_retained);
}

void roundie_elf_crash_unloaded(void)
{
    if (!crash_valid()) {
        crash_reset();
    }
    crash_retained.loaded = 0U;
    crash_retained.crc = crash_checksum(&crash_retained);
}

void CRASH_IRAM roundie_elf_crash_fault(const roundie_elf_crash_fault_t *fault)
{
    bool intact = crash_retained.magic == CRASH_MAGIC && crash_retained.version == CRASH_VERSION &&
        crash_retained.crc == crash_checksum(&crash_retained);
    if (!intact) {
        /* Not initialised this boot, or torn by this panic: keep the
         * details, without an application context. */
        uint8_t *bytes = (uint8_t *)&crash_retained;
        for (size_t i = 0; i < sizeof(crash_retained); i++) {
            bytes[i] = 0;
        }
        crash_retained.magic = CRASH_MAGIC;
        crash_retained.version = CRASH_VERSION;
    }
    crash_copy(&crash_retained.details, fault, sizeof(crash_retained.details));
    if (crash_retained.details.depth > ROUNDIE_ELF_CRASH_FRAMES) {
        crash_retained.details.depth = ROUNDIE_ELF_CRASH_FRAMES;
    }
    crash_retained.fault = 1U;
    crash_retained.crc = crash_checksum(&crash_retained);
}

bool roundie_elf_crash_last(roundie_elf_crash_report_t *out)
{
    if (crash_last_valid) {
        *out = crash_last;
    }
    return crash_last_valid;
}

int roundie_elf_crash_locate(const roundie_elf_crash_context_t *context, uint32_t address,
                             uint32_t *vaddr)
{
    for (unsigned i = 0; i < ROUNDIE_ELF_CRASH_SECTIONS; i++) {
        const roundie_elf_crash_section_t *section = &context->sections[i];
        if (section->size == 0U) {
            continue;
        }
        uint32_t base = section->addr;
        if (i == 0U && address - context->text_exec < section->size) {
            base = context->text_exec;
        }
        if (address - base < section->size) {
            *vaddr = section->vaddr + (address - base);
            return (int)i;
        }
    }
    return -1;
}
