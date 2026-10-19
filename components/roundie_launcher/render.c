#include "private.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static TaskHandle_t gui_task;
static lv_obj_t *screen;
static lv_obj_t *title;
static lv_obj_t *list;
static lv_obj_t *message;
static lv_obj_t *retry;
static lv_obj_t *exit_button;
static lv_obj_t *spinner;
static lv_obj_t *result_value;
static lv_obj_t *result_detail;
static lv_obj_t *back_button;
static lv_obj_t *rows[ROUNDIE_PACKAGES_MAX];
static lv_obj_t *names[ROUNDIE_PACKAGES_MAX];
static lv_obj_t *versions[ROUNDIE_PACKAGES_MAX];
static roundie_launcher_status_t model;
static uint32_t painted_revision;
static roundie_navigation_t painted_state = ROUNDIE_NAV_WAITING;
static bool disabled;
static uint32_t create_time;
static int64_t painted_change;
static uint32_t painted_generation;
static bool creation_failed;
static uint32_t failed_revision;

static uint32_t elapsed_us(int64_t later, int64_t earlier)
{
    if (later <= earlier) {
        return 0;
    }
    uint64_t elapsed = (uint64_t)(later - earlier);
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static void reset_input(void)
{
    lv_indev_t *input = NULL;
    while ((input = lv_indev_get_next(input)) != NULL) {
        lv_indev_reset(input, NULL);
        lv_indev_wait_release(input);
    }
}

static void selected(lv_event_t *event)
{
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (index < model.count && !disabled) {
        /* The callee copies ID before this callback returns. model changes
         * only on this GUI task, and its revision accompanies the selection. */
        roundie_launcher_select(model.entries[index].id, model.revision, 1);
    }
}

static void exit_clicked(lv_event_t *event)
{
    (void)event;
    if (!disabled) {
        roundie_launcher_exit(model.generation);
    }
}

static void refresh_clicked(lv_event_t *event)
{
    (void)event;
    if (!disabled) {
        roundie_launcher_refresh();
    }
}

static void back_clicked(lv_event_t *event)
{
    (void)event;
    if (!disabled) {
        roundie_launcher_back(model.result_request);
    }
}

static lv_obj_t *label(lv_obj_t *parent, int width, int x, int y)
{
    lv_obj_t *object = lv_label_create(parent);
    if (object != NULL) {
        lv_obj_set_width(object, width);
        lv_obj_set_pos(object, x, y);
        lv_obj_set_style_text_color(object, lv_color_hex(0xffffff), 0);
        lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    }
    return object;
}

static bool create(void)
{
    int64_t started = esp_timer_get_time();
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    if (memory.free_size < 16384) {
        return false;
    }
    screen = lv_obj_create(NULL);
    if (screen == NULL) {
        return false;
    }
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101b2b), 0);
    title = label(screen, 240, 60, 30);
    message = label(screen, 250, 55, 64);
    list = lv_obj_create(screen);
    retry = lv_button_create(screen);
    spinner = lv_spinner_create(screen);
    exit_button = lv_button_create(lv_layer_top());
    result_value = label(screen, 250, 55, 150);
    result_detail = label(screen, 270, 45, 205);
    back_button = lv_button_create(screen);
    if (title == NULL || message == NULL || list == NULL || retry == NULL || exit_button == NULL || spinner == NULL ||
        result_value == NULL || result_detail == NULL || back_button == NULL) {
        return false;
    }
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(title, "Roundie / Applications");
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(list, 50, 92);
    lv_obj_set_size(list, 260, 188);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_pos(retry, 110, 290);
    lv_obj_set_size(retry, 140, 44);
    lv_obj_set_pos(exit_button, 120, 12);
    lv_obj_set_size(exit_button, 120, 44);
    lv_obj_set_pos(spinner, 164, 180);
    lv_obj_set_size(spinner, 32, 32);
    lv_obj_set_style_text_align(result_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(result_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(back_button, 100, 268);
    lv_obj_set_size(back_button, 160, 52);
    lv_obj_t *text = lv_label_create(retry);
    lv_obj_t *exit_text = lv_label_create(exit_button);
    lv_obj_t *back_text = lv_label_create(back_button);
    if (text == NULL || exit_text == NULL || back_text == NULL ||
        lv_obj_add_event_cb(retry, refresh_clicked, LV_EVENT_CLICKED, NULL) == NULL ||
        lv_obj_add_event_cb(back_button, back_clicked, LV_EVENT_CLICKED, NULL) == NULL ||
        lv_obj_add_event_cb(exit_button, exit_clicked, LV_EVENT_CLICKED, NULL) == NULL) {
        return false;
    }
    lv_label_set_text(text, "Refresh");
    lv_obj_center(text);
    lv_label_set_text(exit_text, "Exit");
    lv_obj_center(exit_text);
    lv_label_set_text(back_text, "Back");
    lv_obj_center(back_text);
    lv_obj_add_flag(back_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(result_value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(result_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(exit_button, LV_OBJ_FLAG_HIDDEN);
    for (unsigned i = 0; i < ROUNDIE_PACKAGES_MAX; i++) {
        rows[i] = lv_button_create(list);
        if (rows[i] == NULL) {
            return false;
        }
        lv_obj_set_pos(rows[i], 0, (int)i * 82);
        lv_obj_set_size(rows[i], 250, 76);
        lv_obj_set_style_pad_all(rows[i], 8, 0);
        names[i] = label(rows[i], 232, 0, 0);
        versions[i] = label(rows[i], 232, 0, 28);
        if (names[i] == NULL || versions[i] == NULL ||
            lv_obj_add_event_cb(rows[i], selected, LV_EVENT_CLICKED, (void *)(uintptr_t)i) == NULL) {
            return false;
        }
        lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    create_time = (uint32_t)(esp_timer_get_time() - started);
    return true;
}

void roundie_launcher_gui_bind(void)
{
    gui_task = xTaskGetCurrentTaskHandle();
    roundie_launcher_gui_report(true, false, 0, 0, 0, 0);
}

static uint32_t count_objects(lv_obj_t *object)
{
    if (object == NULL) {
        return 0;
    }
    uint32_t count = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        count += count_objects(lv_obj_get_child(object, (int32_t)i));
    }
    return count;
}

void roundie_launcher_gui_pump(bool safe)
{
    if (gui_task == NULL || xTaskGetCurrentTaskHandle() != gui_task) {
        return;
    }
    if (safe) {
        disabled = true;
        if (exit_button != NULL) {
            lv_obj_add_flag(exit_button, LV_OBJ_FLAG_HIDDEN);
        }
        /* Safe Mode may clean the active root; never dereference it afterward. */
        roundie_launcher_gui_report(false, false, 0, 0, 0, 0);
        return;
    }
    if (disabled) {
        return;
    }
    roundie_launcher_status(&model);
    if (model.state == ROUNDIE_NAV_WAITING) {
        return;
    }
    if (creation_failed && model.revision == failed_revision) {
        return; /* Retry only after explicit, successful refresh. */
    }
    if (screen == NULL && !create()) {
        if (exit_button != NULL) {
            lv_obj_delete(exit_button);
            exit_button = NULL;
        }
        if (screen != NULL) {
            lv_obj_delete(screen);
            screen = NULL;
        }
        creation_failed = true;
        failed_revision = model.revision;
        roundie_launcher_gui_failed();
        return; /* Boot confirmation/recovery never depends on launcher. */
    }
    creation_failed = false;
    bool idle = model.state == ROUNDIE_NAV_LIST || model.state == ROUNDIE_NAV_ERROR;
    bool running = model.state == ROUNDIE_NAV_ACTIVE || model.state == ROUNDIE_NAV_LOADING ||
        model.state == ROUNDIE_NAV_EXITING;
    uint32_t latency = 0;
    if (painted_state != model.state || painted_revision != model.revision ||
        painted_change != model.changed_us || painted_generation != model.generation) {
        int64_t apply_started = esp_timer_get_time();
        latency = model.changed_us > 0 ? elapsed_us(apply_started, model.changed_us) : 0;
        reset_input();
        painted_state = model.state;
        painted_revision = model.revision;
        painted_change = model.changed_us;
        painted_generation = model.generation;
        bool result = model.state == ROUNDIE_NAV_RESULT;
        lv_label_set_text(title, result ? model.selected_name : "Roundie / Applications");
        if (result) {
            if (model.executed) {
                lv_label_set_text_fmt(result_value, "%" PRId32, model.app_result);
            } else {
                lv_label_set_text(result_value, "No application result");
            }
            if (model.result_success) {
                lv_label_set_text(result_detail, "Native execution completed");
            } else {
                lv_label_set_text_fmt(result_detail, "Load: %" PRId32 "  Runtime: %" PRId32 "\n%s",
                    model.loader_result, model.launch_result != 0 ? model.launch_result : model.unload_result, model.error);
            }
            lv_obj_remove_flag(result_value, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(result_detail, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(back_button, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(result_value, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(result_detail, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(back_button, LV_OBJ_FLAG_HIDDEN);
        }
        for (unsigned i = 0; i < ROUNDIE_PACKAGES_MAX; i++) {
            if (i >= model.count) {
                lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            const roundie_launcher_entry_t *entry = &model.entries[i];
            lv_label_set_text(names[i], entry->name[0] != '\0' ? entry->name : entry->id);
            lv_label_set_text_fmt(versions[i], "%u.%u.%u  /  %s", entry->version[0],
                entry->version[1], entry->version[2], entry->health == ROUNDIE_PACKAGE_OK ?
                entry->id : entry->health == ROUNDIE_PACKAGE_INCOMPATIBLE ? "Incompatible" : "Unavailable");
            lv_obj_remove_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
            if (model.state == ROUNDIE_NAV_LIST && entry->health == ROUNDIE_PACKAGE_OK) {
                lv_obj_remove_state(rows[i], LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(rows[i], LV_STATE_DISABLED);
            }
        }
        const char *text = result ? (model.result_success ? "Application finished" :
                model.executed ? "Execution / cleanup failed" : "Application could not start") :
            model.state == ROUNDIE_NAV_REFRESHING ? "Reading applications..." :
            model.state == ROUNDIE_NAV_REQUESTED || model.state == ROUNDIE_NAV_LOADING ? "Loading..." :
            model.state == ROUNDIE_NAV_EXITING ? "Closing application..." :
            model.state == ROUNDIE_NAV_ACTIVE ? "Application running" :
            model.state == ROUNDIE_NAV_ERROR ? "List unavailable. Tap Refresh." :
            model.launch_result != 0 ? "Launch failed. Choose again." :
            model.unload_result != 0 ? "Application error. Choose again." :
            model.count == 0 ? "No applications installed" : "Choose an application";
        lv_label_set_text(message, text);
        if (idle) {
            lv_obj_remove_flag(list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(retry, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(retry, LV_OBJ_FLAG_HIDDEN);
            if (result) {
                lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(spinner, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (running && model.generation != 0) {
            lv_obj_remove_flag(exit_button, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lv_obj_get_child(exit_button, 0),
                model.state == ROUNDIE_NAV_EXITING ? "Closing..." : "Exit");
            if (model.state == ROUNDIE_NAV_EXITING) {
                lv_obj_add_state(exit_button, LV_STATE_DISABLED);
            } else {
                lv_obj_remove_state(exit_button, LV_STATE_DISABLED);
            }
        } else {
            lv_obj_add_flag(exit_button, LV_OBJ_FLAG_HIDDEN);
        }
        if (!running || idle) {
            lv_screen_load(screen);
        }
        if (result) {
            int64_t applied_at = esp_timer_get_time();
            char expected[24];
            snprintf(expected, sizeof(expected), "%" PRId32, model.app_result);
            roundie_launcher_result_report(model.result_request,
                lv_screen_active() == screen && strcmp(lv_label_get_text(title), model.selected_name) == 0 &&
                    strcmp(lv_label_get_text(result_value), model.executed ? expected : "No application result") == 0 &&
                    !lv_obj_has_flag(back_button, LV_OBJ_FLAG_HIDDEN),
                elapsed_us(applied_at, apply_started), model.completed_us > 0 ?
                    elapsed_us(applied_at, model.completed_us) : 0);
        }
        int64_t applied_at = esp_timer_get_time();
        roundie_launcher_painted(model.state, model.request, model.generation, applied_at,
            elapsed_us(applied_at, model.changed_us));
    }
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    roundie_launcher_gui_report(true, lv_screen_active() == screen,
        count_objects(screen) + count_objects(exit_button), (uint32_t)memory.free_size,
        create_time, latency);
}
