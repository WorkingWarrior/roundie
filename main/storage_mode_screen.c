#include "storage_mode_screen.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"
#include "roundie_app_ui.h"
#include "recovery_diagnostics.h"
#include "roundie_boot_guard.h"
#include "roundie_storage_session.h"
#include "storage_import.h"
#include "storage_usb.h"

#define EXIT_BUTTON_WIDTH 200
#define EXIT_BUTTON_HEIGHT 48
#define DIALOG_BUTTON_WIDTH 130
#define FAILURE_EXIT_US (INT64_C(20) * 1000000)
#define REPORT_PERIOD_US INT64_C(1000000)
#define TEXT_MAX 160

static const char *TAG = "storage_mode";
static lv_obj_t *screen_object;
static lv_obj_t *status_label;
static lv_obj_t *countdown_label;
static lv_obj_t *exit_button;
static lv_obj_t *dialog_title;
static lv_obj_t *dialog_name;
static lv_obj_t *dialog_version;
static lv_obj_t *install_button;
static lv_obj_t *skip_button;
static roundie_storage_session_t session;
static roundie_import_status_t import_status;
static int64_t started_us;
static int64_t next_report_us;
static bool usb_failed;
static bool exit_requested;
static bool restarting;
static uint32_t take_backs;
static uint32_t host_releases; /* host to firmware: an eject or a take-back */
static roundie_storage_phase_t previous_phase = ROUNDIE_STORAGE_PHASE_WAITING_FOR_HOST;
static uint32_t run_before_request;
static bool cancel_sent;
static bool results_reported;
static char status_text[TEXT_MAX];
static char name_text[ROUNDIE_IMPORT_NAME + 8];
static char version_text[32];
static bool dialog_shown;

static void exit_clicked(lv_event_t *event)
{
    (void)event;
    exit_requested = true;
}

static void install_clicked(lv_event_t *event)
{
    (void)event;
    roundie_storage_import_decide(true);
}

static void skip_clicked(lv_event_t *event)
{
    (void)event;
    roundie_storage_import_decide(false);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int32_t width, int32_t x, int32_t y,
                             lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, EXIT_BUTTON_HEIGHT);
    lv_obj_align(button, LV_ALIGN_TOP_MID, x, y);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *make_label(lv_obj_t *parent, uint32_t color, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, 250);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

