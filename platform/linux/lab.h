#pragma once

/* Commands of the virtual hardware lab (docs/virtual-hardware.md). The
 * desktop script and the control panel send the same text lines here, so
 * every panel operation is also a script command. GUI thread only. */

#include <stdint.h>
#include "desktop_input.h"

typedef enum {
    LAB_UNKNOWN,   /* not a lab command */
    LAB_DONE,
    LAB_MALFORMED, /* a lab command with bad arguments */
} lab_result_t;

/* Applies one command line. A key command sets *hold_ms to how long the key
 * stays pressed; otherwise it is 0. */
lab_result_t lab_command(const char *line, roundie_desktop_input_t *input, int64_t now_us, uint32_t *hold_ms);
/* The "hw" status line on stdout. */
void lab_print_status(int64_t now_us);
