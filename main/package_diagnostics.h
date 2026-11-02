#pragma once
#include <stdbool.h>
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_DEV_PACKAGES
void roundie_package_command(char *line, bool discard);
#else
static inline void roundie_package_command(char *line, bool discard)
{
    (void)line;
    (void)discard;
}
#endif
