#include "power_manager.h"
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_POWER

/* GUI half of the power manager (docs/roundie-core.md, M2): screen policy,
 * BOOT, power menu and parking for light sleep. Portable: the board and the
 * desktop build it alike; the system worker half is per platform. */

#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "power_shared.h"
#include "recovery_diagnostics.h"
#include "roundie_app_ui.h"
#include "roundie_platform.h"
#include "roundie_power.h"
#include "roundie_watchface.h"

static const char *TAG = "power";

/* After the menu tap, the finger must be off the panel this long before the
 * GUI parks. */
#define PARK_AFTER_RELEASE_US 300000
#define MENU_ITEMS 6

roundie_power_shared_t roundie_power_shared = {
    .lock = ROUNDIE_LOCK_INITIALIZER,
};

typedef enum {
    ITEM_SLEEP,
    ITEM_SCREEN_OFF,
    ITEM_RESTART,
    ITEM_STORAGE,
    ITEM_SAFE_MODE,
    ITEM_CANCEL,
} menu_item_t;

/* GUI task only. */
static roundie_power_t policy;
static roundie_board_t *gui_board;
static const uint8_t *gui_brightness;
static lv_obj_t *menu;
static lv_obj_t *menu_title;
static uint32_t shown_message;
static int64_t last_touch_us;
static lv_obj_t *alert_box;
static uint32_t shown_alert;

/* GUI task only. After the screen was off, the board restores the panel once no
 * transfer is in flight (some controllers stay blank on display-on alone), then
 * everything is repainted if the board asks for it. */
static bool restore_pending;

static void apply_screen(bool on)
{
    esp_err_t error;
    if (on) {
        error = roundie_board_display_power(gui_board, true);
        if (error == ESP_OK) {
            error = roundie_board_backlight_set(gui_board, *gui_brightness);
        }
    } else {
        restore_pending = false; /* a later screen-off wins over a pending wake */
        error = roundie_board_backlight_set(gui_board, 0);
        if (error == ESP_OK) {
            error = roundie_board_display_power(gui_board, false);
        }
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Screen %s: %s", on ? "on" : "off", esp_err_to_name(error));
    }
    roundie_lock(&roundie_power_shared.lock);
    roundie_pm_screen(&roundie_power_shared.pm, on, roundie_time_us());
    roundie_unlock(&roundie_power_shared.lock);
    roundie_watchface_gui_visible(on);
}

static void close_menu(void)
{
    if (menu == NULL) {
        return;
    }
    /* Called from the menu's own click events: defer deletion. */
    lv_obj_delete_async(menu);
    menu = NULL;
    menu_title = NULL;
}

static void alert_clicked(lv_event_t *event)
{
    (void)event;
    if (alert_box != NULL) {
        /* Called from the box's own click event: defer deletion. */
        lv_obj_delete_async(alert_box);
        alert_box = NULL;
    }
}

/* The battery test alert (gauge_learning_service.c): a full-screen notice on
 * the top layer, above the power menu; a tap hides it. The values follow
 * roundie_gauge_alert_t. */
