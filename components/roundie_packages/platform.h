#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Fixed integration seams for IDF memory/admission/capacity. Host tests replace
 * these functions, while using the production VFS transaction and validator. */
bool roundie_packages_allowed(void);
bool roundie_packages_runtime_busy(void);
bool roundie_packages_capacity(size_t *free_bytes);
int64_t roundie_packages_clock(void);
void *roundie_packages_allocate(size_t size);
void roundie_packages_free(void *memory);
int roundie_packages_execute(uint8_t *bytes, size_t size, uint16_t abi);
