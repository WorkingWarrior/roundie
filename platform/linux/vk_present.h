#pragma once

/* Vulkan presentation of the emulated panel (docs/roundie-core.md, D4).
 * The panel frame is a CPU buffer of B8G8R8A8 pixels; each present copies it
 * into a device image and blits it, nearest-neighbour and centred, into the
 * window's swapchain image. GUI thread only. */

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>

typedef struct vk_present vk_present_t;

/* width x height is the panel frame. Returns NULL and logs on failure. */
vk_present_t *vk_present_create(SDL_Window *window, uint32_t width, uint32_t height);
/* Copies pixels (width * height, B8G8R8A8) and presents them. False on an
 * unrecoverable device error. A resized or minimised window is handled. */
bool vk_present_frame(vk_present_t *present, const uint32_t *pixels);
/* The window's pixel size changed: the next frame recreates the swapchain.
 * Not every WSI reports a resize through OUT_OF_DATE or SUBOPTIMAL (the
 * Gamescope WSI layer does not). */
void vk_present_resized(vk_present_t *present);
/* Window pixel position to panel pixel, by the layout of the last present;
 * clamped to the panel. Returns whether the position is on the panel. */
bool vk_present_map(const vk_present_t *present, float x, float y, int32_t *panel_x, int32_t *panel_y);
/* Name of the device in use, for the log. */
const char *vk_present_device(const vk_present_t *present);
void vk_present_destroy(vk_present_t *present);