void roundie_storage_screen_start(int64_t now_us)
{
    started_us = now_us;
    next_report_us = now_us + REPORT_PERIOD_US;
    roundie_storage_session_init(&session, now_us);
    usb_failed = roundie_storage_usb_start() != ESP_OK;

    screen_object = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(screen_object);
    lv_obj_set_size(screen_object, ROUNDIE_UI_WIDTH, ROUNDIE_UI_HEIGHT);
    lv_obj_set_style_bg_color(screen_object, lv_color_hex(0x0b1320), 0);
    lv_obj_set_style_bg_opa(screen_object, LV_OPA_COVER, 0);
    lv_obj_add_flag(screen_object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(screen_object, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen_object);
    lv_label_set_text(title, "Tryb pamięci USB");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 62);

    status_label = make_label(screen_object, 0xd0d7de, 100);
    countdown_label = lv_label_create(screen_object);
    lv_obj_set_style_text_color(countdown_label, lv_color_hex(0x9aa5b1), 0);
    lv_obj_align(countdown_label, LV_ALIGN_TOP_MID, 0, 190);
    exit_button = make_button(screen_object, "Zakończ", EXIT_BUTTON_WIDTH, 0, 235, exit_clicked);

    /* The confirmation dialog replaces the status text and the exit button. */
    dialog_title = make_label(screen_object, 0xffffff, 100);
    lv_label_set_text(dialog_title, "Zainstalować?");
    dialog_name = make_label(screen_object, 0xd0d7de, 135);
    dialog_version = make_label(screen_object, 0x9aa5b1, 175);
    install_button = make_button(screen_object, "Instaluj", DIALOG_BUTTON_WIDTH, -70, 225, install_clicked);
    skip_button = make_button(screen_object, "Pomiń", DIALOG_BUTTON_WIDTH, 70, 225, skip_clicked);
    lv_obj_set_style_bg_color(install_button, lv_color_hex(0x2f8f4f), 0);
    lv_obj_add_flag(dialog_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dialog_name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dialog_version, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(install_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(skip_button, LV_OBJ_FLAG_HIDDEN);
}

/* Redraws a label only when its text changed. */
static void set_text(lv_obj_t *label, char *cache, size_t capacity, const char *text)
{
    if (strncmp(cache, text, capacity) != 0) {
        snprintf(cache, capacity, "%s", text);
        lv_label_set_text(label, cache);
    }
}

static void show_dialog(bool shown)
{
    if (shown == dialog_shown) {
        return;
    }
    dialog_shown = shown;
    lv_obj_t *dialog_objects[] = {dialog_title, dialog_name, dialog_version, install_button, skip_button};
    lv_obj_t *plain_objects[] = {status_label, countdown_label, exit_button};
    for (unsigned index = 0; index < 5; index++) {
        if (shown) {
            lv_obj_remove_flag(dialog_objects[index], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(dialog_objects[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (unsigned index = 0; index < 3; index++) {
        if (shown) {
            lv_obj_add_flag(plain_objects[index], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(plain_objects[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void result_text(char *out, size_t capacity)
{
    unsigned installed = 0;
    unsigned skipped = 0;
    unsigned failed = 0;
    const roundie_import_file_result_t *first_failure = NULL;
    for (unsigned index = 0; index < import_status.results_count; index++) {
        const roundie_import_file_result_t *entry = &import_status.results[index];
        if (entry->result == ROUNDIE_IMPORT_RESULT_INSTALLED || entry->result == ROUNDIE_IMPORT_RESULT_UPDATED) {
            installed++;
        } else if (entry->result == ROUNDIE_IMPORT_RESULT_SKIPPED || entry->result == ROUNDIE_IMPORT_RESULT_TIMEOUT) {
            skipped++;
        } else {
            failed++;
            if (first_failure == NULL) {
                first_failure = entry;
            }
        }
    }
    if (import_status.error[0] != '\0') {
        snprintf(out, capacity, "Nie można odczytać folderu Inbox");
    } else if (import_status.results_count == 0) {
        snprintf(out, capacity, "Brak paczek .rpk w folderze Inbox");
    } else {
        int used = snprintf(out, capacity, "Zainstalowano: %u", installed);
        if (skipped > 0 && used > 0 && (size_t)used < capacity) {
            used += snprintf(out + used, capacity - (size_t)used, "\nPominięto: %u", skipped);
        }
        if (failed > 0 && used > 0 && (size_t)used < capacity) {
            used += snprintf(out + used, capacity - (size_t)used, "\nOdrzucono: %u", failed);
            if (first_failure != NULL && (size_t)used < capacity) {
                snprintf(out + used, capacity - (size_t)used, "\n%.20s: %.24s", first_failure->file, first_failure->reason);
            }
        }
    }
}

#if CONFIG_ROUNDIE_DEV_STORAGE_CDC
static void report_results(void)
{
    char line[200];
    snprintf(line, sizeof(line), "IMPORTRUN run=%" PRIu32 " files=%u ignored=%u over=%u cancelled=%u error=%s\r\n",
             import_status.run, import_status.total, import_status.ignored, import_status.over_limit,
             import_status.cancelled, import_status.error[0] != '\0' ? import_status.error : "-");
    roundie_storage_usb_report(line);
    for (unsigned index = 0; index < import_status.results_count; index++) {
        const roundie_import_file_result_t *entry = &import_status.results[index];
        snprintf(line, sizeof(line), "IMPORTRESULT %u file=%s result=%s name=%s version=%u.%u.%u reason=%s\r\n", index,
                 entry->file, roundie_import_result_name(entry->result), entry->name[0] != '\0' ? entry->name : "-",
                 entry->version[0], entry->version[1], entry->version[2], entry->reason[0] != '\0' ? entry->reason : "-");
        roundie_storage_usb_report(line);
    }
}
#endif

static void update_status_text(void)
{
    char text[TEXT_MAX];
    switch (session.phase) {
    case ROUNDIE_STORAGE_PHASE_WAITING_FOR_HOST:
        snprintf(text, sizeof(text), "Podłącz kabel USB do komputera");
        break;
    case ROUNDIE_STORAGE_PHASE_HOST_OWNS:
        snprintf(text, sizeof(text), "Dysk jest podłączony.\nKopiuj pliki do folderu Inbox");
        break;
    case ROUNDIE_STORAGE_PHASE_EJECTED:
        snprintf(text, sizeof(text), "Dysk wysunięty");
        break;
    case ROUNDIE_STORAGE_PHASE_IMPORTING:
        if (import_status.file[0] != '\0') {
            snprintf(text, sizeof(text), "Sprawdzam paczkę\n%.28s (%u z %u)", import_status.file,
                     import_status.index + 1, import_status.total);
        } else {
            snprintf(text, sizeof(text), "Sprawdzam folder Inbox");
        }
        break;
    case ROUNDIE_STORAGE_PHASE_RESULT:
        result_text(text, sizeof(text));
        break;
    }
    set_text(status_label, status_text, sizeof(status_text), text);
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
    uint32_t seconds_left = 0;
    if (usb_failed) {
        static char failure[64];
        snprintf(failure, sizeof(failure), "Błąd USB: %s", usb.failed_step != NULL ? usb.failed_step : "?");
        set_text(status_label, status_text, sizeof(status_text), failure);
        int64_t left_us = started_us + FAILURE_EXIT_US - now_us;
        seconds_left = left_us > 0 ? (uint32_t)(left_us / 1000000) : 0;
        if (left_us <= 0) {
            action = ROUNDIE_STORAGE_ACTION_EXIT;
        }
    } else {
        action = roundie_storage_session_poll(&session, usb.host_owns, usb.io_calls, now_us);
        roundie_storage_import_snapshot(&import_status);
        if (previous_phase == ROUNDIE_STORAGE_PHASE_HOST_OWNS && session.phase == ROUNDIE_STORAGE_PHASE_EJECTED) {
            /* The firmware owns the volume again: read what the host left in Inbox. */
            host_releases++;
            run_before_request = import_status.run;
            results_reported = false;
            cancel_sent = false;
            roundie_storage_import_request();
            roundie_storage_session_set_phase(&session, ROUNDIE_STORAGE_PHASE_IMPORTING, now_us);
        }
        if (session.phase == ROUNDIE_STORAGE_PHASE_HOST_OWNS && !cancel_sent &&
            (import_status.phase == ROUNDIE_IMPORT_WORKING ||
             import_status.phase == ROUNDIE_IMPORT_AWAITING_CONFIRMATION)) {
            /* The host took the volume back during a run: stop reading it. */
            roundie_storage_import_cancel();
            cancel_sent = true;
        }
        if (session.phase == ROUNDIE_STORAGE_PHASE_IMPORTING && import_status.run > run_before_request &&
            import_status.phase == ROUNDIE_IMPORT_DONE) {
            roundie_storage_session_set_phase(&session, ROUNDIE_STORAGE_PHASE_RESULT, now_us);
#if CONFIG_ROUNDIE_DEV_STORAGE_CDC
            if (!results_reported) {
                results_reported = true;
                report_results();
            }
#endif
        }
        previous_phase = session.phase == ROUNDIE_STORAGE_PHASE_IMPORTING || session.phase == ROUNDIE_STORAGE_PHASE_RESULT ?
            ROUNDIE_STORAGE_PHASE_EJECTED : session.phase;
        bool asking = session.phase == ROUNDIE_STORAGE_PHASE_IMPORTING &&
            import_status.phase == ROUNDIE_IMPORT_AWAITING_CONFIRMATION;
        if (asking) {
            char name[ROUNDIE_IMPORT_NAME + 8];
            snprintf(name, sizeof(name), "%.56s", import_status.name[0] != '\0' ? import_status.name : import_status.file);
            set_text(dialog_name, name_text, sizeof(name_text), name);
            char version[32];
            snprintf(version, sizeof(version), "wersja %u.%u.%u", import_status.version[0], import_status.version[1],
                     import_status.version[2]);
            set_text(dialog_version, version_text, sizeof(version_text), version);
        }
        show_dialog(asking);
        update_status_text();
        seconds_left = roundie_storage_session_seconds_left(&session, now_us);
    }
    char countdown[32];
    snprintf(countdown, sizeof(countdown), session.phase == ROUNDIE_STORAGE_PHASE_IMPORTING && !usb_failed ? "" :
             "Powrót za %" PRIu32 " s", seconds_left);
    static char countdown_cache[32];
    set_text(countdown_label, countdown_cache, sizeof(countdown_cache), countdown);
    if (action == ROUNDIE_STORAGE_ACTION_TAKE_BACK) {
        ESP_LOGW(TAG, "Host silent: taking the drive back");
        take_backs++;
        roundie_storage_usb_take_back();
    }
#if CONFIG_ROUNDIE_DEV_STORAGE_CDC
    if (now_us >= next_report_us) {
        next_report_us = now_us + REPORT_PERIOD_US;
        char line[220];
        snprintf(line, sizeof(line),
                 "STGMODE up=%" PRId64 " phase=%d owner=%s usb=%s att=%" PRIu32 " det=%" PRIu32 " susp=%" PRIu32
                 " res=%" PRIu32 " io=%" PRIu32 " drop=%" PRIu32 " tb=%" PRIu32 " rel=%" PRIu32 " imp=%d run=%" PRIu32
                 " left=%" PRIu32 "\r\n",
                 (now_us - started_us) / 1000000, (int)session.phase, usb.host_owns ? "USB" : "APP",
                 usb.started ? "ok" : (usb.failed_step != NULL ? usb.failed_step : "off"), usb.attached,
                 usb.detached, usb.suspended, usb.resumed, usb.io_calls, usb.dropped_events, take_backs, host_releases,
                 (int)import_status.phase, import_status.run, seconds_left);
        roundie_storage_usb_report(line);
    }
    if (roundie_storage_usb_exit_requested()) {
        exit_requested = true;
    }
    int dev_decision = roundie_storage_usb_take_dev_decision();
    if (dev_decision != 0 && import_status.phase == ROUNDIE_IMPORT_AWAITING_CONFIRMATION) {
        roundie_storage_import_decide(dev_decision == 1);
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
