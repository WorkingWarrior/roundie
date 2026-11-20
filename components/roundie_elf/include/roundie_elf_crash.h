#pragma once

/* Retained native-application crash record (M9). Firmware-internal; not part
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

typedef struct {
    uint32_t pc;
    uint32_t exccause; /* Xtensa EXCCAUSE, or the panic pseudo-cause */
    uint32_t excvaddr;
    uint32_t sp;
    uint32_t exception; /* panic_exception_t */
    uint32_t core;
    uint32_t depth;     /* valid entries in frames */
    uint32_t frames[ROUNDIE_ELF_CRASH_FRAMES];
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
/* Panic handler only: IRAM-safe, no locks, no libc. */
void roundie_elf_crash_fault(const roundie_elf_crash_fault_t *fault);
/* The record found at boot; false when none was valid. */
bool roundie_elf_crash_last(roundie_elf_crash_report_t *out);

/* SHA-256 of the ELF bytes (platform part; ESP-IDF: idf/crash_target.c). */
bool roundie_elf_crash_digest(const uint8_t *bytes, size_t size, uint8_t digest[32]);

/* Map a run-time address to the ELF address space. Returns the section index
 * (0 text, ...) and the ELF virtual address, or -1 when the address is outside
 * the loaded image. Text matches either bus address. */
int roundie_elf_crash_locate(const roundie_elf_crash_context_t *context, uint32_t address,
                             uint32_t *vaddr);
