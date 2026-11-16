#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "roundie_board.h"

/* Wall-clock validity (docs/rtc-time-sync.md, docs/roundie-core.md M3).
 *
 * The system wall clock is UTC and valid only after a trusted sync: from the
 * external RTC (ROUNDIE_BOARD_CAP_RTC, PCF85063 on the 1.85B), or on a board
 * without one from an explicit SET, which a software reset keeps
 * (clock_retention.h). Validity never depends on the RTC being present.
 * Wall time is never used for timeouts or intervals: those use the monotonic
 * roundie_time_us(). Hardware and storage are behind roundie_clock_port.h.
 *
 * Threads: init() runs before the system worker exists; start() may run while
 * the worker already serves request() with no board (see request()); poll()
 * and request() run on the system worker; valid() and status() may be called
 * from any task. */

typedef struct {
    bool valid;          /* trusted and published */
    uint32_t sync_count; /* trusted syncs since start, 0 before publication */
    int64_t sync_at_us;  /* roundie_time_us() of the last sync, 0 before publication */
} roundie_wall_clock_status_t;

/* Before the system worker starts: whether the board has an external RTC. */
void roundie_wall_clock_init(bool board_has_rtc);
/* Startup. With an RTC, trusts the startup sample; without one, keeps the
 * system time only after a reset that kept the RTC domain and with an
 * intact retention record. Returns the validity. */
bool roundie_wall_clock_start(const roundie_rtc_sample_t *rtc);
/* Makes the validity visible to valid() and status(), once the board is
 * published. */
void roundie_wall_clock_publish(void);
/* System worker, every 5 s, with a fresh RTC sample: sync from a valid RTC,
 * invalidate after an RTC failure, keep the retention record aligned. */
void roundie_wall_clock_poll(const roundie_rtc_sample_t *rtc);
/* System worker: RT1 SET and GET. result is "OK" or an earlier refusal
 * such as "OTA_BUSY"; a SET runs only when it is "OK". The RTC is read
 * afterwards in any case and returned in *rtc. Returns the result string.
 * board is NULL until the board is published, which is after start(); such
 * a request changes nothing, so start() cannot overwrite an acknowledged SET:
 * a SET is refused with "CLOCK" and ESP_ERR_INVALID_STATE, and a GET on a
 * board with an RTC reports "RTC_READ" with the driver's error. */
const char *roundie_wall_clock_request(roundie_board_t *board, const char *result, bool set, int64_t seconds,
                                       roundie_rtc_sample_t *rtc, esp_err_t *error);
/* roundie_local_wall_t for local_time.c. */
bool roundie_wall_clock_valid(uint32_t *sync_count);
void roundie_wall_clock_status(roundie_wall_clock_status_t *out);
