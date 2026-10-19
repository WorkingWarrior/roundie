#pragma once

#include "roundie_boot_guard.h"

bool roundie_recovery_active(void);
bool roundie_recovery_skip_gui(void);
/* Existing system worker only, same bounded framer as RT1/RO1. */
void roundie_recovery_poll(int64_t now_us);
void roundie_recovery_command(char *line, bool discard);
/* Already-VALID images can reboot from headless recovery without passing the
 * GUI test; PENDING_VERIFY and in-flight updates remain blocked. */
bool roundie_recovery_restart_allowed(void);
bool roundie_recovery_confirmation_allowed(int ota_state);
