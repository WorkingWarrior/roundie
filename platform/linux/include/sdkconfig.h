#pragma once

/* Desktop values of the firmware Kconfig options read by Core code that the
 * desktop compiles. tests/test_desktop.py compares them with the generated
 * firmware configuration (sdkconfig.rep). */

#define CONFIG_ROUNDIE_POWER 1
#define CONFIG_ROUNDIE_POWER_SCREEN_TIMEOUT_SECONDS 30
#define CONFIG_ROUNDIE_POWER_SLEEP_SAFETY_SECONDS 300
