/* Stopwatch: start/stop on the edge button, laps and reset on the top button.
 * Elapsed time comes from monotonic_ms, so wall-clock changes do not affect it.
 * The system dial works as a chronograph: the second hand sweeps the seconds.
 * A 100 ms timer runs only while the stopwatch runs. No LVGL access. */
#include "rep/rep_app_v3.h"
#include <stdbool.h>
REP3_APP_METADATA;

#define TICK_MS 100U
#define LAPS_SHOWN 3U
#define START_COLOR 0x1f9d55
#define STOP_COLOR 0xc0392b

static const rep_api_v3_t *api;
static uint32_t view;
static uint32_t dial;
static uint32_t time_label;
static uint32_t laps_label;
static uint32_t lap_button;
static uint32_t edge;
static uint32_t timer;
/* Elapsed milliseconds (uint32: about 49 days) = base_ms, plus the time since
 * start_ms while running. */
static bool running;
static uint32_t base_ms;
static uint64_t start_ms;
static uint32_t laps;
static uint32_t last_lap_total_ms;
static uint32_t lap_ms[LAPS_SHOWN]; /* newest first */
static uint32_t shown_seconds;
static rep_ui_update_t update = {1, sizeof(update), 0, REP_UI_TEXT, 0, ""};
static rep_ui_widget_t view_widget = {1, sizeof(rep_ui_widget_t), REP_UI_VIEW, 0, 0, 0, 360, 360, 0, ""};
static rep_ui_widget_v2_t dial_widget = {
    {2, sizeof(rep_ui_widget_v2_t), REP_UI_CLOCK_HANDS, 0, 0, 0, 0, 0, 0, ""}, 0, 0, 0, 0};
static rep_ui_widget_v2_t time_widget = {
    {2, sizeof(rep_ui_widget_v2_t), REP_UI_LABEL, 0, 40, 150, 280, 60, 7, "00:00.0"},
    0, 0, 0, REP_UI_OPTION_FONT_LARGE};
static rep_ui_widget_t laps_widget = {1, sizeof(rep_ui_widget_t), REP_UI_LABEL, 0, 70, 214, 220, 58, 0, ""};
static rep_ui_widget_t lap_widget = {
    1, sizeof(rep_ui_widget_t), REP_UI_BUTTON, 0, 110, 76, 140, 48, 11, "Okrążenie"};
static rep_ui_widget_v2_t edge_widget = {
    {2, sizeof(rep_ui_widget_v2_t), REP_UI_EDGE_BUTTON, 0, 0, 0, 0, 0, 5, "Start"}, 0, 0, 0, 0};

static uint32_t append(uint32_t length, const char *text)
{
    while (*text != '\0' && length < REP_UI_TEXT_CAPACITY - 1U) {
        update.text[length++] = *text++;
    }
    return length;
}

static uint32_t append_number(uint32_t length, uint32_t value, bool two_digits)
{
    char digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    if (two_digits && count == 1U) {
        digits[count++] = '0';
    }
    while (count != 0U && length < REP_UI_TEXT_CAPACITY - 1U) {
        update.text[length++] = digits[--count];
    }
    return length;
}

/* "MM:SS.d", or "H:MM:SS.d" from one hour on. */
static uint32_t append_time(uint32_t length, uint32_t milliseconds)
{
    uint32_t seconds = milliseconds / 1000U;
    uint32_t hours = seconds / 3600U;
    if (hours != 0U) {
        length = append_number(length, hours, false);
        length = append(length, ":");
    }
    length = append_number(length, (seconds / 60U) % 60U, true);
    length = append(length, ":");
    length = append_number(length, seconds % 60U, true);
    length = append(length, ".");
    return append_number(length, (milliseconds / 100U) % 10U, false);
}

static int32_t set(uint32_t instance, uint32_t widget, uint32_t property, uint32_t value)
{
    update.widget = widget;
    update.property = property;
    update.value = value;
    return api->ui_update(instance, &update);
}

static int32_t set_text(uint32_t instance, uint32_t widget, uint32_t length)
{
    update.text[length] = '\0';
    return set(instance, widget, REP_UI_TEXT, length);
}

static uint32_t elapsed(uint32_t instance)
{
    if (!running) {
        return base_ms;
    }
    uint64_t now = api->monotonic_ms(instance);
    uint64_t since = now > start_ms ? now - start_ms : 0U;
    return base_ms + (uint32_t)since;
}

