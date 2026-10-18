#pragma once

#include "roundie_ota.h"

/* Boot thread before worker starts. Failure is reported without confirmation. */
void roundie_ota_diagnostics_init(void);
void roundie_ota_startup_failed(void);
/* GUI only publishes evidence; Flash operations belong to the existing worker. */
void roundie_ota_gui_health(bool passed);
void roundie_ota_snapshot(roundie_ota_status_t *out);
/* Existing serial/sensor worker only. */
void roundie_ota_poll(int64_t now_us);
void roundie_ota_command(char *line, bool discard);
bool roundie_ota_restart_allowed(void);
