#pragma once
#include <stdint.h>

/* USB storage mode screen (docs/usb-storage-mode-plan.md, gate G1). GUI task
 * only. The screen is a full-screen object on LVGL's top layer, like the power
 * menu, so it covers the launcher and absorbs touches. */
void roundie_storage_screen_start(void);
/* Once per GUI loop while storage mode is active; handles the exit button and
 * the idle timeout, and restarts into normal mode. */
void roundie_storage_screen_poll(int64_t now_us);
