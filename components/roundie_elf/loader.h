#pragma once

/* Loader port of the shared application runtime (docs/native-apps.md, N5).
 *
 * manager.c owns the lifecycle, services and policy; a platform backend owns
 * the one loaded image and the application's heap. ESP-IDF implements it with
 * espressif/elf_loader (idf/loader.c). Every function is called on the
 * executor only, except where noted. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "roundie_elf.h"
#include "roundie_elf_validate.h"

typedef uintptr_t (*roundie_elf_resolver_t)(const char *name);

/* A function pointer of the platform's representation; manager.c converts it
 * to the entry signature of the image's ABI. */
typedef void (*roundie_elf_entry_t)(void);

typedef enum {
    ROUNDIE_ELF_REGION_CODE,     /* executable code */
    ROUNDIE_ELF_REGION_DATA,     /* any loaded non-code part, read-only included */
    ROUNDIE_ELF_REGION_WRITABLE, /* loaded data the application may write */
} roundie_elf_region_t;

/* Free memory, as status.before/relocated/during/after report it. Any thread. */
roundie_elf_memory_t roundie_elf_loader_memory(void);

/* Whether enough memory remains to load an image described by info. */
bool roundie_elf_loader_reserve(const roundie_elf_memory_t *memory, const roundie_elf_info_t *info);

/* Prepare an empty image. 0 on success, else a loader-specific code; after
 * success roundie_elf_loader_unload() must follow, also when relocate fails. */
int roundie_elf_loader_init(void);

/* Load and bind the validated bytes; imports resolve through resolver only.
 * 0 on success, else a loader-specific code. */
int roundie_elf_loader_relocate(const uint8_t *bytes, size_t size, roundie_elf_resolver_t resolver);

/* One log line describing the loaded image. */
void roundie_elf_loader_describe(uint32_t instance, const roundie_elf_info_t *info);

roundie_elf_entry_t roundie_elf_loader_entry(void);

/* Whether [pointer, pointer + bytes) lies inside one part of the image. */
bool roundie_elf_loader_contains(const void *pointer, uint32_t bytes, roundie_elf_region_t region);

/* Release the image initialized by roundie_elf_loader_init(). */
void roundie_elf_loader_unload(void);

/* The application's heap (rep_api_t alloc/release). */
void *roundie_elf_loader_alloc(uint32_t bytes);
void roundie_elf_loader_free(void *pointer);
