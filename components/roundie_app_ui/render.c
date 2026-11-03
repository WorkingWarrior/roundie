#include "ui_private.h"
#include <string.h>
#include "lvgl.h"
#include "roundie_fonts.h"

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
    lv_obj_t *needles[3];       /* CLOCK_HANDS: hour, minute, second */
    lv_obj_t *ring;             /* CLOCK_HANDS with SECOND_RING: the arc */
    char text[REP_UI_TEXT_CAPACITY]; /* DIALOG: fields split by NUL */
} objects[REP_UI_WIDGET_LIMIT];

/* System geometry for round widgets (REP section 12.3). The top 60 pixels are
 * the navigation safe area; every placed corner lies within radius 176. */
#define ARC_INSET 10
#define ARC_SIZE 340
#define ARC_WIDTH 24
#define ARC_TOUCH_EXTENSION 10
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
/* CLOCK_HANDS: the ArcSlider's 340 px dial; needle values in tenths of a
 * degree of a 3600-step scale. */
#define HANDS_INSET 10
#define HANDS_SIZE 340
#define HANDS_STEPS 3600
#define HOUR_LENGTH 90
#define MINUTE_LENGTH 135
#define SECOND_LENGTH 150
#define SECOND_COLOR 0xff5a36
#define RING_DIM 0x262d36
#define RING_LIT 0xffffff
#define RING_WIDTH 6
/* Buttons: a dark pill; REP_UI_COLOR tints it (see style_button). */
#define BUTTON_FILL 0x2a3441
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

/* LONG_PRESSED or CLICKED, whichever the dial's option registered. */
static void dial_pressed(lv_event_t *event)
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

/* System button look: a pill with 20 px text, dimmed while pressed and when
 * disabled. REP_UI_COLOR replaces the dark fill with a tint of the color at
 * low opacity and draws the text in the color itself. */
