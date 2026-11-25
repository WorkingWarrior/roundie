#pragma once
#include <stdbool.h>

/* RP1 FSBENCH (E0 baseline, development builds): LittleFS read and SHA-256
 * timing of a deterministic test file. Runs in the existing system worker.
 * Returns false when the operation is not an FSBENCH command. */
bool roundie_storage_bench_command(const char *operation, const char *id, int parsed);