static void show_alert(uint8_t alert)
{
    static const char *const texts[] = {
        NULL,
        "Test baterii:\nzbliża się koniec.\nPrzygotuj kabel USB.",
        "Koniec testu baterii.\nPodłącz USB.",
        "Test baterii przerwany.\nPodłącz USB.",
    };
    if (alert_box != NULL) {
        lv_obj_delete(alert_box);
        alert_box = NULL;
    }
    if (alert == 0 || alert >= sizeof(texts) / sizeof(texts[0])) {
        return;
    }
    alert_box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(alert_box, ROUNDIE_UI_WIDTH, ROUNDIE_UI_HEIGHT);
    lv_obj_set_pos(alert_box, 0, 0);
    lv_obj_set_style_radius(alert_box, 0, 0);
    lv_obj_set_style_border_width(alert_box, 0, 0);
    lv_obj_set_style_bg_color(alert_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(alert_box, LV_OPA_COVER, 0);
    lv_obj_remove_flag(alert_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(alert_box, alert_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(alert_box);
    lv_label_set_text(label, texts[alert]);
    /* 240 px wide in the middle stays well inside the round panel. */
    lv_obj_set_width(label, 240);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(alert == 1 ? 0xffd040 : 0xff6040), 0);
    lv_obj_center(label);
}

static bool request_sleep(void)
{
    bool accepted = false;
    roundie_lock(&roundie_power_shared.lock);
    if (roundie_power_shared.phase == SLEEP_IDLE &&
        roundie_pm_request_light_sleep(&roundie_power_shared.pm, roundie_time_us()) == ROUNDIE_POWER_ALLOWED) {
        roundie_power_shared.phase = SLEEP_REQUESTED;
        roundie_power_shared.sleep_limit_s = CONFIG_ROUNDIE_POWER_SLEEP_SAFETY_SECONDS;
        accepted = true;
    }
    roundie_unlock(&roundie_power_shared.lock);
    return accepted;
}

void roundie_power_gui_screen_off(void)
{
    if (gui_board != NULL) {
        close_menu();
        roundie_power_screen_off(&policy);
    }
}

bool roundie_power_gui_sleep(void)
{
    return gui_board != NULL && request_sleep();
}

static void menu_clicked(lv_event_t *event)
{
    menu_item_t item = (menu_item_t)(intptr_t)lv_event_get_user_data(event);
    if (menu_title == NULL) {
        return;
    }
    if (item == ITEM_CANCEL) {
        close_menu();
        return;
    }
    if (item == ITEM_SCREEN_OFF) {
        close_menu();
        roundie_power_screen_off(&policy);
        return;
    }
    bool accepted = false;
    if (item == ITEM_SLEEP) {
        accepted = request_sleep();
    } else {
        roundie_lock(&roundie_power_shared.lock);
        if (roundie_power_shared.command == COMMAND_NONE && roundie_power_shared.phase == SLEEP_IDLE) {
            roundie_power_shared.command = item == ITEM_RESTART ? COMMAND_RESTART :
                item == ITEM_STORAGE ? COMMAND_STORAGE : COMMAND_SAFE_MODE;
            accepted = true;
        }
        roundie_unlock(&roundie_power_shared.lock);
    }
    lv_label_set_text(menu_title, accepted ? "Proszę czekać..." : "Zajęty");
}

static void open_menu(void)
{
    static const char *const labels[MENU_ITEMS] = {
        "Uśpij", "Wygaś ekran", "Restart", "Pamięć USB", "Tryb awaryjny", "Anuluj",
    };
    /* Full-screen backdrop on the top layer: above applications and system
     * chrome, and it absorbs touches outside the buttons. */
    menu = lv_obj_create(lv_layer_top());
    lv_obj_set_size(menu, ROUNDIE_UI_WIDTH, ROUNDIE_UI_HEIGHT);
    lv_obj_set_pos(menu, 0, 0);
    lv_obj_set_style_radius(menu, 0, 0);
    lv_obj_set_style_border_width(menu, 0, 0);
    lv_obj_set_style_pad_all(menu, 0, 0);
    lv_obj_set_style_bg_color(menu, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(menu, LV_OPA_90, 0);
    lv_obj_remove_flag(menu, LV_OBJ_FLAG_SCROLLABLE);

    menu_title = lv_label_create(menu);
    lv_label_set_text(menu_title, "Zasilanie");
    lv_obj_set_width(menu_title, 180);
    lv_obj_set_style_text_align(menu_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(menu_title, lv_color_hex(0xffffff), 0);
    lv_obj_align(menu_title, LV_ALIGN_TOP_MID, 0, 32);

    /* Six 200 x 38 targets every 41 px between y=56 and y=299 stay inside the
     * round panel: the narrowest chord there is about 260 px. */
    for (int index = 0; index < MENU_ITEMS; index++) {
        lv_obj_t *button = lv_button_create(menu);
        lv_obj_set_size(button, 200, 38);
        lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 56 + index * 41);
        lv_obj_add_event_cb(button, menu_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, labels[index]);
        lv_obj_center(label);
    }
    roundie_lock(&roundie_power_shared.lock);
    shown_message = roundie_power_shared.message_serial;
    roundie_unlock(&roundie_power_shared.lock);
}

void roundie_power_gui_bind(roundie_board_t *board, const uint8_t *brightness_percent,
    int64_t now_us)
{
    gui_board = board;
    gui_brightness = brightness_percent;
    roundie_power_init(&policy, CONFIG_ROUNDIE_POWER_SCREEN_TIMEOUT_SECONDS * 1000U, now_us);
    roundie_lock(&roundie_power_shared.lock);
    roundie_pm_init(&roundie_power_shared.pm, now_us);
    roundie_power_inhibit_init(&roundie_power_shared.inhibitors);
    roundie_power_shared.board = board;
    roundie_power_shared.state = policy.state;
    roundie_unlock(&roundie_power_shared.lock);
    ESP_LOGI(TAG, "Screen timeout %d s, sleep safety timer %d s; BOOT opens the power menu",
             CONFIG_ROUNDIE_POWER_SCREEN_TIMEOUT_SECONDS, CONFIG_ROUNDIE_POWER_SLEEP_SAFETY_SECONDS);
}

bool roundie_power_gui_touch(int64_t now_us, bool pressed)
{
    if (gui_board == NULL) {
        return true;
    }
    if (pressed) {
        last_touch_us = now_us;
    }
    return roundie_power_touch(&policy, now_us, pressed);
}

static void unbind_for_recovery(void)
{
    close_menu();
    if (policy.state != ROUNDIE_POWER_ACTIVE) {
        apply_screen(true);
    }
    roundie_lock(&roundie_power_shared.lock);
    roundie_power_shared.board = NULL;
    roundie_power_shared.state = ROUNDIE_POWER_ACTIVE;
    roundie_unlock(&roundie_power_shared.lock);
    gui_board = NULL;
    ESP_LOGI(TAG, "Safe Mode: power management released, screen on");
}

bool roundie_power_gui_poll(int64_t now_us, bool dma_idle, bool keep_display)
{
    if (gui_board == NULL) {
        return true;
    }
    roundie_lock(&roundie_power_shared.lock);
    uint32_t expired = roundie_power_inhibit_expire(&roundie_power_shared.inhibitors, now_us);
    uint32_t inhibited = roundie_power_inhibit_active(&roundie_power_shared.inhibitors);
    sleep_phase_t phase = roundie_power_shared.phase;
    bool user_wake = roundie_power_shared.user_wake;
    screen_request_t screen_request = roundie_power_shared.screen_request;
    roundie_power_shared.screen_request = SCREEN_REQUEST_NONE;
    uint32_t message_serial = roundie_power_shared.message_serial;
    uint32_t alert_serial = roundie_power_shared.battery_alert_serial;
    uint8_t alert = roundie_power_shared.battery_alert;
    char message[sizeof(roundie_power_shared.message)];
    memcpy(message, roundie_power_shared.message, sizeof(message));
    roundie_power_shared.brightness = *gui_brightness;
    roundie_unlock(&roundie_power_shared.lock);
    if (expired != 0U) {
        ESP_LOGW(TAG, "%u sleep inhibitor(s) expired (leak)", (unsigned)expired);
    }
    keep_display = keep_display || (inhibited & ROUNDIE_POWER_INHIBIT_DISPLAY_AWAKE) != 0U;

    if (phase == SLEEP_PARKED) {
        return false;
    }
    if (restore_pending && dma_idle) {
        restore_pending = false;
        bool repaint = false;
        esp_err_t restore_error = roundie_board_display_restore(gui_board, &repaint);
        ESP_LOGI(TAG, "Display re-initialised after screen off: %s", esp_err_to_name(restore_error));
        apply_screen(true);
        if (repaint || restore_error != ESP_OK) {
            lv_obj_invalidate(lv_screen_active());
        }
    }
    if (phase == SLEEP_RESUMED) {
        roundie_power_woke(&policy, now_us, user_wake);
        if (user_wake) {
            /* Light sleep may leave the panel blank (board-specific, see
             * roundie_board_display_restore()); restore it and repaint. */
            bool repaint = false;
            esp_err_t error = roundie_board_display_restore(gui_board, &repaint);
            ESP_LOGI(TAG, "Display re-initialised after wake: %s", esp_err_to_name(error));
            apply_screen(true);
            if (repaint || error != ESP_OK) {
                lv_obj_invalidate(lv_screen_active());
            }
        }
        roundie_lock(&roundie_power_shared.lock);
        roundie_power_shared.phase = SLEEP_IDLE;
        roundie_power_shared.state = policy.state;
        /* A sleep aborted before it started was cancelled by the worker. */
        if (roundie_power_shared.pm.state == ROUNDIE_PM_RESUMING) {
            roundie_pm_screen(&roundie_power_shared.pm, policy.state == ROUNDIE_POWER_ACTIVE, now_us);
            roundie_pm_resumed(&roundie_power_shared.pm, now_us);
        }
        roundie_unlock(&roundie_power_shared.lock);
        /* M8-B: the face resumes after the display restore, from fresh RTC time. */
        roundie_watchface_gui_resume();
        return true;
    }
    if (roundie_recovery_active()) {
        unbind_for_recovery();
        return true;
    }
    if (phase == SLEEP_PARK) {
        close_menu();
        bool released = !policy.touch_held && now_us - last_touch_us >= PARK_AFTER_RELEASE_US;
        if (!dma_idle || !released) {
            return true;
        }
        /* M8-B: the face parks (no LVGL work for it) before the GUI does. */
        roundie_watchface_gui_park();
        apply_screen(false);
        roundie_power_sleep_entered(&policy);
        roundie_lock(&roundie_power_shared.lock);
        roundie_power_shared.phase = SLEEP_PARKED;
        roundie_power_shared.parked_at_us = now_us;
        roundie_power_shared.state = policy.state;
        roundie_unlock(&roundie_power_shared.lock);
        return false;
    }

    if (screen_request == SCREEN_REQUEST_OFF) {
        roundie_power_screen_off(&policy);
    } else if (screen_request == SCREEN_REQUEST_ON) {
        roundie_power_screen_on(&policy, now_us);
    }
    roundie_power_action_t action = roundie_power_tick(&policy, now_us,
        roundie_board_boot_pressed(gui_board), keep_display);
    if (action == ROUNDIE_POWER_TURN_SCREEN_OFF) {
        close_menu();
        apply_screen(false);
        roundie_lock(&roundie_power_shared.lock);
        roundie_power_shared.screen_offs++;
        roundie_unlock(&roundie_power_shared.lock);
    } else if (action == ROUNDIE_POWER_TURN_SCREEN_ON) {
        restore_pending = true;
    } else if (action == ROUNDIE_POWER_TOGGLE_MENU) {
        if (menu != NULL) {
            close_menu();
        } else {
            open_menu();
        }
    }
    if (alert_serial != shown_alert) {
        shown_alert = alert_serial;
        show_alert(alert);
    }
    if (menu_title != NULL && message_serial != shown_message) {
        shown_message = message_serial;
        lv_label_set_text(menu_title, message);
    }
    roundie_lock(&roundie_power_shared.lock);
    roundie_power_shared.state = policy.state;
    roundie_unlock(&roundie_power_shared.lock);
    return true;
}
#endif
