#pragma once
#include <stdbool.h>

/* RP1 MEMPROBE (E0 baseline, development builds): holds at most one heap
 * block of a known size so the RS1 HEALTH heap figures can be checked against
 * a controlled change. Runs in the existing system worker.
 * Returns false when the operation is not a MEMPROBE command. */
bool roundie_memory_probe_command(const char *operation, const char *id, int parsed);
