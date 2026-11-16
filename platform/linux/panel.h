#pragma once

/* Control panel of the virtual hardware lab (docs/virtual-hardware.md).
 *
 * A view of the lab, separate from the emulated board: its own frame, drawn
 * with SDL's software renderer and, with --lab-panel, shown in a second window
 * through the desktop's Vulkan presentation. A click only produces a lab
 * command line; the caller applies it through lab_command(), as for a script
 * line, so every panel operation is also a script command. The emulated
 * panel's frame, resolution and GUI are not touched. GUI thread only. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <SDL3/SDL.h>

typedef struct lab_panel lab_panel_t;

/* The frame only (headless): drawing, clicks and screenshots work. NULL and a
 * log line on failure. */
lab_panel_t *lab_panel_create(void);
/* Shows the frame in its own window, zoom pixels per panel pixel. */
bool lab_panel_open_window(lab_panel_t *panel, int zoom);
void lab_panel_destroy(lab_panel_t *panel);

/* The lab command of the control at frame pixel (x, y), from the lab's state
 * now; false when there is none. */
bool lab_panel_command_at(const lab_panel_t *panel, int32_t x, int32_t y, int64_t now_us, char *command,
                          size_t size);
/* Handles an event of the panel's window; false when the event belongs to
 * another window. A click stores its command for lab_panel_take_command(). */
bool lab_panel_event(lab_panel_t *panel, const SDL_Event *event, int64_t now_us);
bool lab_panel_take_command(lab_panel_t *panel, char *command, size_t size);
/* Redraws the frame from the lab's state and shows it in the window, at most
 * every real 100 ms unless forced. False on a presentation error. */
bool lab_panel_update(lab_panel_t *panel, int64_t now_us, bool force);
/* Draws the frame now and writes it as PNG. */
bool lab_panel_save(lab_panel_t *panel, int64_t now_us, const char *path);
