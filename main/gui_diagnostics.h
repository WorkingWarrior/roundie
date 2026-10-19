#pragma once
#include <stdbool.h>
#include <stdint.h>

/* GUI-owned per-iteration counters. No ISR locks, allocations or logging. */
typedef struct {
    uint64_t flushes;
    uint64_t done;
    uint64_t pixels;
    uint64_t submit_us;
    uint64_t dma_us;
    uint64_t wait_us;
    uint64_t render_us;
    uint64_t invalidations;
    uint64_t invalid_pixels;
    uint64_t full_invalidations;
    uint32_t dma_max_us;
    uint32_t submit_max_us;
    uint32_t wait_max_us;
} roundie_gui_work_t;

unsigned roundie_gui_batch(void);
void roundie_gui_record(int64_t at_us, uint32_t pump_us, uint32_t handler_us,
    uint32_t wake_us, uint32_t before, uint32_t after, uint32_t applied,
    const roundie_gui_work_t *work, uint32_t errors, uint32_t isr_done);
/* Existing worker; bounded RP1 reply. BATCH 4/12 is volatile and idle-only. */
bool roundie_gui_command(const char *operation, const char *id, int parsed);
