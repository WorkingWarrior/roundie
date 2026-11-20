#pragma once

/* Retained native-application crash record (M9, version 2 since M9.1). Firmware-internal; not part
 * of the REP ABI and not a coredump.
 *
 * The loader records the loaded image's identity and section map when an
 * application is relocated and clears it on unload. A panic records the
 * exception PC, cause, address and a short backtrace. The record lives in RTC
 * memory that survives software and panic resets, not power loss, and stays
 * until the next application load, so it is readable after a restart into
 * Safe Mode and after the RS1 REBOOT back. A torn write (a panic during an
 * update) fails the checksum and the record is reported as absent. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROUNDIE_ELF_CRASH_SECTIONS 5U /* loader order: text, bss, data, rodata, drlro */
#define ROUNDIE_ELF_CRASH_FRAMES 8U

typedef struct {
    uint32_t vaddr; /* address in the ELF file's address space */
    uint32_t addr;  /* load address (data bus for text) */
    uint32_t size;
} roundie_elf_crash_section_t;

typedef struct {
    uint32_t instance;
    uint8_t digest[32];  /* SHA-256 of the ELF bytes, as package elf_digest */
    uint32_t text_exec;  /* instruction-bus address of the text section */
    roundie_elf_crash_section_t sections[ROUNDIE_ELF_CRASH_SECTIONS];
} roundie_elf_crash_context_t;

/* What the executor was doing when the panic hit (M9.1). Set by the manager
 * on the executor; read by the panic hook. */
typedef enum {
    ROUNDIE_ELF_PHASE_IDLE = 0,     /* no application code is running */
    ROUNDIE_ELF_PHASE_LOAD = 1,     /* relocation, entry point and v1 on_start */
    ROUNDIE_ELF_PHASE_CALLBACK = 2, /* an application callback is running */
    ROUNDIE_ELF_PHASE_UNLOAD = 3,   /* the image is being released */
} roundie_elf_crash_phase_t;

/* What raised the panic. */
typedef enum {
    ROUNDIE_ELF_TRIGGER_EXCEPTION = 0, /* CPU exception, abort, watchdog, ... */
    ROUNDIE_ELF_TRIGGER_MONITOR = 1,   /* executor stack-pointer monitor (idf/executor_stack.c) */
    ROUNDIE_ELF_TRIGGER_FOREIGN = 2,   /* the monitor fired while another task ran on its core */
} roundie_elf_crash_trigger_t;

/* Classification of a recorded panic (roundie_elf_crash_classify). Only the
 * APP_* origins attribute the panic to the loaded application. */
typedef enum {
    ROUNDIE_ELF_ORIGIN_NONE = 0,                  /* no panic recorded */
    ROUNDIE_ELF_ORIGIN_CORE = 1,                  /* firmware, no application running */
    ROUNDIE_ELF_ORIGIN_CORE_WHILE_APP_ACTIVE = 2, /* firmware off the executor while an app ran */
    ROUNDIE_ELF_ORIGIN_EXECUTOR_CORE = 3,         /* executor stack, no application code running */
    ROUNDIE_ELF_ORIGIN_EXECUTOR_STACK_OVERFLOW = 4, /* executor stack limit crossed, no app code running */
    ROUNDIE_ELF_ORIGIN_APP_CODE = 5,              /* PC inside the loaded application text */
    ROUNDIE_ELF_ORIGIN_APP_CALLBACK = 6,          /* executor, firmware PC, during an app callback */
    ROUNDIE_ELF_ORIGIN_APP_STACK_OVERFLOW = 7,    /* executor stack limit crossed during app code */
} roundie_elf_crash_origin_t;

/* Executor stack, ascending addresses: [guard_low, stack_low) unmapped guard
 * page, [stack_low, limit) red zone, [limit, stack_high) usable stack. All
 * zero when the executor has no guarded stack (host builds). */
