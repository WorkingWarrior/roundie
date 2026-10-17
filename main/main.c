#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "roundie_board.h"

#define DISPLAY_WIDTH 360
#define DISPLAY_HEIGHT 360
#define DISPLAY_STRIP_LINES 20

static const char *TAG = "roundie";

typedef struct {
    roundie_board_t *board;
    lv_display_t *display;
    lv_obj_t *touch_label;
    lv_obj_t *brightness_label;
    TaskHandle_t gui_task;
    bool transfer_pending;
    bool display_fault;
    unsigned transfer_errors;
    bool was_pressed;
    uint16_t last_x;
    uint16_t last_y;
    bool touch_error_logged;
    int64_t touch_retry_after_us;
    uint8_t brightness_percent;
} gui_state_t;

static uint32_t lv_tick_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool lcd_transfer_done(esp_lcd_panel_io_handle_t io,
                              esp_lcd_panel_io_event_data_t *event,
                              void *user_context)
{
    (void)io;
    (void)event;
    gui_state_t *state = user_context;
    BaseType_t higher_priority_woken = pdFALSE;
    vTaskNotifyGiveFromISR(state->gui_task, &higher_priority_woken);
    return higher_priority_woken == pdTRUE;
}

static void wait_for_display(lv_display_t *display)
{
    gui_state_t *state = lv_display_get_user_data(display);
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) > 0) {
        state->transfer_pending = false;
        return;
    }
    state->transfer_errors++;
    ESP_LOGE(TAG, "LCD transfer completion timeout (%u)",
             state->transfer_errors);
    /* LVGL would otherwise reuse a buffer that SPI DMA may still own. */
    esp_restart();
    abort();
}

static void flush_display(lv_display_t *display, const lv_area_t *area,
                          uint8_t *pixels)
{
    gui_state_t *state = lv_display_get_user_data(display);
    if (state->display_fault) {
        lv_display_flush_ready(display);
        return;
    }
    if (state->transfer_pending) {
        state->transfer_errors++;
        ESP_LOGE(TAG, "LCD flush requested while previous transfer is pending");
        esp_restart();
        abort();
    }
    state->transfer_pending = true;
    /* LVGL stores RGB565 in CPU byte order; ST77916 expects MSB first. */
    lv_draw_sw_rgb565_swap(pixels, lv_area_get_size(area));
    esp_err_t error = esp_lcd_panel_draw_bitmap(roundie_board_panel(state->board),
                                                area->x1, area->y1,
                                                area->x2 + 1, area->y2 + 1,
                                                pixels);
    if (error != ESP_OK) {
        state->transfer_pending = false;
        state->display_fault = true;
        state->transfer_errors++;
        ESP_LOGE(TAG, "LCD transfer enqueue failed (%u): %s",
                 state->transfer_errors, esp_err_to_name(error));
        lv_display_flush_ready(display);
    }
}

static void read_touch(lv_indev_t *device, lv_indev_data_t *data)
{
    gui_state_t *state = lv_indev_get_user_data(device);
    bool pressed = false;
    uint16_t x = state->last_x;
    uint16_t y = state->last_y;
    if (esp_timer_get_time() < state->touch_retry_after_us) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = x;
        data->point.y = y;
        return;
    }
    esp_err_t error = roundie_board_touch_read(state->board, &pressed, &x, &y);
    if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) {
        if (!state->touch_error_logged) {
            ESP_LOGE(TAG, "Touch read failed: %s", esp_err_to_name(error));
            state->touch_error_logged = true;
        }
        state->touch_retry_after_us = esp_timer_get_time() + 5000000;
    } else if (error == ESP_OK) {
        state->touch_error_logged = false;
    }
    if (pressed) {
        state->last_x = x;
        state->last_y = y;
    }
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = state->last_x;
    data->point.y = state->last_y;
    if (pressed != state->was_pressed) {
        ESP_LOGI(TAG, "Touch %s at (%u,%u)", pressed ? "press" : "release",
                 state->last_x, state->last_y);
        state->was_pressed = pressed;
        if (pressed) {
            lv_label_set_text_fmt(state->touch_label, "Touch %u, %u", x, y);
        } else {
            lv_label_set_text(state->touch_label, "Touch released");
        }
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int y,
                            lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 270);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static void close_color_field(lv_event_t *event)
{
    lv_obj_delete(lv_event_get_target_obj(event));
}

