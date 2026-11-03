#include "ui_private.h"
#include <string.h>
#include "lvgl.h"

/* These records and their text never leave the GUI task. static-text LVGL API
 * retains these SYSTEM buffers, not the transient command or ELF memory. */
static struct {
    uint32_t handle;
    uint32_t generation;
    uint32_t type;
    lv_obj_t *object;
    lv_obj_t *label;
    lv_obj_t *buttons[2];       /* DIALOG: primary, secondary */
    lv_obj_t *button_labels[2];
    char text[REP_UI_TEXT_CAPACITY]; /* DIALOG: fields split by NUL */
} objects[REP_UI_WIDGET_LIMIT];

/* System geometry for round widgets (REP section 12.3). The top 60 pixels are
 * the navigation safe area; every placed corner lies within radius 176. */
#define ARC_INSET 10
#define ARC_SIZE 340
#define ARC_WIDTH 24
#define ARC_START_ANGLE 320 /* LVGL degrees, clockwise from 3 o'clock */
#define ARC_END_ANGLE 40
#define EDGE_X 100
#define EDGE_Y 280
#define EDGE_WIDTH 160
#define EDGE_HEIGHT 56
#define DIALOG_TEXT_X 50
#define DIALOG_TEXT_Y 90
#define DIALOG_TEXT_WIDTH 260
#define DIALOG_TEXT_HEIGHT 110
#define DIALOG_BUTTON_Y 214
#define DIALOG_BUTTON_WIDTH 120
#define DIALOG_BUTTON_HEIGHT 52
#define TOUCH_ROW 44
static lv_obj_t *root;
static lv_obj_t *system_screen;
static uint32_t retained_bytes;
static uint32_t pool_before;

static uint32_t free_bytes(void)
{
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    return (uint32_t)monitor.free_size;
}

static bool account(uint32_t before);

static void button_clicked(lv_event_t *event)
{
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (index < REP_UI_WIDGET_LIMIT) {
        roundie_ui_input(objects[index].generation, objects[index].handle, REP3_ORIGIN_TOUCH);
    }
}

static void value_changed(lv_event_t *event)
{
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (index >= REP_UI_WIDGET_LIMIT || objects[index].object == NULL) {
        return;
    }
    int32_t value;
    if (objects[index].type == REP_UI_LIST) {
        value = (int32_t)lv_roller_get_selected(objects[index].object);
    } else {
        value = lv_arc_get_value(objects[index].object);
    }
    roundie_ui_value_input(objects[index].generation, objects[index].handle, value, REP3_ORIGIN_TOUCH);
}

static void choose(unsigned index, int32_t choice, uint32_t origin)
{
    if (roundie_ui_value_input(objects[index].generation, objects[index].handle, choice, origin)) {
        lv_obj_add_flag(objects[index].object, LV_OBJ_FLAG_HIDDEN);
    }
}

static void dialog_clicked(lv_event_t *event)
{
    uintptr_t data = (uintptr_t)lv_event_get_user_data(event);
    unsigned index = (unsigned)(data & 0xffU);
    int32_t choice = (int32_t)(data >> 8);
    if (index < REP_UI_WIDGET_LIMIT && objects[index].object != NULL) {
        choose(index, choice, REP3_ORIGIN_TOUCH);
    }
}

/* Splits "message\nprimary[\nsecondary]" in place and binds static labels. */
static void dialog_text(unsigned index, const char *text)
{
    memcpy(objects[index].text, text, sizeof(objects[index].text));
    const char *fields[3] = {objects[index].text, NULL, NULL};
    unsigned field = 1;
    for (unsigned i = 0; objects[index].text[i] != '\0'; i++) {
        if (objects[index].text[i] == '\n' && field < 3) {
            objects[index].text[i] = '\0';
            fields[field++] = &objects[index].text[i + 1];
        }
    }
    lv_label_set_text_static(objects[index].label, fields[0]);
    for (unsigned i = 0; i < 2; i++) {
        if (objects[index].button_labels[i] != NULL && fields[i + 1] != NULL) {
            lv_label_set_text_static(objects[index].button_labels[i], fields[i + 1]);
        }
    }
}