typedef struct {
    uint32_t guard_low;
    uint32_t stack_low;
    uint32_t limit;      /* stack-pointer monitor threshold */
    uint32_t stack_high;
    uint32_t armed;      /* 1 when the monitor was armed and self-tested */
} roundie_elf_crash_stack_t;

typedef struct {
    uint32_t pc;
    uint32_t exccause; /* Xtensa EXCCAUSE, or the panic pseudo-cause */
    uint32_t excvaddr;
    uint32_t sp;
    uint32_t exception; /* panic_exception_t */
    uint32_t core;
    uint32_t depth;     /* valid entries in frames */
    uint32_t frames[ROUNDIE_ELF_CRASH_FRAMES];
    uint32_t phase;      /* roundie_elf_crash_phase_t */
    uint32_t trigger;    /* roundie_elf_crash_trigger_t */
    uint32_t monitor_pc; /* PC the stack-pointer monitor latched; 0 if it did not fire */
    uint32_t frame;      /* address of the exception frame; 0 when not read */
    roundie_elf_crash_stack_t stack;
} roundie_elf_crash_fault_t;

typedef struct {
    bool loaded;  /* an application was loaded when the record was last written */
    bool fault;   /* a panic was recorded since the last application load */
    roundie_elf_crash_context_t context;
    roundie_elf_crash_fault_t details;
} roundie_elf_crash_report_t;

/* Once at boot, before any application load: keep a copy of the retained
 * record for roundie_elf_crash_last(). */
void roundie_elf_crash_boot(void);
/* Executor: the image is relocated / about to be released. */
void roundie_elf_crash_loaded(const roundie_elf_crash_context_t *context);
void roundie_elf_crash_unloaded(void);
/* Executor: what it is doing now (roundie_elf_crash_phase_t). A plain
 * store, read by the panic hook. */
void roundie_elf_crash_phase(uint32_t phase);
uint32_t roundie_elf_crash_current_phase(void);
/* Once, when the executor stack is set up and again when its monitor is
 * armed: the layout copied into every later fault. */
void roundie_elf_crash_executor(const roundie_elf_crash_stack_t *stack);
void roundie_elf_crash_executor_layout(roundie_elf_crash_stack_t *out);
/* Stack-pointer monitor interrupt only (IRAM-safe): the PC it latched and
 * whether the executor was the running task, just before it aborts. */
void roundie_elf_crash_monitor(uint32_t pc, bool executor);
/* Same interrupt: a trigger with no stack pointer below the limit, ignored;
 * the count is reported by RS1 HEALTH. */
void roundie_elf_crash_monitor_spurious(void);
uint32_t roundie_elf_crash_monitor_spurious_count(void);
/* Panic handler only: IRAM-safe, no locks, no libc. Adds the phase, the
 * monitor state and the executor layout to the given details. */
void roundie_elf_crash_fault(const roundie_elf_crash_fault_t *fault);
/* Where the recorded panic came from; pure function of the report. */
roundie_elf_crash_origin_t roundie_elf_crash_classify(const roundie_elf_crash_report_t *report);
/* Short stable names for RS1 CRASH and the host tool. */
const char *roundie_elf_crash_origin_name(roundie_elf_crash_origin_t origin);
const char *roundie_elf_crash_phase_name(uint32_t phase);
/* The record found at boot; false when none was valid. */
bool roundie_elf_crash_last(roundie_elf_crash_report_t *out);

/* SHA-256 of the ELF bytes (platform part; ESP-IDF: idf/crash_target.c). */
bool roundie_elf_crash_digest(const uint8_t *bytes, size_t size, uint8_t digest[32]);

/* Map a run-time address to the ELF address space. Returns the section index
 * (0 text, ...) and the ELF virtual address, or -1 when the address is outside
 * the loaded image. Text matches either bus address. */
int roundie_elf_crash_locate(const roundie_elf_crash_context_t *context, uint32_t address,
                             uint32_t *vaddr);
