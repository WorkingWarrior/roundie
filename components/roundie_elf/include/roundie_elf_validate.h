#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROUNDIE_ELF_MAX_FILE 65536U
#define ROUNDIE_ELF_MAX_CODE 16384U
#define ROUNDIE_ELF_MAX_DATA 16384U
#define ROUNDIE_ELF_ERROR_SIZE 80

typedef struct {
    uint32_t address;
    uint32_t offset;
    uint32_t size;
} roundie_elf_section_t;

typedef struct {
    roundie_elf_section_t sections[5]; /* text, bss, data, rodata, data.rel.ro */
    uint32_t code_bytes;
    uint32_t data_bytes; /* includes worst-case upstream alignment padding */
    uint32_t entry;
    uint32_t relocations;
    uint32_t imports;
    uint32_t abi_version;
} roundie_elf_info_t;

/* Import policy shared by every target's validator. */
bool roundie_elf_import_allowed(const char *name);
bool roundie_elf_import_matches(const char *name, uint32_t abi_version);
/* No allocation or execution. Input may be unaligned. Validation bounds every
 * byte the pinned upstream section loader/relocator may read or write. */
bool roundie_elf_validate_xtensa(const void *bytes, size_t size, roundie_elf_info_t *out,
                                 char error[ROUNDIE_ELF_ERROR_SIZE]);
/* linux-x64.rep1 (docs/native-apps.md, N2): the same policy applied to what
 * ld.so reads, the program headers and the dynamic segment. sections[0] is the
 * code segment; the other sections are not reported. A valid ELF is not safe
 * to run: validation only keeps the loader from binding anything outside the
 * REP ABI or running code before the entry is called. */
bool roundie_elf_validate_x86_64(const void *bytes, size_t size, roundie_elf_info_t *out,
                                 char error[ROUNDIE_ELF_ERROR_SIZE]);
/* The build's own target: x86-64 when ROUNDIE_TARGET_LINUX_X64 is set (the
 * desktop), Xtensa otherwise. */
bool roundie_elf_validate(const void *bytes, size_t size, roundie_elf_info_t *out,
                          char error[ROUNDIE_ELF_ERROR_SIZE]);
