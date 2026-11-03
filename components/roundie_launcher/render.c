#include "private.h"
#include <stdio.h>
#include <inttypes.h>
#include <math.h>
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
static lv_obj_t *edge;      /* left-edge swipe zone on the top layer */
static lv_obj_t *bottom;    /* bottom-edge swipe zone on the top layer, home only */
static lv_obj_t *exit_pill; /* swipe feedback, then "Closing..." */
static lv_obj_t *top;       /* top-edge swipe zone on the top layer, home only */
static lv_obj_t *quick;     /* quick settings panel on the top layer */
static lv_obj_t *quick_battery;
static lv_obj_t *quick_brightness;
static const roundie_launcher_quick_t *quick_actions;
static lv_obj_t *spinner;
static lv_obj_t *result_value;
static lv_obj_t *result_detail;
static lv_obj_t *back_button;
static lv_obj_t *list_title;   /* header that scrolls with the list */
static lv_obj_t *list_message;
static lv_obj_t *rows[ROUNDIE_PACKAGES_MAX];
static lv_obj_t *names[ROUNDIE_PACKAGES_MAX];
static lv_obj_t *versions[ROUNDIE_PACKAGES_MAX];
static roundie_launcher_status_t model;
static uint32_t painted_revision;
static uint32_t painted_list_revision; /* list scrolled to the top for it */
static roundie_navigation_t painted_state = ROUNDIE_NAV_WAITING;
static bool disabled;
static uint32_t create_time;
static int64_t painted_change;
static uint32_t painted_generation;
static bool creation_failed;
static uint32_t failed_revision;
static lv_point_t swipe_start;
static bool swipe_armed;

/* System navigation (docs/system-navigation.md). A swipe to the right that
 * starts in a strip along the left edge closes an application, or goes home
 * from the list. The strip lies below the 60 px top safe area and within the
 * round panel. On the home application, a swipe up that starts in a strip
 * along the bottom edge opens the list instead, and a swipe down from a strip
 * along the top edge opens quick settings. */
#define EDGE_Y 70
#define EDGE_WIDTH 44
#define EDGE_HEIGHT 220
#define BOTTOM_X 90
#define BOTTOM_Y 296
#define BOTTOM_WIDTH 180
#define BOTTOM_HEIGHT 64
#define TOP_HEIGHT 64 /* x as the bottom strip, y=0..63 */
#define QUICK_BUTTON_WIDTH 200
#define QUICK_BUTTON_HEIGHT 48
#define SWIPE_DISTANCE 80 /* travel that accepts, at least twice the cross travel */
#define PILL_WIDTH 120
#define PILL_HEIGHT 44
#define PILL_IDLE 0x3a4452
#define PILL_ARMED 0xc0392b /* closes */
#define PILL_GO 0x2f6fd0    /* navigates */
#define SWIPE_LEFT_EDGE 0U
#define SWIPE_BOTTOM_EDGE 1U
#define SWIPE_TOP_EDGE 2U
#define SWIPE_QUICK_PANEL 3U /* up on the open panel: close it */

/* Transforming list (docs/system-navigation.md): the list scrolls over the
 * whole round screen. A row narrows to fit the circle at its outer edge and
 * fades out between FADE_START and the screen edge. */
#define LIST_FIRST_ROW 100
#define ROW_STEP 82
#define ROW_HEIGHT 76
#define ROW_WIDTH 250
#define ROW_MIN_WIDTH 150
#define ROW_MARGIN 10      /* from the circle, on each side */
#define LIST_END_SPACE 120 /* lets the last item scroll up from the bottom edge */
#define FADE_START 110     /* distance of an item's centre from the screen centre */
#define SCREEN_RADIUS 180

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

/* Travel along the swipe direction: right from the left edge, up from the bottom. */
static int32_t swipe_travel(uint32_t kind, const lv_point_t *point)
{
    if (kind == SWIPE_BOTTOM_EDGE || kind == SWIPE_QUICK_PANEL) {
        return swipe_start.y - point->y;
    }
    if (kind == SWIPE_TOP_EDGE) {
        return point->y - swipe_start.y;
    }
    return point->x - swipe_start.x;
}

static bool swipe_accepts(uint32_t kind, const lv_point_t *point)
{
    int32_t travel = swipe_travel(kind, point);
    int32_t cross = kind == SWIPE_LEFT_EDGE ? point->y - swipe_start.y : point->x - swipe_start.x;
    if (cross < 0) {
        cross = -cross;
    }
    return travel >= SWIPE_DISTANCE && travel >= 2 * cross;
}

