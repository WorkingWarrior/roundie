#pragma once
#include <stdbool.h>
#include <stdint.h>

/* E0 follow-up (development builds only): while armed, records the longest
 * gap between GUI loop iterations and between the wake-ups of four probe
 * tasks, one per core at the GUI/executor priority (1) and one per core at
 * priority 20. A probe that keeps waking every tick shows its core was not
 * stopped; a gap only at priority 1 shows starvation by a higher priority. */

#define ROUNDIE_STALL_PROBES 4U

typedef struct {
    uint32_t gui_iterations;
    uint32_t gui_gap_max_us;  /* between consecutive GUI iteration starts */
    uint32_t gui_late_max_us; /* GUI iteration start after its planned wake-up */
    struct {
        uint8_t core;
        uint8_t priority;
        uint32_t wakes;
        uint32_t gap_max_us;
    } probe[ROUNDIE_STALL_PROBES];
} roundie_stall_report_t;

/* GUI task, once per loop iteration. */
void roundie_stall_gui_mark(int64_t iteration_at_us, uint32_t late_us);
/* System worker. The probe tasks are created on the first arm. */
bool roundie_stall_arm(void);
void roundie_stall_disarm(roundie_stall_report_t *out);

