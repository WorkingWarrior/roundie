/* Stopwatch: a tap on the dial starts and stops; the button at the top records
 * a lap while running and resets when stopped. It sits above the time, so the
 * lap list below can grow without being covered. Elapsed time comes from monotonic_ms,
 * so wall-clock changes do not affect it. The system dial's second ring lights
 * its ticks up to the current second. A 100 ms timer runs only while the
 * stopwatch runs, and while a refused update waits for its redraw.
 * No LVGL access. */
#include "rep/rep_app_v3.h"
#include <stdbool.h>
REP3_APP_METADATA;

#define TICK_MS 100U
#define LAPS_SHOWN 5U
#define RING_COLOR 0xff9f0a
#define TEXT_COLOR 0xffffff
#define PAUSED_COLOR 0xff9f0a /* the time while stopped with time on it */
#define LAPS_COLOR 0x9aa5b1
#define LAP_COLOR 0xd1d5db
#define RESET_COLOR 0xff453a
#define STALE_TICKS_LIMIT 50U /* 5 s of refused redraws: then the refusal is an error */

static const rep_api_v3_t *api;
static uint32_t view;
static uint32_t dial;
static uint32_t time_label;
static uint32_t laps_label;
static uint32_t lap_button;
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
static bool stale; /* an update was refused; the next tick redraws everything */
static uint32_t stale_ticks; /* consecutive refused redraws */
static rep_ui_update_t update = {1, sizeof(update), 0, REP_UI_TEXT, 0, ""};
static rep_ui_widget_t view_widget = {1, sizeof(rep_ui_widget_t), REP_UI_VIEW, 0, 0, 0, 360, 360, 0, ""};
/* The dial covers the face; widgets created after it (the lap button) stay
 * above it, and the labels pass touches through to it. */
static rep_ui_widget_v2_t dial_widget = {
    {2, sizeof(rep_ui_widget_v2_t), REP_UI_CLOCK_HANDS, 0, 0, 0, 0, 0, 0, ""},
    0, 0, 0, REP_UI_OPTION_SECOND_RING | REP_UI_OPTION_TAP};
static rep_ui_widget_v2_t time_widget = {
    {2, sizeof(rep_ui_widget_v2_t), REP_UI_LABEL, 0, 40, 136, 280, 60, 7, "00:00.0"},
    0, 0, 0, REP_UI_OPTION_FONT_LARGE};
/* Five lines of 18 px; every corner stays inside the round panel. */
static rep_ui_widget_t laps_widget = {1, sizeof(rep_ui_widget_t), REP_UI_LABEL, 0, 70, 204, 220, 96, 0, ""};
static rep_ui_widget_t lap_widget = {
    1, sizeof(rep_ui_widget_t), REP_UI_BUTTON, 0, 110, 72, 140, 48, 11, "Okrążenie"};

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