/* Moves the pill in from the edge with the finger; coloured once a release
 * would act: red to close the application, blue to navigate. */
static void swipe_feedback(uint32_t kind, const lv_point_t *point)
{
    if (kind == SWIPE_QUICK_PANEL) {
        return; /* the panel itself is the feedback */
    }
    int32_t travel = swipe_travel(kind, point);
    if (travel < 0) {
        travel = 0;
    }
    if (travel > PILL_WIDTH + 20) {
        travel = PILL_WIDTH + 20;
    }
    if (kind == SWIPE_BOTTOM_EDGE) {
        lv_obj_set_pos(exit_pill, 180 - PILL_WIDTH / 2, 360 - travel);
    } else if (kind == SWIPE_TOP_EDGE) {
        lv_obj_set_pos(exit_pill, 180 - PILL_WIDTH / 2, travel - PILL_HEIGHT);
    } else {
        int32_t y = point->y - PILL_HEIGHT / 2;
        if (y < EDGE_Y) {
            y = EDGE_Y;
        }
        if (y > EDGE_Y + EDGE_HEIGHT - PILL_HEIGHT) {
            y = EDGE_Y + EDGE_HEIGHT - PILL_HEIGHT;
        }
        lv_obj_set_pos(exit_pill, travel - PILL_WIDTH, y);
    }
    bool closes = kind == SWIPE_LEFT_EDGE && model.state != ROUNDIE_NAV_LIST;
    uint32_t armed = closes ? PILL_ARMED : PILL_GO;
    lv_obj_set_style_bg_color(exit_pill, lv_color_hex(swipe_accepts(kind, point) ? armed : PILL_IDLE), 0);
    lv_obj_remove_flag(exit_pill, LV_OBJ_FLAG_HIDDEN);
}

