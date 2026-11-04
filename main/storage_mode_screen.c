#include "storage_mode_screen.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"
#include "roundie_boot_guard.h"
#include "recovery_diagnostics.h"

/* G1 staging: this build shows the mode but has no USB yet, and a short idle
 * timeout keeps the enter/exit cycle test quick. */
#define IDLE_TIMEOUT_US (INT64_C(30) * 1000000)
#define EXIT_BUTTON_WIDTH 200
#define EXIT_BUTTON_HEIGHT 48

static const char *TAG = "storage_mode";
static lv_obj_t *screen_object;
static lv_obj_t *countdown_label;
static int64_t deadline_us;
static bool exit_requested;
static bool restarting;

static void exit_clicked(lv_event_t *event)
{
    (void)event;
    exit_requested = true;
}

void roundie_storage_screen_start(void)
{
    screen_object = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(screen_object);
    lv_obj_set_size(screen_object, 360, 360);
    lv_obj_set_style_bg_color(screen_object, lv_color_hex(0x0b1320), 0);
    lv_obj_set_style_bg_opa(screen_object, LV_OPA_COVER, 0);
    lv_obj_add_flag(screen_object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(screen_object, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen_object);
    lv_label_set_text(title, "Tryb pamięci USB");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    lv_obj_t *status = lv_label_create(screen_object);
    lv_label_set_text(status, "USB w budowie (etap 1)");
    lv_obj_set_style_text_color(status, lv_color_hex(0x9aa5b1), 0);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 110);

    countdown_label = lv_label_create(screen_object);
    lv_obj_set_style_text_color(countdown_label, lv_color_hex(0x9aa5b1), 0);
    lv_obj_align(countdown_label, LV_ALIGN_TOP_MID, 0, 150);

    lv_obj_t *button = lv_button_create(screen_object);
    lv_obj_set_size(button, EXIT_BUTTON_WIDTH, EXIT_BUTTON_HEIGHT);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 230);
    lv_obj_add_event_cb(button, exit_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *text = lv_label_create(button);
    lv_label_set_text(text, "Zakończ");
    lv_obj_center(text);
    deadline_us = 0;
}

void roundie_storage_screen_poll(int64_t now_us)
{
    if (screen_object == NULL || restarting) {
        return;
    }
    if (deadline_us == 0) {
        deadline_us = now_us + IDLE_TIMEOUT_US;
    }
    int64_t remaining_us = deadline_us - now_us;
    lv_label_set_text_fmt(countdown_label, "Powrót za %d s", remaining_us > 0 ? (int)(remaining_us / 1000000) : 0);
    if (!exit_requested && remaining_us > 0) {
        return;
    }
    /* Nothing writes Flash in this mode, but a restart is still refused while
     * an OTA session or a package transaction is open. */
    if (!roundie_recovery_restart_allowed()) {
        exit_requested = false;
        deadline_us = now_us + IDLE_TIMEOUT_US;
        return;
    }
    ESP_LOGW(TAG, "Leaving USB storage mode: planned restart into normal mode");
    restarting = true;
    roundie_boot_planned_restart();
    esp_restart();
}
