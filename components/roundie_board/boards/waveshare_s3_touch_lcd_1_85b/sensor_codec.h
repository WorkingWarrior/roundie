#pragma once

/* PCF85063 calendar registers and BQ27220 words of this board. */

#include <stdbool.h>
#include <stdint.h>

#include "roundie_rtc_time.h"

bool roundie_rtc_decode(const uint8_t raw[7], roundie_rtc_time_t *time,
                        bool *oscillator_stopped);
bool roundie_rtc_encode(const roundie_rtc_time_t *time, uint8_t raw[7]);
uint16_t roundie_gauge_word(const uint8_t raw[2]);
int16_t roundie_gauge_current(uint16_t raw);
int32_t roundie_gauge_deci_c(uint16_t raw);
