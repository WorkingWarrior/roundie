#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "roundie_elf.h"
#include "time_protocol.h"

/* Local time zone (POSIX TZ rule, stored by the platform: NVS namespace
 * "clock" on ESP-IDF) applied to the UTC system clock. Default without a
 * stored rule: "UTC0". */
typedef bool (*roundie_local_wall_t)(uint32_t *sync_count);
void roundie_local_time_init(bool storage_available, roundie_local_wall_t wall);
/* System worker only. Validates, stores, then applies; a storage failure
 * leaves the previous rule active. */
esp_err_t roundie_local_time_set_zone(const char *rule);
void roundie_local_time_zone(char out[ROUNDIE_TIME_ZONE_CAPACITY]);
/* roundie_elf_clock_source_t for the runtime. */
bool roundie_local_time_now(roundie_elf_clock_t *out);
/* Watch faces: local fields and the UTC seconds of the same wall-clock read,
 * so their difference is the zone's current UTC offset. */
bool roundie_local_time_sample(roundie_elf_clock_t *out, int64_t *utc_seconds);