/* Newest lap first: "3.  00:12.4". At zero, a hint instead. */
static int32_t show_laps(uint32_t instance)
{
    uint32_t length = 0;
    if (!running && base_ms == 0U) {
        length = append(0, "Dotknij, aby uruchomić");
    }
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

/* The lap button: Lap while running, Reset when stopped with time, disabled
 * at zero. The time turns amber while stopped with time on it. */
static int32_t show_state(uint32_t instance)
{
    bool reset = !running && base_ms != 0U;
    uint32_t length = append(0, reset ? "Zeruj" : "Okrążenie");
    if (set_text(instance, lap_button, length) != 0 ||
        set(instance, lap_button, REP_UI_COLOR, reset ? RESET_COLOR : LAP_COLOR) != 0 ||
        set(instance, lap_button, REP_UI_ENABLED, running || reset) != 0 ||
        set(instance, time_label, REP_UI_COLOR, reset ? PAUSED_COLOR : TEXT_COLOR) != 0) {
        return -24;
    }
    return 0;
}

static int32_t start_or_stop(uint32_t instance)
{
    bool was_zero = base_ms == 0U;
    if (running) {
        base_ms = elapsed(instance);
        running = false;
        if (!stale) { /* a pending redraw keeps the timer */
            if (api->timer_cancel(instance, timer) != 0) {
                return -26;
            }
            timer = 0;
        }
    } else {
        start_ms = api->monotonic_ms(instance);
        running = true;
        if (timer == 0) { /* a pending redraw may hold it already */
            timer = api->timer_start(instance, TICK_MS);
            if (timer == 0) {
                return -27;
            }
        }
    }
    int32_t result = show_time(instance, base_ms); /* exact on stop; the start point on start */
    if (result == 0 && was_zero) {
        result = show_laps(instance); /* removes the hint */
    }
    return result != 0 ? result : show_state(instance);
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
    return result != 0 ? result : show_state(instance);
}

/* The UI queue is bounded (12 commands) and refuses more while the GUI task
 * catches up: a stop and a reset handled within one GUI frame queue 13. A
 * refused update (-21..-24) is therefore not an error: the next timer tick
 * redraws the whole state. Only a refusal that lasts STALE_TICKS_LIMIT ticks is
 * returned. The timer runs while stale, also when stopped; if it cannot start,
 * the system is stopping the app and there is nothing left to redraw. */
static int32_t refused(uint32_t instance, int32_t result)
{
    if (result > -21 || result < -24) {
        return result; /* 0, or not a UI refusal */
    }
    stale = true;
    if (timer == 0) {
        timer = api->timer_start(instance, TICK_MS);
    }
    return 0;
}

static int32_t redraw(uint32_t instance)
{
    shown_seconds = UINT32_MAX; /* resend the dial as well */
    int32_t result = show_time(instance, elapsed(instance));
    if (result == 0) {
        result = show_laps(instance);
    }
    if (result == 0) {
        result = show_state(instance);
    }
    if (result != 0) {
        stale_ticks++;
        return stale_ticks > STALE_TICKS_LIMIT ? result : 0; /* still stale: next tick */
    }
    stale = false;
    stale_ticks = 0;
    if (!running) {
        if (api->timer_cancel(instance, timer) != 0) {
            return -26;
        }
        timer = 0;
    }
    return 0;
}

static int32_t initialize(const rep_context_v3_t *context)
{
    uint32_t instance = context->instance;
    running = false;
    stale = false;
    stale_ticks = 0;
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
    dial = api->ui_create(instance, &dial_widget.base);
    time_label = api->ui_create(instance, &time_widget.base);
    laps_label = api->ui_create(instance, &laps_widget);
    lap_button = api->ui_create(instance, &lap_widget);
    if (dial == 0 || time_label == 0 || laps_label == 0 || lap_button == 0) {
        return -12;
    }
    /* Five creations plus six updates: within the system's 12-entry UI queue. */
    if (set(instance, view, REP_UI_COLOR, 0x000000) != 0 ||
        set(instance, dial, REP_UI_COLOR, RING_COLOR) != 0 ||
        set(instance, laps_label, REP_UI_COLOR, LAPS_COLOR) != 0 ||
        set(instance, lap_button, REP_UI_COLOR, LAP_COLOR) != 0 ||
        set(instance, lap_button, REP_UI_ENABLED, 0) != 0) {
        return -13;
    }
    return show_laps(instance);
}

static int32_t event(const rep_context_v3_t *context, const rep_event_v3_t *event)
{
    uint32_t instance = context->instance;
    if (event->type == REP2_EVENT_TIMER && timer != 0 && event->value == timer) {
        if (stale) {
            return redraw(instance);
        }
        return running ? refused(instance, show_time(instance, elapsed(instance))) : 0;
    }
    if (event->type == REP3_EVENT_BUTTON && event->value == dial) { /* tap */
        return refused(instance, start_or_stop(instance));
    }
    if (event->type == REP3_EVENT_BUTTON && event->value == lap_button) {
        return refused(instance, lap_or_reset(instance));
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
