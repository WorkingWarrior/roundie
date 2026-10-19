#pragma once
#include <stdbool.h>

/* Boot-time serial diagnostics, before GUI ownership starts. Nonfatal. */
bool roundie_storage_diagnostics(void);
/* Mount is boot-owned and remains mounted for the lifetime of normal mode. */
bool roundie_storage_available(void);
