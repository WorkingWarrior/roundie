#pragma once

#include <stdbool.h>
#include "roundie_board.h"

/* Emulated board of the Linux desktop: the part of roundie_board.h that the
 * power manager uses (docs/roundie-core.md, M2). The GUI thread feeds it the
 * emulated BOOT button and touch panel; the system worker reads them as wake
 * sources, so the state is atomic. */

roundie_board_t *roundie_desktop_board(void);
/* GUI thread, once per iteration. */
void roundie_desktop_board_inputs(roundie_board_t *board, bool boot, bool touched);
/* Whether the panel shows anything: display on and backlight above zero. */
bool roundie_desktop_board_lit(roundie_board_t *board);
