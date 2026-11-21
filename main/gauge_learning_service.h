#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "roundie_board.h"
#include "sdkconfig.h"

/* BQ27220 capacity learning on the 1.85B (docs/bringup.md, "Gauge capacity
 * learning"). System worker only; no lock. Observes every battery sample,
 * keeps the learned Full Charge Capacity in NVS (namespace "gauge", key
 * "learned") when CONFIG_ROUNDIE_GAUGE_DESIGN_CAPACITY_MAH > 0, and holds
 * the system load for one controlled cycle on request. Never writes the
 * gauge: the restore in main.c does, through the board layer. */
#if defined(CONFIG_ROUNDIE_GAUGE_DESIGN_CAPACITY_MAH)
void roundie_gauge_service_sample(roundie_board_t *board, const roundie_battery_sample_t *battery,
                                  int64_t now_us);
/* The FCC to write with Design Capacity design_mah after a gauge reset:
 * the stored learned value when its record is valid for this profile,
 * otherwise design_mah. before: the capacity read just before the write. */
uint16_t roundie_gauge_service_restore_full(const roundie_gauge_capacity_t *before, uint16_t design_mah);
#if CONFIG_ROUNDIE_DEV_POWER
/* minutes 1..1440 arms the load hold; 0 stops it. */
esp_err_t roundie_gauge_service_hold(uint32_t minutes, int64_t now_us);
/* The RG1 LEARN reply object; returns snprintf's result. */
int roundie_gauge_service_json(char *buffer, size_t size, int64_t now_us);
/* The RG1 LEARN LOG reply object: up to 10 telemetry entries from sequence
 * from (or the oldest retained one); -1 when it does not fit. */
int roundie_gauge_service_log_json(char *buffer, size_t size, uint32_t from);
/* The RG1 LEARN END reply object: the end of the hold (warning, EDV2
 * snapshot, result, protection); returns snprintf's result. */
int roundie_gauge_service_end_json(char *buffer, size_t size, int64_t now_us);
#endif
#endif
