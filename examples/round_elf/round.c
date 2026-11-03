/* Round UI example: RoundList, ArcSlider, EdgeButton and RoundDialog through
 * the ABI 3 Round UI extension, with shadow-state reads. No LVGL access. */
#include "rep/rep_app_v3.h"
#include <stdbool.h>
REP3_APP_METADATA;

#define COLORS "Red\nGreen\nBlue\nAmber"
#define DIALOG "Reset level to 40?\nReset\nCancel"
#define LEVEL_DEFAULT 40

static const rep_api_v3_t *api;
static uint32_t view;
static uint32_t color_label;
static uint32_t list;
static uint32_t slider;
static uint32_t level_label;
static uint32_t edge;
static uint32_t dialog;
static int32_t shown_level;
static const char *const color_names[] = {"Red", "Green", "Blue", "Amber"};
static rep_ui_result_v2_t shadow;
static rep_ui_update_t update = {1, sizeof(update), 0, REP_UI_TEXT, 0, ""};
static rep_ui_widget_t view_widget = {1, sizeof(rep_ui_widget_t), REP_UI_VIEW, 0, 0, 0, 360, 360, 0, ""};
static rep_ui_widget_t color_widget = {1, sizeof(rep_ui_widget_t), REP_UI_LABEL, 0, 90, 64, 180, 28, 3, "Red"};
static rep_ui_widget_t level_widget = {1, sizeof(rep_ui_widget_t), REP_UI_LABEL, 0, 80, 238, 180, 28, 9, "Level: 40"};
static rep_ui_widget_v2_t round_widgets[] = {
    {{2, sizeof(rep_ui_widget_v2_t), REP_UI_LIST, 0, 60, 100, 180, 132, sizeof(COLORS) - 1, COLORS},
     0, 0, 0, 0},
    {{2, sizeof(rep_ui_widget_v2_t), REP_UI_ARC_SLIDER, 0, 0, 0, 0, 0, 0, ""},
     LEVEL_DEFAULT, 0, 100, 0},
    {{2, sizeof(rep_ui_widget_v2_t), REP_UI_EDGE_BUTTON, 0, 0, 0, 0, 0, 5, "Reset"},
     0, 0, 0, 0},
    {{2, sizeof(rep_ui_widget_v2_t), REP_UI_DIALOG, 0, 0, 0, 0, 0, sizeof(DIALOG) - 1, DIALOG},
     0, 0, 0, REP_UI_OPTION_HIDDEN},
};
/* Requests the bridge must refuse: a list box crossing the round edge, and a
 * second system-placed slider. */
static rep_ui_widget_v2_t corner_list = {
    {2, sizeof(rep_ui_widget_v2_t), REP_UI_LIST, 0, 0, 0, 100, 100, 1, "X"}, 0, 0, 0, 0};

static int32_t set_text(uint32_t instance, uint32_t widget, const char *prefix, int32_t value, bool number)
{
    uint32_t length = 0;
    while (prefix[length] != '\0') {
        update.text[length] = prefix[length];
        length++;
    }
    if (number) {
        uint32_t remaining = value < 0 ? 0 : (uint32_t)value;
        uint32_t divisor = 1000000U;
        bool leading = true;
        do {
            uint32_t digit = remaining / divisor;
            remaining %= divisor;
            if (digit != 0 || !leading || divisor == 1) {
                update.text[length++] = (char)('0' + digit);
                leading = false;
            }
            divisor /= 10;
        } while (divisor != 0);
    }
    update.text[length] = '\0';
    update.widget = widget;
    update.property = REP_UI_TEXT;
    update.value = length;
    return api->ui_update(instance, &update);
}

static int32_t show_level(uint32_t instance, int32_t level)
{
    if (level == shown_level) {
        return 0;
    }
    shown_level = level;
    return set_text(instance, level_label, "Level: ", level, true);
}

