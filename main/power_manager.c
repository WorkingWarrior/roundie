#include "power_manager.h"
#include "roundie_storage_mode.h"
#if CONFIG_ROUNDIE_POWER

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "recovery_diagnostics.h"
#include "roundie_ota.h"
#include "roundie_power.h"
#if CONFIG_ROUNDIE_APP_RUNTIME
#include "app_runtime.h"
#include "roundie_launcher.h"
#endif
#if CONFIG_ROUNDIE_DEV_POWER
#include "driver/usb_serial_jtag.h"
#endif

static const char *TAG = "power";

/* After the menu tap, the finger must be off the panel this long before the
 * GUI parks, and touch INT must be high within the next second. */
#define PARK_AFTER_RELEASE_US 300000
#define WAKE_SOURCE_WAIT_US 1000000
/* Bound on unloading the home application before light sleep. */
#define HOME_PARK_WAIT_US 3000000
#define MENU_ITEMS 5

typedef enum {
    SLEEP_IDLE,
    SLEEP_REQUESTED, /* GUI or RW1 asked; worker has not checked admission. */
    SLEEP_PARK,      /* Admitted; GUI must turn the panel off and park. */
    SLEEP_PARKED,    /* GUI parked; worker may sleep. */
    SLEEP_RESUMED,   /* Worker finished or aborted; GUI resumes. */
} sleep_phase_t;

typedef enum {
    COMMAND_NONE,
    COMMAND_RESTART,
    COMMAND_SAFE_MODE,
} command_t;

typedef enum {
    SCREEN_REQUEST_NONE,
    SCREEN_REQUEST_OFF,
    SCREEN_REQUEST_ON,
} screen_request_t;

typedef enum {
    ITEM_SLEEP,
    ITEM_SCREEN_OFF,
    ITEM_RESTART,
    ITEM_SAFE_MODE,
    ITEM_CANCEL,
} menu_item_t;

/* The only state shared by the GUI task and the worker; copied under lock. */
static struct {
    portMUX_TYPE lock;
    roundie_board_t *board;
    sleep_phase_t phase;
    uint32_t sleep_limit_s;
    int64_t parked_at_us;
    bool user_wake;
    command_t command;
    screen_request_t screen_request;
    uint32_t message_serial;
    char message[40];
    roundie_power_state_t state;
    roundie_power_refusal_t refusal;
    esp_err_t sleep_error;
    uint32_t sleeps;
    uint32_t screen_offs;
    uint32_t wake_causes;
    int64_t last_sleep_us;
} shared = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

/* Worker only: first admission check of the pending sleep or restart request. */
static int64_t sleep_requested_us;
static int64_t command_requested_us;

/* GUI task only. */
static roundie_power_t policy;
static roundie_board_t *gui_board;
static const uint8_t *gui_brightness;
static lv_obj_t *menu;
static lv_obj_t *menu_title;
static uint32_t shown_message;
static int64_t last_touch_us;

static void report(const char *message)
{
    portENTER_CRITICAL(&shared.lock);
    snprintf(shared.message, sizeof(shared.message), "%s", message);
    shared.message_serial++;
    portEXIT_CRITICAL(&shared.lock);
}

static void apply_screen(bool on)
{
    esp_err_t error;
    if (on) {
        error = roundie_board_display_power(gui_board, true);
        if (error == ESP_OK) {
            error = roundie_board_backlight_set(gui_board, *gui_brightness);
        }
    } else {
        error = roundie_board_backlight_set(gui_board, 0);
        if (error == ESP_OK) {
            error = roundie_board_display_power(gui_board, false);
        }
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Screen %s: %s", on ? "on" : "off", esp_err_to_name(error));
    }
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

static bool request_sleep(void)
{
    bool accepted = false;
    portENTER_CRITICAL(&shared.lock);
    if (shared.phase == SLEEP_IDLE) {
        shared.phase = SLEEP_REQUESTED;
        shared.sleep_limit_s = CONFIG_ROUNDIE_POWER_SLEEP_SAFETY_SECONDS;
        accepted = true;
    }
    portEXIT_CRITICAL(&shared.lock);
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
        portENTER_CRITICAL(&shared.lock);
        if (shared.command == COMMAND_NONE && shared.phase == SLEEP_IDLE) {
            shared.command = item == ITEM_RESTART ? COMMAND_RESTART : COMMAND_SAFE_MODE;
            accepted = true;
        }
        portEXIT_CRITICAL(&shared.lock);
    }
    lv_label_set_text(menu_title, accepted ? "Proszę czekać..." : "Zajęty");
}