/* Time text every call; the dial only when the whole second changes. */
static int32_t show_time(uint32_t instance, uint32_t milliseconds)
{
    if (set_text(instance, time_label, append_time(0, milliseconds)) != 0) {
        return -21;
    }
    uint32_t seconds = milliseconds / 1000U;
    if (seconds != shown_seconds) {
        shown_seconds = seconds;
        if (set(instance, dial, REP_UI_VALUE, seconds % (uint32_t)REP_UI_CLOCK_HANDS_PERIOD) != 0) {
            return -22;
        }
    }
    return 0;
}

/* Newest lap first: "3.  00:12.4". */
static int32_t show_laps(uint32_t instance)
{
    uint32_t length = 0;
    for (uint32_t i = 0; i < LAPS_SHOWN && i < laps; i++) {
        if (i != 0U) {
            length = append(length, "\n");
        }
        length = append_number(length, laps - i, false);
        length = append(length, ".  ");
        length = append_time(length, lap_ms[i]);
    }
    return set_text(instance, laps_label, length) == 0 ? 0 : -23;
}

/* Start/Stop and its color; Lap while running, Reset when stopped with time,
 * disabled at zero. */
static int32_t show_controls(uint32_t instance)
{
    uint32_t length = append(0, running ? "Stop" : "Start");
    if (set_text(instance, edge, length) != 0 ||
        set(instance, edge, REP_UI_COLOR, running ? STOP_COLOR : START_COLOR) != 0) {
        return -24;
    }
    length = append(0, running || base_ms == 0U ? "Okrążenie" : "Zeruj");
    if (set_text(instance, lap_button, length) != 0 ||
        set(instance, lap_button, REP_UI_ENABLED, running || base_ms != 0U) != 0) {
        return -25;
    }
    return 0;
}

static int32_t start_or_stop(uint32_t instance)
{
    if (running) {
        base_ms = elapsed(instance);
        running = false;
        if (api->timer_cancel(instance, timer) != 0) {
            return -26;
        }
        timer = 0;
    } else {
        start_ms = api->monotonic_ms(instance);
        running = true;
        timer = api->timer_start(instance, TICK_MS);
        if (timer == 0) {
            return -27;
        }
    }
    int32_t result = show_time(instance, base_ms); /* exact on stop; the start point on start */
    return result != 0 ? result : show_controls(instance);
}

static int32_t lap_or_reset(uint32_t instance)
{
    if (running) {
        uint32_t total = elapsed(instance);
        for (uint32_t i = LAPS_SHOWN - 1U; i != 0U; i--) {
            lap_ms[i] = lap_ms[i - 1U];
        }
        lap_ms[0] = total - last_lap_total_ms;
        last_lap_total_ms = total;
        laps++;
        return show_laps(instance);
    }
    base_ms = 0;
    laps = 0;
    last_lap_total_ms = 0;
    int32_t result = show_time(instance, 0);
    if (result == 0) {
        result = show_laps(instance);
    }
    return result != 0 ? result : show_controls(instance);
}

static int32_t initialize(const rep_context_v3_t *context)
{
    uint32_t instance = context->instance;
    running = false;
    base_ms = 0;
    laps = 0;
    last_lap_total_ms = 0;
    shown_seconds = 0;
    timer = 0;
    view = api->ui_create(instance, &view_widget);
    if (view == 0) {
        return -11;
    }
    dial_widget.base.parent = view;
    time_widget.base.parent = view;
    laps_widget.parent = view;
    lap_widget.parent = view;
    edge_widget.base.parent = view;
    dial = api->ui_create(instance, &dial_widget.base);
    time_label = api->ui_create(instance, &time_widget.base);
    laps_label = api->ui_create(instance, &laps_widget);
    lap_button = api->ui_create(instance, &lap_widget);
    edge = api->ui_create(instance, &edge_widget.base);
    if (dial == 0 || time_label == 0 || laps_label == 0 || lap_button == 0 || edge == 0) {
        return -12;
    }
    return show_controls(instance);
}

static int32_t event(const rep_context_v3_t *context, const rep_event_v3_t *event)
{
    uint32_t instance = context->instance;
    if (event->type == REP2_EVENT_TIMER && running && event->value == timer) {
        return show_time(instance, elapsed(instance));
    }
    if (event->type == REP3_EVENT_BUTTON && event->value == edge) {
        return start_or_stop(instance);
    }
    if (event->type == REP3_EVENT_BUTTON && event->value == lap_button) {
        return lap_or_reset(instance);
    }
    return 0;
}

static int32_t quiet(const rep_context_v3_t *context)
{
    (void)context;
    return 0;
}

static const rep_descriptor_v3_t descriptor = {
    3, sizeof(descriptor), initialize, quiet, event, quiet, quiet, quiet, quiet,
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
