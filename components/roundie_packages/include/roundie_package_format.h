#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "roundie_elf_validate.h"

#define ROUNDIE_PACKAGE_HEADER 64U
#define ROUNDIE_PACKAGE_MANIFEST 160U
#define ROUNDIE_PACKAGE_PREFIX (ROUNDIE_PACKAGE_HEADER + ROUNDIE_PACKAGE_MANIFEST)
#define ROUNDIE_PACKAGE_MAX (ROUNDIE_PACKAGE_PREFIX + ROUNDIE_ELF_MAX_FILE)
#define ROUNDIE_PACKAGE_ID 64U
#define ROUNDIE_PACKAGE_NAME 64U
/* Each build accepts only packages of its own target (docs/native-apps.md, N3). */
#if ROUNDIE_TARGET_LINUX_X64
#define ROUNDIE_PACKAGE_TARGET "linux-x64.rep1"
#else
#define ROUNDIE_PACKAGE_TARGET "esp32s3.rep1"
#endif
#define ROUNDIE_PACKAGE_API 1U

typedef struct {
    char id[ROUNDIE_PACKAGE_ID];
    char name[ROUNDIE_PACKAGE_NAME];
    uint16_t version[3];
    uint16_t abi;
    char target[16];
    uint16_t api;
    uint32_t elf_size;
    uint8_t elf_digest[32];
} roundie_package_metadata_t;

bool roundie_package_id_valid(const char *id);
/* Decode serialized bytes, never a native struct cast. Structural validation
 * accepts future ABI/target values so discovery can report INCOMPATIBLE. */
bool roundie_package_decode(const void *prefix, size_t available, size_t total,
                            roundie_package_metadata_t *out,
                            char error[ROUNDIE_ELF_ERROR_SIZE]);
bool roundie_package_compatible(const roundie_package_metadata_t *metadata);
int roundie_package_version_compare(const uint16_t left[3], const uint16_t right[3]);