static void open_menu(void)
{
    static const char *const labels[MENU_ITEMS] = {
        "Uśpij", "Wygaś ekran", "Restart", "Tryb awaryjny", "Anuluj",
    };
    /* Full-screen backdrop on the top layer: above applications and system
     * chrome, and it absorbs touches outside the buttons. */
    menu = lv_obj_create(lv_layer_top());
    lv_obj_set_size(menu, 360, 360);
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

    /* 200 x 42 targets between y=62 and y=304 stay inside the round panel:
     * the narrowest chord there is about 260 px. */
    for (int index = 0; index < MENU_ITEMS; index++) {
        lv_obj_t *button = lv_button_create(menu);
        lv_obj_set_size(button, 200, 42);
        lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 62 + index * 50);
        lv_obj_add_event_cb(button, menu_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, labels[index]);
        lv_obj_center(label);
    }
    portENTER_CRITICAL(&shared.lock);
    shown_message = shared.message_serial;
    portEXIT_CRITICAL(&shared.lock);
}

void roundie_power_gui_bind(roundie_board_t *board, const uint8_t *brightness_percent,
    int64_t now_us)
{
    gui_board = board;
    gui_brightness = brightness_percent;
    roundie_power_init(&policy, CONFIG_ROUNDIE_POWER_SCREEN_TIMEOUT_SECONDS * 1000U, now_us);
    portENTER_CRITICAL(&shared.lock);
    shared.board = board;
    shared.state = policy.state;
    portEXIT_CRITICAL(&shared.lock);
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
    portENTER_CRITICAL(&shared.lock);
    shared.board = NULL;
    shared.state = ROUNDIE_POWER_ACTIVE;
    portEXIT_CRITICAL(&shared.lock);
    gui_board = NULL;
    ESP_LOGI(TAG, "Safe Mode: power management released, screen on");
}

bool roundie_power_gui_poll(int64_t now_us, bool dma_idle, bool keep_display)
{
    if (gui_board == NULL) {
        return true;
    }
    portENTER_CRITICAL(&shared.lock);
    sleep_phase_t phase = shared.phase;
    bool user_wake = shared.user_wake;
    screen_request_t screen_request = shared.screen_request;
    shared.screen_request = SCREEN_REQUEST_NONE;
    uint32_t message_serial = shared.message_serial;
    char message[sizeof(shared.message)];
    memcpy(message, shared.message, sizeof(message));
    portEXIT_CRITICAL(&shared.lock);

    if (phase == SLEEP_PARKED) {
        return false;
    }
    if (phase == SLEEP_RESUMED) {
        roundie_power_woke(&policy, now_us, user_wake);
        if (user_wake) {
            /* Hypothesis, pending the device test: light sleep leaves the
             * ST77916 blank, so initialise it again and repaint all. */
            esp_err_t error = roundie_board_display_reinit(gui_board);
            ESP_LOGI(TAG, "Display re-initialised after wake: %s", esp_err_to_name(error));
            apply_screen(true);
            lv_obj_invalidate(lv_screen_active());
        }
        portENTER_CRITICAL(&shared.lock);
        shared.phase = SLEEP_IDLE;
        shared.state = policy.state;
        portEXIT_CRITICAL(&shared.lock);
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
        apply_screen(false);
        roundie_power_sleep_entered(&policy);
        portENTER_CRITICAL(&shared.lock);
        shared.phase = SLEEP_PARKED;
        shared.parked_at_us = now_us;
        shared.state = policy.state;
        portEXIT_CRITICAL(&shared.lock);
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
        portENTER_CRITICAL(&shared.lock);
        shared.screen_offs++;
        portEXIT_CRITICAL(&shared.lock);
    } else if (action == ROUNDIE_POWER_TURN_SCREEN_ON) {
        apply_screen(true);
    } else if (action == ROUNDIE_POWER_TOGGLE_MENU) {
        if (menu != NULL) {
            close_menu();
        } else {
            open_menu();
        }
    }
    if (menu_title != NULL && message_serial != shown_message) {
        shown_message = message_serial;
        lv_label_set_text(menu_title, message);
    }
    portENTER_CRITICAL(&shared.lock);
    shared.state = policy.state;
    portEXIT_CRITICAL(&shared.lock);
    return true;
}

