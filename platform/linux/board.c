#include "board.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "roundie_platform.h"
#include "vhw.h"

struct roundie_board {
    atomic_bool display_on;
    atomic_uint backlight_percent;
    atomic_bool boot;
    atomic_bool touched;
    atomic_bool panel_lost; /* panel memory lost; the GUI thread clears it */
    atomic_uint restore_failures;
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

/* The emulated panel keeps its memory while off, so nothing needs repainting,
 * unless the lab's display fault makes re-initialisation fail: then the panel
 * memory is lost and the caller's error path must repaint. */
esp_err_t roundie_board_display_restore(roundie_board_t *target, bool *repaint)
{
    *repaint = false;
    vhw_state_t state;
    vhw_get(&state, roundie_time_us());
    if (state.display == VHW_DISPLAY_RESTORE) {
        atomic_store(&target->panel_lost, true);
        atomic_fetch_add(&target->restore_failures, 1);
        atomic_store(&target->display_on, true);
        return ESP_ERR_TIMEOUT;
    }
    atomic_store(&target->display_on, true);
    return ESP_OK;
}

bool roundie_desktop_board_take_panel_lost(roundie_board_t *target)
{
    return atomic_exchange(&target->panel_lost, false);
}

uint32_t roundie_desktop_board_backlight(roundie_board_t *target)
{
    return atomic_load(&target->backlight_percent);
}

uint32_t roundie_desktop_board_restore_failures(roundie_board_t *target)
{
    return atomic_load(&target->restore_failures);
}

/* The gauge (1.85B) or PMIC (1.75C) of the virtual hardware. */
void roundie_board_battery_read(roundie_board_t *target, roundie_battery_sample_t *sample)
{
    if (sample == NULL) {
        return;
    }
    if (target == NULL) {
        memset(sample, 0, sizeof(*sample));
        sample->state = ROUNDIE_SENSOR_NOT_FOUND;
        sample->error = ESP_ERR_NOT_FOUND;
        sample->observed_us = roundie_time_us();
        return;
    }
    vhw_battery_sample(sample, roundie_time_us());
}

bool roundie_board_boot_pressed(roundie_board_t *target)
{
    return atomic_load(&target->boot);
}

bool roundie_board_wake_source_active(roundie_board_t *target)
{
    return atomic_load(&target->boot) || atomic_load(&target->touched);
}

esp_err_t roundie_board_gauge_capacity_read(roundie_board_t *board,
                                            roundie_gauge_capacity_t *capacity)
{
    (void)board;
    if (capacity != NULL) {
        memset(capacity, 0, sizeof(*capacity));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t roundie_board_gauge_capacity_set(roundie_board_t *board, uint16_t capacity_mah,
                                           roundie_gauge_capacity_t *capacity)
{
    (void)capacity_mah;
    return roundie_board_gauge_capacity_read(board, capacity);
}
