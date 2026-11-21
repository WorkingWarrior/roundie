#pragma once
/* Desktop application runtime (app_runtime.c, docs/native-apps.md N7). */
#include <stdbool.h>
#include <stdint.h>

/* --native-apps: native applications are trusted code in the desktop process;
 * without it every launch is refused. Set before the system worker starts. */
void roundie_desktop_apps_enable(bool enabled);
bool roundie_desktop_apps_enabled(void);

/* System worker, once before the launcher's home is chosen: true, with the
 * firmware's warning, when the previous session ended while an application was
 * loaded: a fault or a killed process. Unlike the board, the desktop cannot
 * tell a kill from a fault. */
bool roundie_desktop_apps_startup(void);
/* System worker on a planned exit: forgets the loaded application. */
void roundie_desktop_apps_shutdown(void);
/* GUI thread, just before a virtual hardware reset replaces the process: a
 * reset after which the board's BootGuard does not blame the loaded
 * application (planned restart, power-on, brownout, reset pin). */
void roundie_desktop_apps_forget_loaded(void);

/* System worker: admission, the unloaded instance's storage, stall reports. */
void roundie_desktop_apps_poll(int64_t now_us);
/* System worker, once before the first poll: the watch face store. */
void roundie_desktop_faces_init(bool fallback_only);
