#include "ui_private.h"
#include <string.h>
#include "lvgl.h"

/* These records and their text never leave the GUI task. static-text LVGL API
 * retains these SYSTEM buffers, not the transient command or ELF memory. */
static struct {
    uint32_t handle;
    uint32_t generation;
    lv_obj_t *object;
    lv_obj_t *label;
    char text[REP_UI_TEXT_CAPACITY];
} objects[REP_UI_WIDGET_LIMIT];
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

static void button_clicked(lv_event_t *event)
{
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (index < REP_UI_WIDGET_LIMIT) {
        roundie_ui_input(objects[index].generation, objects[index].handle, REP3_ORIGIN_TOUCH);
    }
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
        const rep_ui_widget_t *widget = &command->widget;
        if (widget->type == REP_UI_VIEW) {
            pool_before = before;
        }
        if ((widget->type == REP_UI_VIEW && root != NULL) ||
            (widget->type != REP_UI_VIEW && root == NULL) || before < 8192 || retained_bytes > 12288) {
            return false;
        }
        objects[index].handle = command->handle;
        objects[index].generation = command->generation;
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
        if (command->operation == REP_UI_TEXT) {
            if (objects[index].label == NULL) {
                return false;
            }
            memcpy(objects[index].text, command->widget.text, sizeof(objects[index].text));
            lv_label_set_text_static(objects[index].label, objects[index].text);
        } else if (command->operation == REP_UI_VISIBLE) {
            if (command->value != 0) {
                lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (command->operation == REP_UI_ENABLED) {
            if (command->value != 0) {
                lv_obj_remove_state(object, LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(object, LV_STATE_DISABLED);
            }
        } else {
            return false;
        }
    }
    uint32_t after = free_bytes();
    if (before >= after) {
        retained_bytes += before - after;
    } else if (after - before <= retained_bytes) {
        retained_bytes -= after - before;
    }
    return retained_bytes <= 16384;
}