static void style_button(lv_obj_t *button, lv_obj_t *label, int32_t height)
{
    lv_obj_set_style_radius(button, height / 2, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(BUTTON_FILL), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(button, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label, &roundie_font_20, 0);
}

static void tint_button(lv_obj_t *button, lv_obj_t *label, lv_color_t color)
{
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_30, 0);
    if (label != NULL) {
        lv_obj_set_style_text_color(label, color, 0);
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
    *label = lv_label_create(button);
    if (*label == NULL || lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, (void *)data) == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(*label);
    style_button(button, *label, height);
    lv_obj_set_width(*label, width - 8);
    lv_obj_center(*label);
    lv_label_set_long_mode(*label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(*label, LV_TEXT_ALIGN_CENTER, 0);
    return button;
}

static void hands_set(unsigned index, int32_t seconds)
{
    if (objects[index].ring != NULL) {
        /* From 12 o'clock up to the current second of the minute. */
        lv_arc_set_value(objects[index].ring, seconds % 60);
        return;
    }
    static const int32_t lengths[3] = {HOUR_LENGTH, MINUTE_LENGTH, SECOND_LENGTH};
    int32_t values[3] = {
        seconds / 12,          /* hour hand: 12 h over the scale */
        seconds % 3600,        /* minute hand: one hour */
        (seconds % 60) * 60,   /* second hand: one minute */
    };
    for (unsigned i = 0; i < 3; i++) {
        if (objects[index].needles[i] != NULL) {
            lv_scale_set_line_needle_value(objects[index].object, objects[index].needles[i],
                                           lengths[i], values[i]);
        }
    }
}

static lv_obj_t *add_needle(lv_obj_t *scale, int32_t width, uint32_t color)
{
    lv_obj_t *needle = lv_line_create(scale);
    if (needle == NULL) {
        return NULL;
    }
    lv_obj_remove_flag(needle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(needle, width, 0);
    lv_obj_set_style_line_rounded(needle, true, 0);
    lv_obj_set_style_line_color(needle, lv_color_hex(color), 0);
    return needle;
}

/* SECOND_RING: a thin arc around the face fills to the current second of the
 * minute. Unlike scale sections, an arc value change invalidates only the
 * changed part of the ring, so a stopwatch does not redraw the whole face. */
static bool create_ring(unsigned index, lv_obj_t *frame, const rep_ui_widget_v2_t *widget)
{
    lv_obj_t *arc = lv_arc_create(frame);
    objects[index].ring = arc;
    if (arc == NULL) {
        return false;
    }
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
    lv_obj_set_size(arc, HANDS_SIZE, HANDS_SIZE);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 60);
    for (unsigned i = 0; i < 2; i++) {
        lv_part_t part = i == 0 ? LV_PART_MAIN : LV_PART_INDICATOR;
        lv_obj_set_style_arc_width(arc, RING_WIDTH, part);
        lv_obj_set_style_arc_color(arc, lv_color_hex(i == 0 ? RING_DIM : RING_LIT), part);
    }
    hands_set(index, widget->value);
    return true;
}

/* Dial with 60 ticks and hands, or with SECOND_RING a ring and no hands.
 * Display only, so touches reach widgets below, unless REP_UI_OPTION_LONG_PRESS
 * or REP_UI_OPTION_TAP makes its 340 px box report long presses or taps. */
static bool create_hands(unsigned index, const rep_ui_widget_v2_t *widget)
{
    bool ring = (widget->options & REP_UI_OPTION_SECOND_RING) != 0;
    lv_obj_t *dial = ring ? lv_obj_create(root) : lv_scale_create(root);
    objects[index].object = dial;
    if (dial == NULL) {
        return false;
    }
    if (ring) {
        lv_obj_remove_style_all(dial);
    }
    lv_obj_remove_flag(dial, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if ((widget->options & (REP_UI_OPTION_LONG_PRESS | REP_UI_OPTION_TAP)) != 0) {
        lv_event_code_t code = (widget->options & REP_UI_OPTION_TAP) != 0 ? LV_EVENT_CLICKED : LV_EVENT_LONG_PRESSED;
        lv_obj_add_flag(dial, LV_OBJ_FLAG_CLICKABLE);
        if (lv_obj_add_event_cb(dial, dial_pressed, code, (void *)(uintptr_t)index) == NULL) {
            return false;
        }
    }
    lv_obj_set_pos(dial, HANDS_INSET, HANDS_INSET);
    lv_obj_set_size(dial, HANDS_SIZE, HANDS_SIZE);
    if (ring) {
        return create_ring(index, dial, widget);
    }
    lv_obj_t *scale = dial;
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(scale, false);
    lv_scale_set_total_tick_count(scale, 61);
    lv_scale_set_major_tick_every(scale, 5);
    lv_scale_set_range(scale, 0, HANDS_STEPS);
    lv_scale_set_angle_range(scale, 360);
    lv_scale_set_rotation(scale, 270);
    lv_obj_set_style_arc_width(scale, 0, LV_PART_MAIN);
    lv_obj_set_style_length(scale, 14, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 4, LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 6, LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, lv_color_hex(0xffffff), LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x7f8c9a), LV_PART_ITEMS);
    unsigned needles = (widget->options & REP_UI_OPTION_NO_SECONDS) != 0 ? 2 : 3;
    static const int32_t widths[3] = {8, 5, 2};
    for (unsigned i = 0; i < needles; i++) {
        objects[index].needles[i] = add_needle(scale, widths[i], i == 2 ? SECOND_COLOR : 0xffffff);
        if (objects[index].needles[i] == NULL) {
            return false;
        }
    }
    hands_set(index, widget->value);
    return true;
}

/* REP_UI_COLOR: the widget's one themable color. */
static void set_color(unsigned index, uint32_t value)
{
    lv_color_t color = lv_color_hex(value);
    lv_obj_t *object = objects[index].object;
    switch (objects[index].type) {
    case REP_UI_VIEW:
        lv_obj_set_style_bg_color(object, color, 0);
        break;
    case REP_UI_LABEL:
        lv_obj_set_style_text_color(object, color, 0);
        break;
    case REP_UI_BUTTON:
    case REP_UI_EDGE_BUTTON:
        tint_button(object, objects[index].label, color);
        break;
    case REP_UI_LIST:
        lv_obj_set_style_bg_color(object, color, LV_PART_SELECTED);
        break;
    case REP_UI_ARC_SLIDER:
        lv_obj_set_style_arc_color(object, color, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(object, color, LV_PART_KNOB);
        break;
    case REP_UI_DIALOG:
        for (unsigned i = 0; i < 2; i++) {
            if (objects[index].buttons[i] != NULL) {
                tint_button(objects[index].buttons[i], objects[index].button_labels[i], color);
            }
        }
        break;
    case REP_UI_CLOCK_HANDS:
        if (objects[index].ring != NULL) {
            lv_obj_set_style_arc_color(objects[index].ring, color, LV_PART_INDICATOR);
            break;
        }
        lv_obj_set_style_line_color(object, color, LV_PART_INDICATOR);
        for (unsigned i = 0; i < 2; i++) {
            lv_obj_set_style_line_color(objects[index].needles[i], color, 0);
        }
        break;
    default:
        break;
    }
}

static const lv_font_t *label_font(uint32_t options)
{
    if ((options & REP_UI_OPTION_FONT_LARGE) != 0) {
        return &roundie_font_48;
    }
    if ((options & REP_UI_OPTION_FONT_MEDIUM) != 0) {
        return &roundie_font_28;
    }
    return &roundie_font_14;
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
        /* Without advanced hit testing the arc claims its whole 340 x 340 box,
         * above earlier widgets. With it, only the ring within its angles is
         * hit; the extension makes that band 44 px wide. */
        lv_obj_add_flag(object, LV_OBJ_FLAG_ADV_HITTEST);
        lv_obj_set_ext_click_area(object, ARC_TOUCH_EXTENSION);
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
    } else if (base->type == REP_UI_CLOCK_HANDS) {
        if (!create_hands(index, widget)) {
            return false;
        }
        object = objects[index].object;
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
        } else if (objects[i].type == REP_UI_CLOCK_HANDS) {
            /* A long press or tap; the bridge refuses a dial without either option. */
            return roundie_ui_input(generation, handle, REP3_ORIGIN_INJECTED);
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
        for (unsigned j = 0; j < 3; j++) {
            *count += objects[i].needles[j] != NULL;
        }
        *count += objects[i].ring != NULL;
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
                label = lv_label_create(object);
                if (label == NULL || lv_obj_add_event_cb(object, button_clicked, LV_EVENT_CLICKED,
                                                        (void *)(uintptr_t)index) == NULL) {
                    return false;
                }
                lv_obj_remove_style_all(label);
                style_button(object, label, widget->height);
                lv_obj_set_width(label, widget->width - 8);
                lv_obj_center(label);
            } else {
                lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
            }
            objects[index].label = label;
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            if (widget->type == REP_UI_LABEL) {
                lv_obj_set_style_text_font(label, label_font(command->widget.options), 0);
                if ((command->widget.options & REP_UI_OPTION_HIDDEN) != 0) {
                    lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
                }
            }
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
        } else if (command->operation == REP_UI_VALUE && type == REP_UI_CLOCK_HANDS) {
            hands_set(index, (int32_t)command->value);
        } else if (command->operation == REP_UI_COLOR) {
            set_color(index, command->value);
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
