#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Boot-time serial diagnostics, before GUI ownership starts. Nonfatal. */
bool roundie_storage_diagnostics(void);
/* Mount is boot-owned and remains mounted for the lifetime of normal mode. */
bool roundie_storage_available(void);
bool roundie_storage_usage(size_t *total, size_t *used);
