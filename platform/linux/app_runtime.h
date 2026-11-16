#pragma once
/* Desktop application runtime (app_runtime.c, docs/native-apps.md N7). */
#include <stdbool.h>
#include <stdint.h>

/* --native-apps: native applications are trusted code in the desktop process;
 * without it every launch is refused. Set before the system worker starts. */
void roundie_desktop_apps_enable(bool enabled);
bool roundie_desktop_apps_enabled(void);

/* System worker: admission, the unloaded instance's storage, stall reports. */
void roundie_desktop_apps_poll(int64_t now_us);
