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
#define CRASH_VERSION 2U /* 2: phase, trigger, monitor PC and executor stack (M9.1) */

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

/* Live state copied into a fault by the panic hook; plain DRAM words. */
static volatile uint32_t crash_phase_now;
static volatile uint32_t crash_trigger_now;
static volatile uint32_t crash_monitor_pc;
static volatile uint32_t crash_monitor_spurious;
static roundie_elf_crash_stack_t crash_stack_now;

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
        crash_retained.details.phase <= ROUNDIE_ELF_PHASE_UNLOAD &&
        crash_retained.details.trigger <= ROUNDIE_ELF_TRIGGER_FOREIGN &&
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

void roundie_elf_crash_phase(uint32_t phase)
{
    crash_phase_now = phase;
}

uint32_t roundie_elf_crash_current_phase(void)
{
    return crash_phase_now;
}

void roundie_elf_crash_executor(const roundie_elf_crash_stack_t *stack)
{
    crash_stack_now = *stack;
}

/* Also called by the panic hook. */
void CRASH_IRAM roundie_elf_crash_executor_layout(roundie_elf_crash_stack_t *out)
{
    crash_copy(out, &crash_stack_now, sizeof(*out));
}

void CRASH_IRAM roundie_elf_crash_monitor(uint32_t pc, bool executor)
{
    crash_monitor_pc = pc;
    crash_trigger_now = executor ? ROUNDIE_ELF_TRIGGER_MONITOR : ROUNDIE_ELF_TRIGGER_FOREIGN;
}

void CRASH_IRAM roundie_elf_crash_monitor_spurious(void)
{
    crash_monitor_spurious = crash_monitor_spurious + 1U;
}

uint32_t roundie_elf_crash_monitor_spurious_count(void)
{
    return crash_monitor_spurious;
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
    crash_retained.details.phase = crash_phase_now <= ROUNDIE_ELF_PHASE_UNLOAD ? crash_phase_now : ROUNDIE_ELF_PHASE_IDLE;
    crash_retained.details.trigger = crash_trigger_now <= ROUNDIE_ELF_TRIGGER_FOREIGN ? crash_trigger_now
                                                                                      : ROUNDIE_ELF_TRIGGER_EXCEPTION;
    crash_retained.details.monitor_pc = crash_monitor_pc;
    crash_copy(&crash_retained.details.stack, &crash_stack_now, sizeof(crash_retained.details.stack));
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

static bool crash_on_executor_stack(const roundie_elf_crash_stack_t *stack, uint32_t address)
{
    return stack->stack_high != 0U && address >= stack->guard_low && address < stack->stack_high;
}

/* Below the monitor threshold: red zone or guard page. */
static bool crash_below_limit(const roundie_elf_crash_stack_t *stack, uint32_t address)
{
    return stack->stack_high != 0U && address >= stack->guard_low && address < stack->limit;
}

roundie_elf_crash_origin_t roundie_elf_crash_classify(const roundie_elf_crash_report_t *report)
{
    if (!report->fault) {
        return ROUNDIE_ELF_ORIGIN_NONE;
    }
    const roundie_elf_crash_fault_t *details = &report->details;
    const roundie_elf_crash_stack_t *stack = &details->stack;
    /* Application code can only be running with an image loaded. */
    bool app_running = report->loaded &&
        (details->phase == ROUNDIE_ELF_PHASE_LOAD || details->phase == ROUNDIE_ELF_PHASE_CALLBACK);
    bool overflow = details->trigger == ROUNDIE_ELF_TRIGGER_MONITOR ||
        crash_below_limit(stack, details->sp) ||
        (stack->stack_high != 0U && details->excvaddr >= stack->guard_low && details->excvaddr < stack->stack_low);
    if (overflow) {
        return app_running ? ROUNDIE_ELF_ORIGIN_APP_STACK_OVERFLOW : ROUNDIE_ELF_ORIGIN_EXECUTOR_STACK_OVERFLOW;
    }
    uint32_t vaddr = 0;
    if (report->loaded && roundie_elf_crash_locate(&report->context, details->pc, &vaddr) == 0) {
        return ROUNDIE_ELF_ORIGIN_APP_CODE;
    }
    bool executor = crash_on_executor_stack(stack, details->sp);
    if (app_running && executor) {
        return ROUNDIE_ELF_ORIGIN_APP_CALLBACK;
    }
    if (app_running) {
        return ROUNDIE_ELF_ORIGIN_CORE_WHILE_APP_ACTIVE;
    }
    if (executor) {
        return ROUNDIE_ELF_ORIGIN_EXECUTOR_CORE;
    }
    return ROUNDIE_ELF_ORIGIN_CORE;
}

const char *roundie_elf_crash_origin_name(roundie_elf_crash_origin_t origin)
{
    switch (origin) {
    case ROUNDIE_ELF_ORIGIN_NONE:
        return "none";
    case ROUNDIE_ELF_ORIGIN_CORE:
        return "core";
    case ROUNDIE_ELF_ORIGIN_CORE_WHILE_APP_ACTIVE:
        return "core_while_app_active";
    case ROUNDIE_ELF_ORIGIN_EXECUTOR_CORE:
        return "executor_core";
    case ROUNDIE_ELF_ORIGIN_EXECUTOR_STACK_OVERFLOW:
        return "executor_stack_overflow";
    case ROUNDIE_ELF_ORIGIN_APP_CODE:
        return "app_code";
    case ROUNDIE_ELF_ORIGIN_APP_CALLBACK:
        return "app_callback";
    case ROUNDIE_ELF_ORIGIN_APP_STACK_OVERFLOW:
        return "app_stack_overflow";
    }
    return "unknown";
}

const char *roundie_elf_crash_phase_name(uint32_t phase)
{
    switch (phase) {
    case ROUNDIE_ELF_PHASE_IDLE:
        return "idle";
    case ROUNDIE_ELF_PHASE_LOAD:
        return "load";
    case ROUNDIE_ELF_PHASE_CALLBACK:
        return "callback";
    case ROUNDIE_ELF_PHASE_UNLOAD:
        return "unload";
    }
    return "unknown";
}
