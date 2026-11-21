#pragma once
#include <stdbool.h>
#include "roundie_board.h"

/* !RG1 development transport (CONFIG_ROUNDIE_DEV_POWER), system worker only:
 *   !RG1 <8 hex id> STATUS          read-only gauge capacity configuration
 *   !RG1 <8 hex id> CAPACITY <mAh>  Design Capacity and Full Charge Capacity
 *                                   in the gauge's RAM (lost at gauge reset)
 * Reply: !RG1 <id> <esp_err> {json}. board may be NULL (not ready). */
void roundie_gauge_command(roundie_board_t *board, const char *line, bool discard);
