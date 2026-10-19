#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_DEV_PACKAGES
void roundie_package_command(char *line, bool discard);
void roundie_package_poll(int64_t now_us);
#else
static inline void roundie_package_command(char *line, bool discard)
{
    (void)line;
    (void)discard;
}
static inline void roundie_package_poll(int64_t now_us)
{
    (void)now_us;
}
#endif
