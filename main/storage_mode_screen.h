#pragma once
#include <stdint.h>

/* USB storage mode screen and session driver (docs/usb-storage-mode-plan.md,
 * gate G2). GUI task only. The screen is a full-screen object on LVGL's top
 * layer, like the power menu, so it covers the launcher and absorbs touches. */

/* Creates the screen and starts the USB drive. A USB failure is shown on the
 * screen and ends the mode after a short time. */
void roundie_storage_screen_start(int64_t now_us);
/* Once per GUI loop while storage mode is active: bus events, ownership policy,
 * the exit button, and the planned restart into normal mode. */
void roundie_storage_screen_poll(int64_t now_us);