static void cycle_backlight(lv_event_t *event)
{
    gui_state_t *state = lv_event_get_user_data(event);
    uint8_t next_percent = 30;
    if (state->brightness_percent < 60) {
        next_percent = 60;
    } else if (state->brightness_percent < 100) {
        next_percent = 100;
    }
    esp_err_t error = roundie_board_backlight_set(state->board, next_percent);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Backlight change: %s", esp_err_to_name(error));
        return;
    }
    state->brightness_percent = next_percent;
    lv_label_set_text_fmt(state->brightness_label, "Backlight %u%%",
                          next_percent);
}

static void show_color_field(lv_event_t *event)
{
    const uint32_t *color = lv_event_get_user_data(event);
    lv_obj_t *screen = lv_obj_get_parent(lv_event_get_target_obj(event));
    lv_obj_t *field = lv_obj_create(screen);
    lv_obj_set_size(field, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_pos(field, 0, 0);
    lv_obj_set_style_radius(field, 0, 0);
    lv_obj_set_style_border_width(field, 0, 0);
    lv_obj_set_style_pad_all(field, 0, 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(field, lv_color_hex(*color), 0);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(field, close_color_field, LV_EVENT_CLICKED, NULL);
}

static void make_color_swatches(lv_obj_t *parent)
{
    static const uint32_t colors[] = {0xff3030, 0x30dd66, 0x368dff, 0xffffff};
    for (size_t index = 0; index < 4; index++) {
        lv_obj_t *swatch = lv_obj_create(parent);
        lv_obj_set_size(swatch, 45, 12);
        lv_obj_set_style_radius(swatch, 2, 0);
        lv_obj_set_style_border_width(swatch, 0, 0);
        lv_obj_set_style_pad_all(swatch, 0, 0);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(colors[index]), 0);
        lv_obj_align(swatch, LV_ALIGN_TOP_MID,
                     (int)index * 49 - 74, 296);
        lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(swatch, show_color_field, LV_EVENT_CLICKED,
                            (void *)&colors[index]);
    }
}

static void make_screen(gui_state_t *state,
                        const roundie_board_status_t *status,
                        uint32_t flash_bytes, size_t psram_bytes)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101b2b), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    make_label(screen, "ROUNDIE  /  0.1.0", 24, lv_color_hex(0xffffff));
    make_label(screen, "ESP32-S3  |  1.85B bring-up", 54,
               lv_color_hex(0x9eb8d0));

    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    lv_obj_t *memory = make_label(screen, "", 87, lv_color_hex(0xe1ecf6));
    lv_label_set_text_fmt(memory,
                          "Flash %" PRIu32 " MB   PSRAM %zu MB\n"
                          "SRAM %zu / %zu KiB\n"
                          "PSRAM %zu / %zu KiB",
                          flash_bytes / (1024U * 1024U),
                          psram_bytes / (1024U * 1024U),
                          internal_free / 1024U, internal_largest / 1024U,
                          psram_free / 1024U, psram_largest / 1024U);

    lv_obj_t *i2c = make_label(screen, "", 153, lv_color_hex(0xb6dce6));
    lv_label_set_text_fmt(i2c, "I2C  Touch %s  RTC %s  Gauge %s",
                          status->touch == ESP_OK ? "OK" : "ERR",
                          status->rtc == ESP_OK ? "OK" : "ERR",
                          status->gauge == ESP_OK ? "OK" : "ERR");
    lv_obj_t *reads = make_label(screen, "", 180, lv_color_hex(0xc3d2e2));
    if (status->rtc_read == ESP_OK) {
        lv_label_set_text_fmt(reads, "RTC %02u:%02u:%02u%s",
                              status->rtc_hours, status->rtc_minutes,
                              status->rtc_seconds,
                              status->rtc_voltage_low ? "  LOW V" : "");
    } else {
        lv_label_set_text(reads, "RTC read unavailable");
    }
    lv_obj_t *gauge = make_label(screen, "", 205, lv_color_hex(0xc3d2e2));
    if (status->gauge_read == ESP_OK) {
        lv_label_set_text_fmt(gauge, "Gauge %u mV  status %04x",
                              status->gauge_millivolts, status->gauge_status);
    } else {
        lv_label_set_text(gauge, "Gauge read unavailable");
    }
    state->touch_label = make_label(screen, "Touch to test", 237,
                                    lv_color_hex(0xffffff));
    lv_obj_t *brightness_button = lv_button_create(screen);
    lv_obj_set_size(brightness_button, 145, 30);
    lv_obj_align(brightness_button, LV_ALIGN_TOP_MID, 0, 258);
    lv_obj_add_event_cb(brightness_button, cycle_backlight, LV_EVENT_CLICKED,
                        state);
    state->brightness_label = lv_label_create(brightness_button);
    lv_label_set_text(state->brightness_label, "Backlight 60%");
    lv_obj_center(state->brightness_label);
    make_color_swatches(screen);

    lv_obj_t *spinner = lv_spinner_create(screen);
    lv_obj_set_size(spinner, 25, 25);
    lv_obj_align(spinner, LV_ALIGN_BOTTOM_MID, 0, -12);
}

