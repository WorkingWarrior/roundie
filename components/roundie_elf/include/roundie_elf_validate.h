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
} roundie_elf_info_t;

bool roundie_elf_import_allowed(const char *name);
/* No allocation or execution. Input may be unaligned. Validation bounds every
 * byte the pinned upstream section loader/relocator may read or write. */
bool roundie_elf_validate(const void *bytes, size_t size, roundie_elf_info_t *out,
                          char error[ROUNDIE_ELF_ERROR_SIZE]);
