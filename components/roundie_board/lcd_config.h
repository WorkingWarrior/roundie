#pragma once

#include "sdkconfig.h"

/* Compile-time profiles only; no NVS tuning or live bus reconfiguration. */
#define ROUNDIE_LCD_READ_HZ 3000000
#define ROUNDIE_LCD_WRITE_HZ CONFIG_ROUNDIE_LCD_WRITE_HZ
#if ROUNDIE_LCD_WRITE_HZ != 3000000 && ROUNDIE_LCD_WRITE_HZ != 10000000 && \
    ROUNDIE_LCD_WRITE_HZ != 20000000 && ROUNDIE_LCD_WRITE_HZ != 30000000 && \
    ROUNDIE_LCD_WRITE_HZ != 40000000
#error "Unsupported 1.85B LCD write clock"
#endif

#define ROUNDIE_LCD_WIDTH 360
#define ROUNDIE_LCD_HEIGHT 360
#define ROUNDIE_LCD_STRIP_LINES 20
#define ROUNDIE_LCD_TRANSFER_BYTES (ROUNDIE_LCD_WIDTH * ROUNDIE_LCD_STRIP_LINES * 2)

_Static_assert(ROUNDIE_LCD_TRANSFER_BYTES == 14400, "Retain internal DMA strip size");
