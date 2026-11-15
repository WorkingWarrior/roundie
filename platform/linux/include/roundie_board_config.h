#pragma once

/* Emulated board of the Linux desktop (platform/linux/board.c). The panel
 * size is chosen at run time with --board, so ROUNDIE_BOARD_DISPLAY_* are not
 * defined here; the desktop's profiles in desktop.c mirror the real boards. */

#define ROUNDIE_BOARD_NAME "desktop"
#define ROUNDIE_BOARD_CAPABILITIES (ROUNDIE_BOARD_CAP_TOUCH | ROUNDIE_BOARD_CAP_ROUND_DISPLAY)