void app_main(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware %s %s, built %s %s, IDF %s",
             description->project_name, description->version,
             description->date, description->time, description->idf_ver);
    char elf_hash[65] = {0};
    esp_app_get_elf_sha256(elf_hash, sizeof(elf_hash));
    ESP_LOGI(TAG, "Firmware ELF SHA-256 prefix: %s", elf_hash);

    uint32_t flash_bytes = 0;
    esp_err_t error = esp_flash_get_size(NULL, &flash_bytes);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Flash size query: %s", esp_err_to_name(error));
    }
    size_t psram_bytes = esp_psram_get_size();
    ESP_LOGI(TAG, "Detected flash %" PRIu32 " bytes, PSRAM %zu bytes",
             flash_bytes, psram_bytes);
    ESP_LOGI(TAG, "Internal free %zu, largest %zu; PSRAM free %zu, largest %zu",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    roundie_board_t *board = NULL;
    roundie_board_status_t status;
    error = roundie_board_start(&board, &status);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Bring-up stopped: %s", esp_err_to_name(error));
        return;
    }

    lv_init();
    lv_tick_set_cb(lv_tick_now);
    gui_state_t state = {
        .board = board,
        .gui_task = xTaskGetCurrentTaskHandle(),
        .brightness_percent = 60,
    };
    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = lcd_transfer_done,
    };
    error = esp_lcd_panel_io_register_event_callbacks(
        roundie_board_panel_io(board), &callbacks, &state);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "LCD callback registration: %s", esp_err_to_name(error));
        roundie_board_stop(board);
        return;
    }

    const size_t buffer_bytes = DISPLAY_WIDTH * DISPLAY_STRIP_LINES * 2;
    void *draw_buffer = heap_caps_malloc(buffer_bytes,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (draw_buffer == NULL) {
        ESP_LOGE(TAG, "No internal DMA memory for %zu-byte LVGL buffer",
                 buffer_bytes);
        roundie_board_stop(board);
        return;
    }

    state.display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (state.display == NULL) {
        ESP_LOGE(TAG, "LVGL display allocation failed");
        heap_caps_free(draw_buffer);
        roundie_board_stop(board);
        return;
    }
    lv_display_set_color_format(state.display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(state.display, &state);
    lv_display_set_flush_cb(state.display, flush_display);
    lv_display_set_flush_wait_cb(state.display, wait_for_display);
    lv_display_set_buffers(state.display, draw_buffer, NULL, buffer_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_indev_t *input = lv_indev_create();
    if (input == NULL) {
        ESP_LOGE(TAG, "LVGL input allocation failed");
        lv_display_delete(state.display);
        heap_caps_free(draw_buffer);
        roundie_board_stop(board);
        return;
    }
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, state.display);
    lv_indev_set_user_data(input, &state);
    lv_indev_set_read_cb(input, read_touch);
    make_screen(&state, &status, flash_bytes, psram_bytes);

    error = roundie_board_backlight_set(board, 60);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Backlight start: %s", esp_err_to_name(error));
    }
    error = esp_task_wdt_add(NULL);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "GUI watchdog registration: %s", esp_err_to_name(error));
        esp_restart();
        abort();
    }
    int64_t next_health_log_us = esp_timer_get_time() + 300000000;
    while (true) {
        lv_timer_handler();
        if (esp_timer_get_time() >= next_health_log_us) {
            ESP_LOGI(TAG,
                     "Health SRAM free/largest %zu/%zu, PSRAM free/largest %zu/%zu, LCD errors %u",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                     state.transfer_errors);
            next_health_log_us = esp_timer_get_time() + 300000000;
        }
        error = esp_task_wdt_reset();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "GUI watchdog feed: %s", esp_err_to_name(error));
            esp_restart();
            abort();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
