#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* io_worker: the one owner of /storage after startup (docs/storage-io.md, P1).
 * It runs the package, face, launcher and import service loop and executes the
 * forwarded !RP1, !RF1 and !RE1 lines. */

/* Bounded wait for a park at a 4 KiB chunk boundary (requirement 3). */
#define ROUNDIE_IO_PARK_TIMEOUT_US 250000

/* Main task, before system_worker starts. Without CONFIG_ROUNDIE_APP_RUNTIME it
 * does nothing. */
esp_err_t roundie_io_worker_start(void);

/* system_worker: hand a line over without waiting; a full queue answers BUSY. */
void roundie_io_worker_forward(const char *line, bool discard);

/* system_worker: ask io_worker to park between two VFS calls and wait at most
 * timeout_us for it. True once parked (or when there is no io_worker); false
 * on timeout, with the request withdrawn. A successful park lasts until
 * roundie_io_worker_resume. */
bool roundie_io_worker_park(int64_t timeout_us);
void roundie_io_worker_resume(void);
