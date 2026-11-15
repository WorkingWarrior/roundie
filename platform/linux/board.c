#include "board.h"

#include <stdatomic.h>
#include <stdio.h>

struct roundie_board {
    atomic_bool display_on;
    atomic_uint backlight_percent;
    atomic_bool boot;
    atomic_bool touched;
};

static struct roundie_board board = {
    .display_on = true,
    .backlight_percent = 100,
};

roundie_board_t *roundie_desktop_board(void)
{
    return &board;
}

void roundie_desktop_board_inputs(roundie_board_t *target, bool boot, bool touched)
{
    atomic_store(&target->boot, boot);
    atomic_store(&target->touched, touched);
}

bool roundie_desktop_board_lit(roundie_board_t *target)
{
    return atomic_load(&target->display_on) && atomic_load(&target->backlight_percent) > 0;
}

esp_err_t roundie_board_backlight_set(roundie_board_t *target, uint8_t percent)
{
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store(&target->backlight_percent, percent);
    return ESP_OK;
}

esp_err_t roundie_board_display_power(roundie_board_t *target, bool on)
{
    atomic_store(&target->display_on, on);
    fprintf(stderr, "roundie-desktop: panel %s\n", on ? "on" : "off");
    return ESP_OK;
}

/* The emulated panel keeps its memory while off, so nothing needs repainting. */
esp_err_t roundie_board_display_restore(roundie_board_t *target, bool *repaint)
{
    atomic_store(&target->display_on, true);
    *repaint = false;
    return ESP_OK;
}

bool roundie_board_boot_pressed(roundie_board_t *target)
{
    return atomic_load(&target->boot);
}

bool roundie_board_wake_source_active(roundie_board_t *target)
{
    return atomic_load(&target->boot) || atomic_load(&target->touched);
}
