#include "rep/rep_app_v3.h"
#include <stdbool.h>
REP3_APP_METADATA;

static const rep_api_v3_t *api;
static uint32_t view;
static uint32_t counter_label;
static uint32_t uptime_label;
static uint32_t button;
static uint32_t counter;
static uint32_t ticks;
static rep_ui_result_t completion;
static rep_ui_update_t update = {1, sizeof(update), 0, REP_UI_TEXT, 0, ""};
static rep_ui_widget_t widgets[] = {
    {1, sizeof(rep_ui_widget_t), REP_UI_VIEW, 0, 0, 0, 360, 360, 0, ""},
    {1, sizeof(rep_ui_widget_t), REP_UI_LABEL, 0, 70, 64, 220, 30, 11, "Roundie ELF"},
    {1, sizeof(rep_ui_widget_t), REP_UI_LABEL, 0, 55, 108, 250, 28, 17, "Native Xtensa app"},
    {1, sizeof(rep_ui_widget_t), REP_UI_LABEL, 0, 70, 158, 220, 28, 10, "Counter: 0"},
    {1, sizeof(rep_ui_widget_t), REP_UI_BUTTON, 0, 95, 210, 170, 52, 6, "TAP ME"},
    {1, sizeof(rep_ui_widget_t), REP_UI_LABEL, 0, 90, 280, 180, 24, 10, "Uptime: 0s"},
};

static int32_t number(uint32_t instance, uint32_t label, const char *prefix,
                      uint32_t value, bool seconds)
{
    uint32_t length = 0;
    while (prefix[length] != '\0') {
        update.text[length] = prefix[length];
        length++;
    }
    uint32_t divisor = 1000000000U;
    bool leading = true;
    do {
        uint32_t digit = value / divisor;
        value %= divisor;
        if (digit != 0 || !leading || divisor == 1) {
            update.text[length++] = (char)('0' + digit);
            leading = false;
        }
        divisor /= 10;
    } while (divisor != 0);
    if (seconds) {
        update.text[length++] = 's';
    }
    update.text[length] = '\0';
    update.widget = label;
    update.value = length;
    return api->ui_update(instance, &update);
}

static int32_t initialize(const rep_context_v3_t *context)
{
    counter = 0;
    ticks = 0;
    uint32_t instance = context->instance;
    if (context->abi_version != 3 || api->ui_create(instance, (const rep_ui_widget_t *)1) != 0 ||
        api->ui_result(instance, (rep_ui_result_t *)1) != -1) {
        return -10;
    }
    view = api->ui_create(instance, &widgets[0]);
    if (view == 0) {
        return -11;
    }
    for (unsigned i = 1; i < sizeof(widgets) / sizeof(widgets[0]); i++) {
        widgets[i].parent = view;
        uint32_t handle = api->ui_create(instance, &widgets[i]);
        if (handle == 0) {
            return -12;
        }
        if (i == 3) {
            counter_label = handle;
        } else if (i == 4) {
            button = handle;
        } else if (i == 5) {
            uptime_label = handle;
        }
    }
    update.widget = UINT32_MAX;
    if (api->ui_update(instance, &update) != -1 || api->ui_create(instance + 1, &widgets[0]) != 0) {
        return -13;
    }
    return api->log(instance, "ui init: six widgets; invalid pointer/ID/owner refused", 52);
}

static int32_t start(const rep_context_v3_t *context)
{
    return api->timer_start(context->instance, 1000) == 0 ? -14 : 0;
}

static int32_t event(const rep_context_v3_t *context, const rep_event_v3_t *event)
{
    if (event->type == REP3_EVENT_BUTTON) {
        if (event->value != button || (event->reserved != REP3_ORIGIN_TOUCH &&
                                      event->reserved != REP3_ORIGIN_INJECTED)) {
            return -15;
        }
        counter++;
        int32_t result = number(context->instance, counter_label, "Counter: ", counter, false);
        api->log(context->instance, event->reserved == REP3_ORIGIN_TOUCH ?
                 "ui counter: physical touch" : "ui counter: injected event", 26);
        return result;
    }
    if (event->type == REP2_EVENT_TIMER) {
        ticks++;
        if (api->ui_result(context->instance, &completion) != 0 || completion.error != 0) {
            return -16;
        }
        return number(context->instance, uptime_label, "Uptime: ", ticks, true);
    }
    return 0;
}

static int32_t suspend(const rep_context_v3_t *context)
{
    return api->log(context->instance, "ui suspend", 10);
}

static int32_t resume(const rep_context_v3_t *context)
{
    return api->log(context->instance, "ui resume", 9);
}

static int32_t stop(const rep_context_v3_t *context)
{
    update.widget = counter_label;
    if (api->ui_update(context->instance, &update) != -1 ||
        api->ui_destroy(context->instance, view) != -1) {
        return -17;
    }
    return api->log(context->instance, "ui stop: UI update/destroy admission closed", 42);
}

static int32_t destroy(const rep_context_v3_t *context)
{
    return api->log(context->instance, "ui destroy", 10);
}

static const rep_descriptor_v3_t descriptor = {
    3, sizeof(descriptor), initialize, start, event, suspend, resume, stop, destroy,
    REP3_CAP_UI | REP2_CAP_TIMER,
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
