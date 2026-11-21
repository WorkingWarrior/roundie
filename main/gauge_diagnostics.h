#pragma once
#include <stdbool.h>
#include "roundie_board.h"

/* !RG1 development transport (CONFIG_ROUNDIE_DEV_POWER), system worker only:
 *   !RG1 <8 hex id> STATUS          read-only gauge capacity configuration
 *   !RG1 <8 hex id> CAPACITY <mAh>  Design Capacity and Full Charge Capacity
 *                                   in the gauge's RAM (lost at gauge reset)
 *   !RG1 <8 hex id> LEARN           capacity learning report (read-only)
 *   !RG1 <8 hex id> LEARN HOLD <m>  hold the system load 1..1440 min for one
 *                                   controlled cycle; ends by itself
 *   !RG1 <8 hex id> LEARN STOP      end the hold
 *   !RG1 <8 hex id> LEARN LOG <n>   up to 10 RAM telemetry entries from
 *                                   sequence n (read after USB reconnect)
 * Reply: !RG1 <id> <esp_err> {json}. board may be NULL (not ready). */
void roundie_gauge_command(roundie_board_t *board, const char *line, bool discard);
