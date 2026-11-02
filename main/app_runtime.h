#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Application runtime admission, independent of the development transports.
 * All entry points belong to the existing system worker. */
bool roundie_app_runtime_allowed(void); /* Confirmed normal/STABLE policy. */
bool roundie_app_runtime_busy(void); /* Native, package or staged work remains. */
bool roundie_app_runtime_upload_busy(void);
bool roundie_app_runtime_admit(void);
void roundie_app_runtime_poll(int64_t now_us);