static void quick_show(bool shown)
{
    if (!shown) {
        lv_obj_add_flag(quick, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    uint8_t percent = 0;
    bool charging = false;
    if (quick_actions->battery(&percent, &charging)) {
        lv_label_set_text_fmt(quick_battery, "Bateria %u%%%s", percent, charging ? " · ładuje" : "");
    } else {
        lv_label_set_text(quick_battery, "Bateria: brak odczytu");
    }
    lv_label_set_text_fmt(quick_brightness, "Jasność %u%%", quick_actions->brightness(false));
    lv_obj_remove_flag(quick, LV_OBJ_FLAG_HIDDEN);
}

static void quick_clicked(lv_event_t *event)
{
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (disabled || quick_actions == NULL) {
        return;
    }
    if (index == 0) {
        lv_label_set_text_fmt(quick_brightness, "Jasność %u%%", quick_actions->brightness(true));
        return;
    }
    quick_show(false);
    if (index == 1) {
        quick_actions->screen_off();
    } else {
        quick_actions->sleep();
    }
}

static void swipe_released(uint32_t kind)
{
    bool running = model.state == ROUNDIE_NAV_ACTIVE || model.state == ROUNDIE_NAV_LOADING;
    if (kind == SWIPE_QUICK_PANEL) {
        quick_show(false);
    } else if (kind == SWIPE_TOP_EDGE) {
        if (running && model.home_active && quick_actions != NULL) {
            quick_show(true);
        }
    } else if (kind == SWIPE_LEFT_EDGE && model.state == ROUNDIE_NAV_LIST) {
        roundie_launcher_home();
    } else if (running && (kind == SWIPE_BOTTOM_EDGE) == model.home_active) {
        roundie_launcher_exit(model.generation); /* home's exit shows the list */
    }
}

static void edge_swiped(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *input = lv_indev_active();
    uint32_t kind = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
    bool running = model.state == ROUNDIE_NAV_ACTIVE || model.state == ROUNDIE_NAV_LOADING;
    if (disabled || (!running && model.state != ROUNDIE_NAV_LIST)) {
        return;
    }
    if (code == LV_EVENT_PRESS_LOST) {
        swipe_armed = false;
        lv_obj_add_flag(exit_pill, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (input == NULL) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(input, &point);
    if (code == LV_EVENT_PRESSED) {
        swipe_start = point;
        swipe_armed = true;
        lv_label_set_text(lv_obj_get_child(exit_pill, 0), kind == SWIPE_TOP_EDGE ? LV_SYMBOL_SETTINGS " Ustawienia" :
            kind == SWIPE_BOTTOM_EDGE ? LV_SYMBOL_LIST " Apps" :
            model.state == ROUNDIE_NAV_LIST ? LV_SYMBOL_HOME " Home" : LV_SYMBOL_CLOSE " Close");
        swipe_feedback(kind, &point);
    } else if (code == LV_EVENT_PRESSING && swipe_armed) {
        swipe_feedback(kind, &point);
    } else if (code == LV_EVENT_RELEASED && swipe_armed) {
        swipe_armed = false;
        lv_obj_add_flag(exit_pill, LV_OBJ_FLAG_HIDDEN);
        if (swipe_accepts(kind, &point)) {
            swipe_released(kind);
        }
    }
}

static void refresh_clicked(lv_event_t *event)
{
    (void)event;
    if (!disabled) {
        roundie_launcher_refresh();
    }
}

/* Width that fits the circle at a distance from the screen centre. */
static int32_t list_width(int32_t outer)
{
    int32_t width = ROW_MIN_WIDTH;
    if (outer < SCREEN_RADIUS) {
        float half = sqrtf((float)(SCREEN_RADIUS * SCREEN_RADIUS - outer * outer));
        width = 2 * ((int32_t)half - ROW_MARGIN);
    }
    if (width < ROW_MIN_WIDTH) {
        width = ROW_MIN_WIDTH;
    }
    if (width > ROW_WIDTH) {
        width = ROW_WIDTH;
    }
    return width;
}

/* Shapes the visible items by their position on the screen. */
static void list_transform(void)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); i++) {
        lv_obj_t *item = lv_obj_get_child(list, (int32_t)i);
        if (lv_obj_has_flag(item, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(item, &area);
        int32_t distance = (area.y1 + area.y2) / 2 - SCREEN_RADIUS;
        if (distance < 0) {
            distance = -distance;
        }
        lv_opa_t opa = LV_OPA_COVER;
        if (distance >= SCREEN_RADIUS) {
            opa = LV_OPA_TRANSP;
        } else if (distance > FADE_START) {
            opa = (lv_opa_t)(LV_OPA_COVER * (SCREEN_RADIUS - distance) / (SCREEN_RADIUS - FADE_START));
        }
        if (lv_obj_get_style_opa(item, LV_PART_MAIN) != opa) {
            lv_obj_set_style_opa(item, opa, 0);
        }
        if (lv_obj_check_type(item, &lv_button_class) && item != retry) {
            int32_t width = list_width(distance + lv_area_get_height(&area) / 2);
            if (lv_obj_get_width(item) != width) {
                lv_obj_set_width(item, width);
                lv_obj_set_x(item, SCREEN_RADIUS - width / 2);
            }
        }
    }
}

static void list_scrolled(lv_event_t *event)
{
    (void)event;
    list_transform();
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
    spinner = lv_spinner_create(screen);
    edge = lv_obj_create(lv_layer_top());
    bottom = lv_obj_create(lv_layer_top());
    exit_pill = lv_obj_create(lv_layer_top());
    top = lv_obj_create(lv_layer_top());
    quick = lv_obj_create(lv_layer_top());
    result_value = label(screen, 250, 55, 150);
    result_detail = label(screen, 270, 45, 205);
    back_button = lv_button_create(screen);
    if (title == NULL || message == NULL || list == NULL || edge == NULL || bottom == NULL || top == NULL || quick == NULL || exit_pill == NULL || spinner == NULL ||
        result_value == NULL || result_detail == NULL || back_button == NULL) {
        return false;
    }
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(title, "Roundie / Applications");
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_size(list, 360, 360);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_bottom(list, LIST_END_SPACE, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    /* A straight scrollbar would be cut by the round panel. */
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *const strips[] = {edge, bottom, top, quick};
    for (unsigned i = 0; i < 4; i++) {
        lv_obj_remove_style_all(strips[i]);
        /* Press lock: the strip keeps the touch after the finger leaves it. */
        lv_obj_add_flag(strips[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
        /* The strip keeps the drag: no scrolling or gesture of what lies below. */
        lv_obj_remove_flag(strips[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                           LV_OBJ_FLAG_SCROLL_CHAIN_VER | LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
    lv_obj_set_pos(edge, 0, EDGE_Y);
    lv_obj_set_size(edge, EDGE_WIDTH, EDGE_HEIGHT);
    lv_obj_set_pos(bottom, BOTTOM_X, BOTTOM_Y);
    lv_obj_set_size(bottom, BOTTOM_WIDTH, BOTTOM_HEIGHT);
    lv_obj_set_pos(top, BOTTOM_X, 0);
    lv_obj_set_size(top, BOTTOM_WIDTH, TOP_HEIGHT);
    /* Quick settings: an opaque full-screen panel, closed by a swipe up that
     * starts outside its buttons. */
    lv_obj_set_size(quick, 360, 360);
    lv_obj_set_style_bg_color(quick, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(quick, LV_OPA_COVER, 0);
    quick_battery = label(quick, 240, 60, 44);
    lv_obj_t *hint = label(quick, 220, 70, 290);
    if (quick_battery == NULL || hint == NULL) {
        return false;
    }
    lv_obj_set_style_text_align(quick_battery, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9aa5b1), 0);
    lv_label_set_text(hint, "Przesuń w górę, aby zamknąć");
    static const char *const quick_labels[] = {"Jasność", "Wygaś ekran", "Uśpij"};
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_t *button = lv_button_create(quick);
        lv_obj_t *text = button != NULL ? lv_label_create(button) : NULL;
        if (text == NULL ||
            lv_obj_add_event_cb(button, quick_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i) == NULL) {
            return false;
        }
        lv_obj_set_size(button, QUICK_BUTTON_WIDTH, QUICK_BUTTON_HEIGHT);
        lv_obj_set_pos(button, 180 - QUICK_BUTTON_WIDTH / 2, 86 + (int32_t)i * 64);
        lv_label_set_text(text, quick_labels[i]);
        lv_obj_center(text);
        if (i == 0) {
            quick_brightness = text;
        }
    }
    lv_obj_remove_style_all(exit_pill);
    lv_obj_remove_flag(exit_pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(exit_pill, PILL_WIDTH, PILL_HEIGHT);
    lv_obj_set_style_radius(exit_pill, PILL_HEIGHT / 2, 0);
    lv_obj_set_style_bg_opa(exit_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(exit_pill, lv_color_hex(PILL_IDLE), 0);
    lv_obj_set_pos(spinner, 164, 180);
    lv_obj_set_size(spinner, 32, 32);
    lv_obj_set_style_text_align(result_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(result_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(back_button, 100, 268);
    lv_obj_set_size(back_button, 160, 52);
    lv_obj_t *exit_text = lv_label_create(exit_pill);
    lv_obj_t *back_text = lv_label_create(back_button);
    if (exit_text == NULL || back_text == NULL ||
        lv_obj_add_event_cb(list, list_scrolled, LV_EVENT_SCROLL, NULL) == NULL ||
        lv_obj_add_event_cb(back_button, back_clicked, LV_EVENT_CLICKED, NULL) == NULL ||
        lv_obj_add_event_cb(edge, edge_swiped, LV_EVENT_ALL, (void *)(uintptr_t)SWIPE_LEFT_EDGE) == NULL ||
        lv_obj_add_event_cb(bottom, edge_swiped, LV_EVENT_ALL, (void *)(uintptr_t)SWIPE_BOTTOM_EDGE) == NULL ||
        lv_obj_add_event_cb(top, edge_swiped, LV_EVENT_ALL, (void *)(uintptr_t)SWIPE_TOP_EDGE) == NULL ||
        lv_obj_add_event_cb(quick, edge_swiped, LV_EVENT_ALL, (void *)(uintptr_t)SWIPE_QUICK_PANEL) == NULL) {
        return false;
    }
    lv_obj_set_style_text_color(exit_text, lv_color_hex(0xffffff), 0);
    lv_label_set_text(exit_text, LV_SYMBOL_CLOSE " Close");
    lv_obj_center(exit_text);
    lv_label_set_text(back_text, "Back");
    lv_obj_center(back_text);
    lv_obj_add_flag(back_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(result_value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(result_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(edge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bottom, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(quick, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(exit_pill, LV_OBJ_FLAG_HIDDEN);
    for (unsigned i = 0; i < ROUNDIE_PACKAGES_MAX; i++) {
        rows[i] = lv_button_create(list);
        if (rows[i] == NULL) {
            return false;
        }
        lv_obj_set_pos(rows[i], SCREEN_RADIUS - ROW_WIDTH / 2, LIST_FIRST_ROW + (int32_t)i * ROW_STEP);
        lv_obj_set_size(rows[i], ROW_WIDTH, ROW_HEIGHT);
        lv_obj_set_style_pad_all(rows[i], 8, 0);
        names[i] = label(rows[i], 0, 0, 0);
        versions[i] = label(rows[i], 0, 0, 28);
        if (names[i] == NULL || versions[i] == NULL ||
            lv_obj_add_event_cb(rows[i], selected, LV_EVENT_CLICKED, (void *)(uintptr_t)i) == NULL) {
            return false;
        }
        /* The labels follow the row's width as it narrows. */
        lv_obj_set_width(names[i], lv_pct(100));
        lv_obj_set_width(versions[i], lv_pct(100));
        lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    /* Header and Refresh scroll with the rows, after them in child order. */
    list_title = label(list, 240, 60, 30);
    list_message = label(list, 250, 55, 64);
    retry = lv_button_create(list);
    lv_obj_t *text = retry != NULL ? lv_label_create(retry) : NULL;
    if (list_title == NULL || list_message == NULL || text == NULL ||
        lv_obj_add_event_cb(retry, refresh_clicked, LV_EVENT_CLICKED, NULL) == NULL) {
        return false;
    }
    lv_obj_set_style_text_align(list_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(list_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(list_title, "Roundie / Applications");
    lv_obj_set_size(retry, 140, 44);
    lv_label_set_text(text, "Refresh");
    lv_obj_center(text);
    create_time = (uint32_t)(esp_timer_get_time() - started);
    return true;
}

void roundie_launcher_gui_set_quick_settings(const roundie_launcher_quick_t *quick_settings)
{
    quick_actions = quick_settings;
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
        if (edge != NULL) {
            lv_obj_add_flag(edge, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(bottom, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(quick, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(exit_pill, LV_OBJ_FLAG_HIDDEN);
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
        if (edge != NULL) {
            lv_obj_delete(edge);
            edge = NULL;
        }
        lv_obj_t **const chrome[] = {&bottom, &top, &quick};
        for (unsigned i = 0; i < 3; i++) {
            if (*chrome[i] != NULL) {
                lv_obj_delete(*chrome[i]);
                *chrome[i] = NULL;
            }
        }
        if (exit_pill != NULL) {
            lv_obj_delete(exit_pill);
            exit_pill = NULL;
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
        lv_label_set_text(list_message, text);
        if (idle) {
            /* The list carries its own header, which scrolls with it. */
            lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(message, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(retry, SCREEN_RADIUS - 70, LIST_FIRST_ROW + (int32_t)model.count * ROW_STEP + 6);
            lv_obj_remove_flag(list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
            if (painted_list_revision != model.revision) {
                painted_list_revision = model.revision;
                lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
            }
            lv_obj_update_layout(list);
            list_transform();
        } else {
            lv_obj_remove_flag(title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(message, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
            if (result) {
                lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(spinner, LV_OBJ_FLAG_HIDDEN);
            }
        }
        swipe_armed = false;
        lv_obj_t *pill_text = lv_obj_get_child(exit_pill, 0);
        lv_obj_set_style_bg_color(exit_pill, lv_color_hex(PILL_IDLE), 0);
        lv_obj_add_flag(edge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bottom, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(exit_pill, LV_OBJ_FLAG_HIDDEN);
        bool home_running = running && model.generation != 0 && model.state != ROUNDIE_NAV_EXITING &&
            model.home_active;
        if (!home_running) {
            lv_obj_add_flag(quick, LV_OBJ_FLAG_HIDDEN);
        }
        if (home_running && quick_actions != NULL) {
            lv_obj_remove_flag(top, LV_OBJ_FLAG_HIDDEN);
        }
        if (running && model.generation != 0 && model.state != ROUNDIE_NAV_EXITING) {
            /* Home has no Close: a swipe up from the bottom opens the list. */
            lv_obj_remove_flag(model.home_active ? bottom : edge, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(pill_text, model.home_active ? LV_SYMBOL_LIST " Apps" : LV_SYMBOL_CLOSE " Close");
        } else if (running && model.generation != 0) {
            /* Accepted: the pill stays at the top centre until unloaded. */
            lv_obj_set_pos(exit_pill, 180 - PILL_WIDTH / 2, 12);
            lv_label_set_text(pill_text, model.home_active ? "Apps..." : "Closing...");
            lv_obj_remove_flag(exit_pill, LV_OBJ_FLAG_HIDDEN);
        } else if (model.state == ROUNDIE_NAV_LIST && model.home_available) {
            lv_obj_remove_flag(edge, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(pill_text, LV_SYMBOL_HOME " Home");
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
        count_objects(screen) + count_objects(edge) + count_objects(bottom) + count_objects(top) +
            count_objects(quick) + count_objects(exit_pill), (uint32_t)memory.free_size,
        create_time, latency);
}