static bool ota_idle(void)
{
    roundie_ota_status_t ota;
    roundie_ota_status(&ota);
    return ota.initialized && ota.running.state == ESP_OTA_IMG_VALID && ota.phase == ROUNDIE_OTA_IDLE;
}

static bool runtime_busy(void)
{
#if CONFIG_ROUNDIE_APP_RUNTIME
    return roundie_app_runtime_busy();
#else
    return false;
#endif
}

static roundie_power_refusal_t sleep_refusal(roundie_board_t *board, bool check_wake_source)
{
    roundie_boot_status_t boot;
    roundie_boot_status(&boot);
    roundie_power_conditions_t conditions = {
        .safe = boot.safe,
        .stable = boot.stage == ROUNDIE_BOOT_STABLE,
        .ota_idle = ota_idle(),
        .runtime_busy = runtime_busy() || roundie_storage_mode_active(), /* USB would drop */
        .wake_source_active = check_wake_source && roundie_board_wake_source_active(board),
    };
    return roundie_power_sleep_refusal(&conditions);
}

static void restart(bool safe)
{
    if (runtime_busy()) {
        report("Exit the app first");
        return;
    }
    if (roundie_recovery_active() || !ota_idle() || !roundie_recovery_restart_allowed()) {
        report("Busy: update pending");
        return;
    }
    if (safe) {
        /* Same persistent request as RS1 REQUEST on an idle runtime. */
        roundie_boot_enter_safe(ROUNDIE_SAFE_REQUEST);
        if (!roundie_boot_persistence_sync()) {
            report("Safe Mode not saved");
            return;
        }
    }
    ESP_LOGI(TAG, "Power menu %s", safe ? "Safe Mode restart" : "restart");
    roundie_boot_planned_restart();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

/* Light sleep needs an idle runtime. The home application is unloaded for it
 * and launched again afterwards; any other application refuses sleep. */
static bool park_home(void)
{
#if CONFIG_ROUNDIE_APP_RUNTIME
    return roundie_launcher_park_home() == ESP_OK;
#else
    return false;
#endif
}

static void resume_home(void)
{
#if CONFIG_ROUNDIE_APP_RUNTIME
    roundie_launcher_resume_home();
#endif
}

static void finish_sleep(bool user_wake, roundie_power_refusal_t refusal, esp_err_t error)
{
    resume_home();
    portENTER_CRITICAL(&shared.lock);
    shared.phase = SLEEP_RESUMED;
    shared.user_wake = user_wake;
    shared.refusal = refusal;
    shared.sleep_error = error;
    portEXIT_CRITICAL(&shared.lock);
}

static void light_sleep(roundie_board_t *board, uint32_t limit_s)
{
    ESP_LOGI(TAG, "Entering light sleep; USB Serial/JTAG is unavailable until wake");
    esp_err_t error = roundie_board_wake_arm(board);
    if (error == ESP_OK && limit_s > 0) {
        error = esp_sleep_enable_timer_wakeup((uint64_t)limit_s * 1000000U);
    }
    int64_t started_us = esp_timer_get_time();
    if (error == ESP_OK) {
        error = esp_light_sleep_start();
    }
    int64_t slept_us = esp_timer_get_time() - started_us;
    uint32_t causes = esp_sleep_get_wakeup_causes();
    if (limit_s > 0) {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }
    roundie_board_wake_disarm(board);
    /* Only a pure timer wake keeps the screen off; a failure shows it. */
    bool timer_only = causes == (1U << ESP_SLEEP_WAKEUP_TIMER);
    bool user_wake = error != ESP_OK || !timer_only;
    portENTER_CRITICAL(&shared.lock);
    shared.wake_causes = causes;
    shared.last_sleep_us = slept_us;
    if (error == ESP_OK) {
        shared.sleeps++;
    }
    portEXIT_CRITICAL(&shared.lock);
    finish_sleep(user_wake, ROUNDIE_POWER_ALLOWED, error);
    ESP_LOGI(TAG, "Light sleep %" PRId64 " ms, wake causes 0x%08" PRIx32 ", result %s",
             slept_us / 1000, causes, esp_err_to_name(error));
}

void roundie_power_worker_poll(int64_t now_us)
{
    portENTER_CRITICAL(&shared.lock);
    roundie_board_t *board = shared.board;
    sleep_phase_t phase = shared.phase;
    uint32_t limit_s = shared.sleep_limit_s;
    int64_t parked_at_us = shared.parked_at_us;
    command_t command = shared.command;
    shared.command = COMMAND_NONE;
    portEXIT_CRITICAL(&shared.lock);
    if (board == NULL) {
        return;
    }
    if (command != COMMAND_NONE) {
        if (command_requested_us == 0) {
            command_requested_us = now_us;
        }
        if (runtime_busy() && now_us - command_requested_us < HOME_PARK_WAIT_US && park_home()) {
            portENTER_CRITICAL(&shared.lock);
            shared.command = command; /* again once the home application has unloaded */
            portEXIT_CRITICAL(&shared.lock);
            return;
        }
        command_requested_us = 0;
        restart(command == COMMAND_SAFE_MODE);
        resume_home(); /* reached only when the restart was refused */
    }
    if (phase == SLEEP_REQUESTED) {
        /* The finger that chose Sleep may still be on the panel. */
        roundie_power_refusal_t refusal = sleep_refusal(board, false);
        if (sleep_requested_us == 0) {
            sleep_requested_us = now_us;
        }
        if (refusal == ROUNDIE_POWER_REFUSED_APP && now_us - sleep_requested_us < HOME_PARK_WAIT_US &&
            park_home()) {
            return; /* checked again once the home application has unloaded */
        }
        sleep_requested_us = 0;
        if (refusal != ROUNDIE_POWER_ALLOWED) {
            resume_home();
        }
        portENTER_CRITICAL(&shared.lock);
        shared.refusal = refusal;
        shared.phase = refusal == ROUNDIE_POWER_ALLOWED ? SLEEP_PARK : SLEEP_IDLE;
        portEXIT_CRITICAL(&shared.lock);
        if (refusal != ROUNDIE_POWER_ALLOWED) {
            char message[40];
            snprintf(message, sizeof(message), "Sleep refused: %s", roundie_power_refusal_name(refusal));
            report(message);
            ESP_LOGW(TAG, "%s", message);
        }
        return;
    }
    if (phase != SLEEP_PARKED) {
        return;
    }
    roundie_power_refusal_t refusal = sleep_refusal(board, true);
    if (refusal == ROUNDIE_POWER_REFUSED_WAKE_SOURCE && now_us - parked_at_us < WAKE_SOURCE_WAIT_US) {
        return;
    }
    if (refusal != ROUNDIE_POWER_ALLOWED) {
        ESP_LOGW(TAG, "Sleep aborted: %s", roundie_power_refusal_name(refusal));
        finish_sleep(true, refusal, ESP_ERR_INVALID_STATE);
        return;
    }
    light_sleep(board, limit_s);
}

#if CONFIG_ROUNDIE_DEV_POWER
static const char *phase_name(sleep_phase_t phase)
{
    switch (phase) {
    case SLEEP_IDLE: return "idle";
    case SLEEP_REQUESTED: return "requested";
    case SLEEP_PARK: return "park";
    case SLEEP_PARKED: return "parked";
    case SLEEP_RESUMED: return "resumed";
    }
    return "unknown";
}

void roundie_power_command(char *line, bool discard)
{
    char id[9] = "00000000";
    esp_err_t error = ESP_ERR_INVALID_ARG;
    size_t length = strlen(line);
    if (length >= 14 && memcmp(line, "!RW1 ", 5) == 0 && line[13] == ' ') {
        bool valid_id = true;
        for (size_t i = 5; i < 13; i++) {
            valid_id = valid_id && ((line[i] >= '0' && line[i] <= '9') ||
                                    (line[i] >= 'a' && line[i] <= 'f'));
        }
        if (valid_id) {
            memcpy(id, line + 5, 8);
        }
        const char *operation = line + 14;
        portENTER_CRITICAL(&shared.lock);
        bool bound = shared.board != NULL;
        bool idle = shared.phase == SLEEP_IDLE;
        portEXIT_CRITICAL(&shared.lock);
        if (!valid_id || discard || length >= 64) {
            error = ESP_ERR_INVALID_ARG;
        } else if (strcmp(operation, "STATUS") == 0) {
            error = ESP_OK;
        } else if (!bound) {
            error = ESP_ERR_INVALID_STATE;
        } else if (strcmp(operation, "SCREEN OFF") == 0 || strcmp(operation, "SCREEN ON") == 0) {
            portENTER_CRITICAL(&shared.lock);
            shared.screen_request = operation[8] == 'F' ? SCREEN_REQUEST_OFF : SCREEN_REQUEST_ON;
            portEXIT_CRITICAL(&shared.lock);
            error = ESP_OK;
        } else if (strncmp(operation, "SLEEP ", 6) == 0) {
            /* Diagnostics always carry a timer so the host gets USB back. */
            char *end = NULL;
            long seconds = strtol(operation + 6, &end, 10);
            if (end == NULL || *end != '\0' || seconds < 1 || seconds > 3600) {
                error = ESP_ERR_INVALID_ARG;
            } else if (!idle) {
                error = ESP_ERR_INVALID_STATE;
            } else {
                portENTER_CRITICAL(&shared.lock);
                shared.phase = SLEEP_REQUESTED;
                shared.sleep_limit_s = (uint32_t)seconds;
                portEXIT_CRITICAL(&shared.lock);
                error = ESP_OK;
            }
        } else {
            error = ESP_ERR_INVALID_ARG;
        }
    }
    portENTER_CRITICAL(&shared.lock);
    roundie_power_state_t state = shared.state;
    sleep_phase_t phase = shared.phase;
    roundie_power_refusal_t refusal = shared.refusal;
    esp_err_t sleep_error = shared.sleep_error;
    uint32_t sleeps = shared.sleeps;
    uint32_t screen_offs = shared.screen_offs;
    uint32_t wake_causes = shared.wake_causes;
    int64_t last_sleep_us = shared.last_sleep_us;
    bool bound = shared.board != NULL;
    portEXIT_CRITICAL(&shared.lock);
    char response[384];
    int length_out = snprintf(response, sizeof(response),
        "\n!RW1 %s %d {\"bound\":%u,\"state\":\"%s\",\"phase\":\"%s\",\"refusal\":\"%s\","
        "\"sleep_error\":%d,\"sleeps\":%" PRIu32 ",\"last_sleep_ms\":%" PRId64 ",\"wake_causes\":%" PRIu32 ","
        "\"screen_offs\":%" PRIu32 ",\"timeout_s\":%d,\"safety_s\":%d}\n",
        id, (int)error, bound, roundie_power_state_name(state), phase_name(phase),
        roundie_power_refusal_name(refusal), (int)sleep_error, sleeps, last_sleep_us / 1000,
        wake_causes, screen_offs, CONFIG_ROUNDIE_POWER_SCREEN_TIMEOUT_SECONDS,
        CONFIG_ROUNDIE_POWER_SLEEP_SAFETY_SECONDS);
    if (length_out > 0 && (size_t)length_out < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)length_out, pdMS_TO_TICKS(100));
    }
}
#endif
#endif