static lv_obj_t *add_button(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height,
                            lv_event_cb_t callback, uintptr_t data, lv_obj_t **label)
{
    lv_obj_t *button = lv_button_create(parent);
    if (button == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(button);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x176b91), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, height / 2, 0);
    *label = lv_label_create(button);
    if (*label == NULL || lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, (void *)data) == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(*label);
    lv_obj_set_width(*label, width - 8);
    lv_obj_center(*label);
    lv_label_set_long_mode(*label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(*label, LV_TEXT_ALIGN_CENTER, 0);
    return button;
}

static bool create_round(unsigned index, const rep_ui_widget_v2_t *widget)
{
    const rep_ui_widget_t *base = &widget->base;
    lv_obj_t *object = NULL;
    if (base->type == REP_UI_LIST) {
        object = lv_roller_create(root);
        objects[index].object = object;
        if (object == NULL) {
            return false;
        }
        lv_obj_set_pos(object, base->x, base->y);
        lv_obj_set_size(object, base->width, base->height);
        /* Rows at least TOUCH_ROW pixels tall: line height plus line spacing. */
        const lv_font_t *font = lv_obj_get_style_text_font(object, LV_PART_MAIN);
        int32_t spacing = TOUCH_ROW - lv_font_get_line_height(font);
        lv_obj_set_style_text_line_space(object, spacing > 0 ? spacing : 0, LV_PART_MAIN);
        memcpy(objects[index].text, base->text, sizeof(objects[index].text));
        lv_roller_set_options(object, objects[index].text, LV_ROLLER_MODE_NORMAL);
        lv_roller_set_selected(object, (uint32_t)widget->value, LV_ANIM_OFF);
        if (lv_obj_add_event_cb(object, value_changed, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)index) == NULL) {
            return false;
        }
    } else if (base->type == REP_UI_ARC_SLIDER) {
        object = lv_arc_create(root);
        objects[index].object = object;
        if (object == NULL) {
            return false;
        }
        lv_obj_set_pos(object, ARC_INSET, ARC_INSET);
        lv_obj_set_size(object, ARC_SIZE, ARC_SIZE);
        lv_arc_set_bg_angles(object, ARC_START_ANGLE, ARC_END_ANGLE);
        lv_arc_set_mode(object, LV_ARC_MODE_REVERSE); /* larger values upward */
        lv_arc_set_range(object, widget->minimum, widget->maximum);
        lv_arc_set_value(object, widget->value);
        lv_obj_set_style_arc_width(object, ARC_WIDTH, LV_PART_MAIN);
        lv_obj_set_style_arc_width(object, ARC_WIDTH, LV_PART_INDICATOR);
        if (lv_obj_add_event_cb(object, value_changed, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)index) == NULL) {
            return false;
        }
    } else if (base->type == REP_UI_EDGE_BUTTON) {
        object = add_button(root, EDGE_X, EDGE_Y, EDGE_WIDTH, EDGE_HEIGHT, button_clicked,
                            (uintptr_t)index, &objects[index].label);
        objects[index].object = object;
        if (object == NULL) {
            return false;
        }
        memcpy(objects[index].text, base->text, sizeof(objects[index].text));
        lv_label_set_text_static(objects[index].label, objects[index].text);
    } else {
        /* DIALOG: an opaque full-panel backdrop absorbs touches below it. */
        object = lv_obj_create(root);
        objects[index].object = object;
        if (object == NULL) {
            return false;
        }
        lv_obj_remove_style_all(object);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(object, 360, 360);
        lv_obj_set_style_bg_color(object, lv_color_hex(0x05080d), 0);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
        lv_obj_t *label = lv_label_create(object);
        objects[index].label = label;
        if (label == NULL) {
            return false;
        }
        lv_obj_remove_style_all(label);
        lv_obj_set_pos(label, DIALOG_TEXT_X, DIALOG_TEXT_Y);
        lv_obj_set_size(label, DIALOG_TEXT_WIDTH, DIALOG_TEXT_HEIGHT);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        unsigned buttons = roundie_ui_text_fields(base->text, base->text_length) - 1;
        for (unsigned i = 0; i < buttons; i++) {
            /* Primary on the right, or centred when alone. */
            int32_t x = buttons == 1 ? 120 : (i == 0 ? 186 : 54);
            uintptr_t data = (uintptr_t)index | ((uintptr_t)(i + 1) << 8);
            objects[index].buttons[i] = add_button(object, x, DIALOG_BUTTON_Y, DIALOG_BUTTON_WIDTH,
                                                   DIALOG_BUTTON_HEIGHT, dialog_clicked, data,
                                                   &objects[index].button_labels[i]);
            if (objects[index].buttons[i] == NULL) {
                return false;
            }
        }
        dialog_text(index, base->text);
    }
    if ((widget->options & REP_UI_OPTION_HIDDEN) != 0) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
    return true;
}

