/* Test helper for tests/test_desktop.py: pointer events into a nested KWin
 * through org_kde_kwin_fake_input. stdin: "move X Y" (logical px), "down",
 * "up", "sleep MS". Built with wayland-scanner from fake-input.xml. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

#include "fake-input.h"

#define BUTTON_LEFT 0x110 /* BTN_LEFT */

static struct org_kde_kwin_fake_input *fake;

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, org_kde_kwin_fake_input_interface.name) == 0) {
        uint32_t bound = version < 4 ? version : 4;
        fake = wl_registry_bind(registry, name, &org_kde_kwin_fake_input_interface, bound);
    }
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener listener = {global, global_remove};

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (display == NULL) {
        fprintf(stderr, "fake-input: no Wayland display\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, NULL);
    wl_display_roundtrip(display);
    if (fake == NULL) {
        fprintf(stderr, "fake-input: the compositor does not offer org_kde_kwin_fake_input\n");
        return 1;
    }
    org_kde_kwin_fake_input_authenticate(fake, "roundie-test", "Roundie desktop input test");
    char line[128];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        double x = 0.0;
        double y = 0.0;
        int ms = 0;
        if (sscanf(line, "move %lf %lf", &x, &y) == 2) {
            org_kde_kwin_fake_input_pointer_motion_absolute(fake, wl_fixed_from_double(x), wl_fixed_from_double(y));
        } else if (strncmp(line, "down", 4) == 0) {
            org_kde_kwin_fake_input_button(fake, BUTTON_LEFT, 1);
        } else if (strncmp(line, "up", 2) == 0) {
            org_kde_kwin_fake_input_button(fake, BUTTON_LEFT, 0);
        } else if (sscanf(line, "sleep %d", &ms) == 1) {
            wl_display_flush(display);
            struct timespec delay = {ms / 1000, (long)(ms % 1000) * 1000000L};
            nanosleep(&delay, NULL);
        }
        if (wl_display_roundtrip(display) < 0) {
            fprintf(stderr, "fake-input: connection lost\n");
            return 1;
        }
    }
    wl_display_disconnect(display);
    return 0;
}
