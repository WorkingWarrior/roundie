#pragma once
#include <stdbool.h>
#include "sdkconfig.h"

/* RF1 development face store service (docs/watch-face-packages.md). Runs on
 * the system worker like RP1; no authentication or signing. */
#if CONFIG_ROUNDIE_DEV_PACKAGES
void roundie_face_command(char *line, bool discard);
#else
static inline void roundie_face_command(char *line, bool discard)
{
    (void)line;
    (void)discard;
}
#endif