bool roundie_ui_render_inject(uint32_t generation, uint32_t handle, int32_t value)
{
    for (unsigned i = 0; i < REP_UI_WIDGET_LIMIT; i++) {
        if (objects[i].handle != handle || objects[i].generation != generation || objects[i].object == NULL) {
            continue;
        }
        if (objects[i].type == REP_UI_LIST) {
            lv_roller_set_selected(objects[i].object, (uint32_t)value, LV_ANIM_OFF);
        } else if (objects[i].type == REP_UI_ARC_SLIDER) {
            lv_arc_set_value(objects[i].object, value);
        } else if (objects[i].type == REP_UI_DIALOG) {
            choose(i, value, REP3_ORIGIN_INJECTED);
            return true;
        } else {
            return false;
        }
        return roundie_ui_value_input(generation, handle, value, REP3_ORIGIN_INJECTED);
    }
    return false;
}

static void reset_input(void)
{
    lv_indev_t *input = NULL;
    while ((input = lv_indev_get_next(input)) != NULL) {
        lv_indev_reset(input, NULL);
        lv_indev_wait_release(input);
    }
}

bool roundie_ui_render_show(bool visible)
{
    reset_input();
    if (visible) {
        if (root == NULL) {
            return false;
        }
        lv_screen_load(root);
    } else if (system_screen != NULL) {
        lv_screen_load(system_screen);
    }
    return true;
}

void roundie_ui_render_destroy(void)
{
    roundie_ui_render_show(false);
    if (root != NULL) {
        lv_obj_delete(root);
    }
    root = NULL;
    system_screen = NULL;
    memset(objects, 0, sizeof(objects));
    retained_bytes = 0;
}

uint32_t roundie_ui_render_bytes(void)
{
    return retained_bytes;
}

void roundie_ui_render_metrics(uint32_t *before, uint32_t *free, uint32_t *count)
{
    *before = pool_before;
    *free = free_bytes();
    *count = 0;
    for (unsigned i = 0; i < REP_UI_WIDGET_LIMIT; i++) {
        *count += objects[i].object != NULL;
        *count += objects[i].label != NULL && objects[i].label != objects[i].object;
        for (unsigned j = 0; j < 2; j++) {
            *count += objects[i].buttons[j] != NULL;
            *count += objects[i].button_labels[j] != NULL;
        }
    }
}

