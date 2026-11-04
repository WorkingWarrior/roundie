#include "storage_mode_screen.h"
#include <inttypes.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"
#include "recovery_diagnostics.h"
#include "roundie_boot_guard.h"
#include "roundie_storage_session.h"
#include "storage_usb.h"

#define EXIT_BUTTON_WIDTH 200
#define EXIT_BUTTON_HEIGHT 48
#define FAILURE_EXIT_US (INT64_C(20) * 1000000)
#define REPORT_PERIOD_US INT64_C(1000000)

static const char *TAG = "storage_mode";
static lv_obj_t *screen_object;
static lv_obj_t *status_label;
static lv_obj_t *countdown_label;
static roundie_storage_session_t session;
static roundie_storage_phase_t shown_phase = (roundie_storage_phase_t)-1;
static int64_t started_us;
static int64_t next_report_us;
static bool usb_failed;
static bool exit_requested;
static bool restarting;

static void exit_clicked(lv_event_t *event)
{
    (void)event;
    exit_requested = true;
}

void roundie_storage_screen_start(int64_t now_us)
{
    started_us = now_us;
    next_report_us = now_us + REPORT_PERIOD_US;
    roundie_storage_session_init(&session, now_us);
    usb_failed = roundie_storage_usb_start() != ESP_OK;

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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 62);

    status_label = lv_label_create(screen_object);
    lv_obj_set_width(status_label, 250);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xd0d7de), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 100);

    countdown_label = lv_label_create(screen_object);
    lv_obj_set_style_text_color(countdown_label, lv_color_hex(0x9aa5b1), 0);
    lv_obj_align(countdown_label, LV_ALIGN_TOP_MID, 0, 180);

    lv_obj_t *button = lv_button_create(screen_object);
    lv_obj_set_size(button, EXIT_BUTTON_WIDTH, EXIT_BUTTON_HEIGHT);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 235);
    lv_obj_add_event_cb(button, exit_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *text = lv_label_create(button);
    lv_label_set_text(text, "Zakończ");
    lv_obj_center(text);
}

static void show_phase(roundie_storage_phase_t phase)
{
    if (phase == shown_phase) {
        return;
    }
    shown_phase = phase;
    switch (phase) {
    case ROUNDIE_STORAGE_PHASE_WAITING_FOR_HOST:
        lv_label_set_text(status_label, "Podłącz kabel USB do komputera");
        break;
    case ROUNDIE_STORAGE_PHASE_HOST_OWNS:
        lv_label_set_text(status_label, "Dysk jest podłączony.\nKopiuj pliki do folderu Inbox");
        break;
    case ROUNDIE_STORAGE_PHASE_EJECTED:
        lv_label_set_text(status_label, "Dysk wysunięty");
        break;
    }
}

void roundie_storage_screen_poll(int64_t now_us)
{
    if (screen_object == NULL || restarting) {
        return;
    }
    roundie_storage_usb_status_t usb;
    roundie_storage_usb_status(&usb);
    roundie_storage_bus_event_t event;
    while (roundie_storage_usb_next_event(&event)) {
        roundie_storage_session_bus_event(&session, event, now_us);
    }
    roundie_storage_action_t action = ROUNDIE_STORAGE_ACTION_NONE;
    uint32_t seconds_left;
    if (usb_failed) {
        static char failure[64];
        snprintf(failure, sizeof(failure), "Błąd USB: %s", usb.failed_step != NULL ? usb.failed_step : "?");
        lv_label_set_text(status_label, failure);
        shown_phase = (roundie_storage_phase_t)-1;
        int64_t left_us = started_us + FAILURE_EXIT_US - now_us;
        seconds_left = left_us > 0 ? (uint32_t)(left_us / 1000000) : 0;
        if (left_us <= 0) {
            action = ROUNDIE_STORAGE_ACTION_EXIT;
        }
    } else {
        action = roundie_storage_session_poll(&session, usb.host_owns, usb.io_calls, now_us);
        show_phase(session.phase);
        seconds_left = roundie_storage_session_seconds_left(&session, now_us);
    }
    lv_label_set_text_fmt(countdown_label, "Powrót za %" PRIu32 " s", seconds_left);
    if (action == ROUNDIE_STORAGE_ACTION_TAKE_BACK) {
        ESP_LOGW(TAG, "Host silent: taking the drive back");
        roundie_storage_usb_take_back();
    }
#if CONFIG_ROUNDIE_DEV_STORAGE_CDC
    if (now_us >= next_report_us) {
        next_report_us = now_us + REPORT_PERIOD_US;
        char line[200];
        snprintf(line, sizeof(line),
                 "STGMODE up=%" PRId64 " phase=%d owner=%s usb=%s att=%" PRIu32 " det=%" PRIu32 " susp=%" PRIu32
                 " res=%" PRIu32 " io=%" PRIu32 " drop=%" PRIu32 " left=%" PRIu32 "\r\n",
                 (now_us - started_us) / 1000000, (int)session.phase, usb.host_owns ? "USB" : "APP",
                 usb.started ? "ok" : (usb.failed_step != NULL ? usb.failed_step : "off"), usb.attached,
                 usb.detached, usb.suspended, usb.resumed, usb.io_calls, usb.dropped_events, seconds_left);
        roundie_storage_usb_report(line);
    }
    if (roundie_storage_usb_exit_requested()) {
        exit_requested = true;
    }
#endif
    if (!exit_requested && action != ROUNDIE_STORAGE_ACTION_EXIT) {
        return;
    }
    /* A restart is refused while an OTA session or a package transaction is open. */
    if (!roundie_recovery_restart_allowed()) {
        exit_requested = false;
        return;
    }
    ESP_LOGW(TAG, "Leaving USB storage mode: planned restart into normal mode");
    restarting = true;
    roundie_boot_planned_restart();
    esp_restart();
}
