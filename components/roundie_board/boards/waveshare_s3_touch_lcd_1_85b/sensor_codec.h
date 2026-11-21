#pragma once

/* PCF85063 calendar registers and BQ27220 words of this board. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "roundie_rtc_time.h"

bool roundie_rtc_decode(const uint8_t raw[7], roundie_rtc_time_t *time,
                        bool *oscillator_stopped);
bool roundie_rtc_encode(const roundie_rtc_time_t *time, uint8_t raw[7]);
uint16_t roundie_gauge_word(const uint8_t raw[2]);
int16_t roundie_gauge_current(uint16_t raw);
int32_t roundie_gauge_deci_c(uint16_t raw);
/* BQ27220 MAC block checksum (TRM SLUUBD4A 2.30): 255 minus the 8-bit sum of
 * the two subcommand address bytes and the block data. */
uint8_t roundie_gauge_block_checksum(uint16_t address, const uint8_t *data,
                                     size_t length);