bool roundie_ui_render(const roundie_ui_command_t *command)
{
    unsigned index = REP_UI_WIDGET_LIMIT;
    for (unsigned i = 0; i < REP_UI_WIDGET_LIMIT; i++) {
        if ((command->operation == 0 && objects[i].handle == 0) ||
            (command->operation != 0 && objects[i].handle == command->handle)) {
            index = i;
            break;
        }
    }
    if (index == REP_UI_WIDGET_LIMIT) {
        return false;
    }
    uint32_t before = free_bytes();
    if (command->operation == 0) {
        const rep_ui_widget_t *widget = &command->widget.base;
        if (widget->type == REP_UI_VIEW) {
            pool_before = before;
        }
        if ((widget->type == REP_UI_VIEW && root != NULL) ||
            (widget->type != REP_UI_VIEW && root == NULL) || before < 8192 || retained_bytes > 12288) {
            return false;
        }
        objects[index].handle = command->handle;
        objects[index].generation = command->generation;
        objects[index].type = widget->type;
        if (widget->type >= REP_UI_LIST) {
            if (!create_round(index, &command->widget)) {
                return false;
            }
            return account(before);
        }
        lv_obj_t *object = widget->type == REP_UI_VIEW ? lv_obj_create(NULL) :
            widget->type == REP_UI_BUTTON ? lv_button_create(root) : lv_label_create(root);
        objects[index].object = object;
        if (object == NULL) {
            return false;
        }
        lv_obj_remove_style_all(object);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(object, widget->x, widget->y);
        lv_obj_set_size(object, widget->width, widget->height);
        if (widget->type == REP_UI_VIEW) {
            system_screen = lv_screen_active();
            root = object; /* built offscreen until lifecycle SHOW barrier */
            lv_obj_set_style_bg_color(object, lv_color_hex(0x101b2b), 0);
            lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(object, lv_color_hex(0xffffff), 0);
        } else {
            lv_obj_t *label = object;
            if (widget->type == REP_UI_BUTTON) {
                lv_obj_set_style_bg_color(object, lv_color_hex(0x176b91), 0);
                lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(object, 18, 0);
                label = lv_label_create(object);
                if (label == NULL || lv_obj_add_event_cb(object, button_clicked, LV_EVENT_CLICKED,
                                                        (void *)(uintptr_t)index) == NULL) {
                    return false;
                }
                lv_obj_remove_style_all(label);
                lv_obj_set_width(label, widget->width - 8);
                lv_obj_center(label);
            } else {
                lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
            }
            objects[index].label = label;
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            memcpy(objects[index].text, widget->text, sizeof(widget->text));
            lv_label_set_text_static(label, objects[index].text);
        }
    } else {
        if (objects[index].generation != command->generation || objects[index].object == NULL) {
            return false;
        }
        lv_obj_t *object = objects[index].object;
        uint32_t type = objects[index].type;
        if (command->operation == REP_UI_TEXT && type == REP_UI_LIST) {
            memcpy(objects[index].text, command->widget.base.text, sizeof(objects[index].text));
            lv_roller_set_options(object, objects[index].text, LV_ROLLER_MODE_NORMAL);
            lv_roller_set_selected(object, 0, LV_ANIM_OFF);
        } else if (command->operation == REP_UI_TEXT && type == REP_UI_DIALOG) {
            dialog_text(index, command->widget.base.text);
        } else if (command->operation == REP_UI_TEXT) {
            if (objects[index].label == NULL) {
                return false;
            }
            memcpy(objects[index].text, command->widget.base.text, sizeof(objects[index].text));
            lv_label_set_text_static(objects[index].label, objects[index].text);
        } else if (command->operation == REP_UI_VALUE && type == REP_UI_LIST) {
            lv_roller_set_selected(object, command->value, LV_ANIM_OFF);
        } else if (command->operation == REP_UI_VALUE && type == REP_UI_ARC_SLIDER) {
            lv_arc_set_value(object, (int32_t)command->value);
        } else if (command->operation == REP_UI_VISIBLE) {
            if (command->value != 0) {
                lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
                if (type == REP_UI_DIALOG) {
                    lv_obj_move_foreground(object); /* modal above later widgets */
                }
            } else {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (command->operation == REP_UI_ENABLED) {
            for (unsigned i = 0; i < 3; i++) {
                lv_obj_t *target = i == 0 ? object : objects[index].buttons[i - 1];
                if (target == NULL) {
                    continue;
                }
                if (command->value != 0) {
                    lv_obj_remove_state(target, LV_STATE_DISABLED);
                } else {
                    lv_obj_add_state(target, LV_STATE_DISABLED);
                }
            }
        } else {
            return false;
        }
    }
    return account(before);
}

/* Charges the pool delta of one operation to this application's UI. */
static bool account(uint32_t before)
{
    uint32_t after = free_bytes();
    if (before >= after) {
        retained_bytes += before - after;
    } else if (after - before <= retained_bytes) {
        retained_bytes -= after - before;
    }
    return retained_bytes <= 16384;
}