static int32_t initialize(const rep_context_v3_t *context)
{
    uint32_t instance = context->instance;
    shown_level = LEVEL_DEFAULT;
    view = api->ui_create(instance, &view_widget);
    if (view == 0) {
        return -11;
    }
    color_widget.parent = view;
    level_widget.parent = view;
    color_label = api->ui_create(instance, &color_widget);
    level_label = api->ui_create(instance, &level_widget);
    uint32_t *handles[] = {&list, &slider, &edge, &dialog};
    for (unsigned i = 0; i < sizeof(round_widgets) / sizeof(round_widgets[0]); i++) {
        round_widgets[i].base.parent = view;
        *handles[i] = api->ui_create(instance, &round_widgets[i].base);
    }
    if (color_label == 0 || level_label == 0 || list == 0 || slider == 0 || edge == 0 || dialog == 0) {
        return -12;
    }
    corner_list.base.parent = view;
    if (api->ui_create(instance, &corner_list.base) != 0 ||
        api->ui_create(instance, &round_widgets[1].base) != 0) {
        return -13;
    }
    return api->log(instance, "round init: list, slider, edge, dialog; misfits refused", 55);
}

static int32_t start(const rep_context_v3_t *context)
{
    return api->timer_start(context->instance, 1000) == 0 ? -14 : 0;
}

static int32_t event(const rep_context_v3_t *context, const rep_event_v3_t *event)
{
    uint32_t instance = context->instance;
    if (event->type == REP3_EVENT_VALUE && event->value == list) {
        int32_t index = (int32_t)event->reserved;
        if (index < 0 || index > 3) {
            return -15;
        }
        return set_text(instance, color_label, color_names[index], 0, false);
    }
    if (event->type == REP3_EVENT_VALUE && event->value == slider) {
        return show_level(instance, (int32_t)event->reserved);
    }
    if (event->type == REP3_EVENT_BUTTON && event->value == edge) {
        update.widget = dialog;
        update.property = REP_UI_VISIBLE;
        update.value = 1;
        return api->ui_update(instance, &update);
    }
    if (event->type == REP3_EVENT_VALUE && event->value == dialog) {
        if (event->reserved != 1) {
            return api->log(instance, "round dialog: canceled", 22);
        }
        update.widget = slider;
        update.property = REP_UI_VALUE;
        update.value = (uint32_t)LEVEL_DEFAULT;
        if (api->ui_update(instance, &update) != 0) {
            return -16;
        }
        /* The request is accepted, not yet applied: shadow reports it pending. */
        shadow = (rep_ui_result_v2_t){{2, sizeof(shadow), 0, 0, 0, 0, 0, 0}, slider, 0, 0, 0};
        if (api->ui_result(instance, &shadow.base) != 0 || shadow.requested != LEVEL_DEFAULT) {
            return -17;
        }
        return show_level(instance, LEVEL_DEFAULT);
    }
    if (event->type == REP2_EVENT_TIMER) {
        /* Reconcile with the committed value, in case an event was stale. */
        shadow = (rep_ui_result_v2_t){{2, sizeof(shadow), 0, 0, 0, 0, 0, 0}, slider, 0, 0, 0};
        if (api->ui_result(instance, &shadow.base) != 0 || shadow.base.error != 0) {
            return -18;
        }
        return shadow.pending != 0 ? 0 : show_level(instance, shadow.committed);
    }
    return 0;
}

static int32_t suspend(const rep_context_v3_t *context)
{
    return api->log(context->instance, "round suspend", 13);
}

static int32_t resume(const rep_context_v3_t *context)
{
    return api->log(context->instance, "round resume", 12);
}

static int32_t stop(const rep_context_v3_t *context)
{
    return api->log(context->instance, "round stop", 10);
}

static int32_t destroy(const rep_context_v3_t *context)
{
    return api->log(context->instance, "round destroy", 13);
}

static const rep_descriptor_v3_t descriptor = {
    3, sizeof(descriptor), initialize, start, event, suspend, resume, stop, destroy,
    REP3_CAP_UI | REP3_CAP_ROUND_UI | REP2_CAP_TIMER,
};

REP2_EXPORT const rep_descriptor_v3_t *rep_app_entry_v3(const rep_api_v3_t *table)
{
    api = rep_abi_v3(REP_ABI_V3);
    if (api != table || api == NULL || api->abi_version != 3 || api->struct_size != sizeof(*api) ||
        (api->capabilities & descriptor.required_capabilities) != descriptor.required_capabilities) {
        return NULL;
    }
    return &descriptor;
}
