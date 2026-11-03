/* Watch face: analog dial with hands, large digital time and date, driven by
 * REP3_EVENT_CLOCK and a one-second timer. A long press on the dial cycles
 * themes. No LVGL access; the system draws the dial. */
#include "rep/rep_app_v3.h"
#include <stdbool.h>
REP3_APP_METADATA;

#define MS_PER_DAY 86400000U
#define THEME_COUNT 3U

typedef struct {
    uint32_t background;
    uint32_t accent;
    uint32_t text;
} theme_t;

/* Terminal green, blue, amber. */
static const theme_t themes[THEME_COUNT] = {
    {0x021a0c, 0x1aff80, 0x1aff80},
    {0x0b1220, 0x3d9cff, 0xffffff},
    {0x000000, 0xffb000, 0xffd480},
};
/* Polish abbreviations in UTF-8; weekday 0 is Sunday. */
static const char *const weekdays[7] = {"ND", "PN", "WT", "ŚR", "CZW", "PT", "SOB"};
static const char *const months[12] = {
    "STY", "LUT", "MAR", "KWI", "MAJ", "CZE", "LIP", "SIE", "WRZ", "PAŹ", "LIS", "GRU",
};

static const rep_api_v3_t *api;
static uint32_t view;
static uint32_t hands;
static uint32_t date_label;
static uint32_t time_label;
static uint32_t theme;
/* Last clock sample: local day, millisecond of day and its monotonic time. */
static bool have_clock;
static uint32_t base_days;
static uint32_t base_ms;
static uint64_t base_us;
static uint32_t shown_minute;
static uint32_t shown_days;
static rep_ui_update_t update = {1, sizeof(update), 0, REP_UI_TEXT, 0, ""};
static rep_ui_widget_t view_widget = {1, sizeof(rep_ui_widget_t), REP_UI_VIEW, 0, 0, 0, 360, 360, 0, ""};
static rep_ui_widget_v2_t hands_widget = {
    {2, sizeof(rep_ui_widget_v2_t), REP_UI_CLOCK_HANDS, 0, 0, 0, 0, 0, 0, ""}, 0, 0, 0, REP_UI_OPTION_LONG_PRESS};
static rep_ui_widget_v2_t date_widget = {
    {2, sizeof(rep_ui_widget_v2_t), REP_UI_LABEL, 0, 70, 100, 220, 36, 0, ""},
    0, 0, 0, REP_UI_OPTION_FONT_MEDIUM};
static rep_ui_widget_v2_t time_widget = {
    {2, sizeof(rep_ui_widget_v2_t), REP_UI_LABEL, 0, 60, 204, 240, 60, 5, "--:--"},
    0, 0, 0, REP_UI_OPTION_FONT_LARGE};

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

static int32_t apply_theme(uint32_t instance)
{
    const theme_t *colors = &themes[theme];
    if (set(instance, view, REP_UI_COLOR, colors->background) != 0 ||
        set(instance, hands, REP_UI_COLOR, colors->accent) != 0 ||
        set(instance, date_label, REP_UI_COLOR, colors->text) != 0 ||
        set(instance, time_label, REP_UI_COLOR, colors->text) != 0) {
        return -20;
    }
    return 0;
}

/* Extrapolates local time from the last clock sample to now_us. Between clock
 * events (at most a minute apart) 32-bit microseconds suffice; a longer gap,
 * which only occurs while wall time is invalid, holds the last minute. */
static int32_t refresh(uint32_t instance, uint64_t now_us)
{
    uint64_t elapsed_us = now_us > base_us ? now_us - base_us : 0U;
    if (elapsed_us > UINT64_C(3600000000)) {
        elapsed_us = UINT64_C(3600000000);
    }
    uint32_t total_ms = base_ms + (uint32_t)elapsed_us / 1000U;
    uint32_t days = base_days + total_ms / MS_PER_DAY;
    uint32_t seconds = (total_ms % MS_PER_DAY) / 1000U;
    if (seconds >= 86400U) {
        seconds = 86399U; /* leap second */
    }
    if (set(instance, hands, REP_UI_VALUE, seconds % (uint32_t)REP_UI_CLOCK_HANDS_PERIOD) != 0) {
        return -21;
    }
    uint32_t minute = seconds / 60U;
    if (minute != shown_minute) {
        shown_minute = minute;
        uint32_t length = append_number(0, minute / 60U, true);
        length = append(length, ":");
        length = append_number(length, minute % 60U, true);
        if (set_text(instance, time_label, length) != 0) {
            return -22;
        }
    }
    if (days != shown_days) {
        shown_days = days;
        uint32_t year;
        uint32_t month;
        uint32_t day;
        uint32_t weekday;
        rep_clock_civil(days, &year, &month, &day, &weekday);
        uint32_t length = append(0, weekdays[weekday]);
        length = append(length, " ");
        length = append_number(length, day, false);
        length = append(length, " ");
        length = append(length, months[month - 1U]);
        if (set_text(instance, date_label, length) != 0) {
            return -23;
        }
    }
    return 0;
}

static int32_t initialize(const rep_context_v3_t *context)
{
    uint32_t instance = context->instance;
    have_clock = false;
    theme = 0;
    shown_minute = UINT32_MAX;
    shown_days = UINT32_MAX;
    view = api->ui_create(instance, &view_widget);
    if (view == 0) {
        return -11;
    }
    hands_widget.base.parent = view;
    date_widget.base.parent = view;
    time_widget.base.parent = view;
    hands = api->ui_create(instance, &hands_widget.base);
    date_label = api->ui_create(instance, &date_widget.base);
    time_label = api->ui_create(instance, &time_widget.base);
    if (hands == 0 || date_label == 0 || time_label == 0) {
        return -12;
    }
    if (set_text(instance, date_label, append(0, "Ustaw czas")) != 0) {
        return -13;
    }
    return apply_theme(instance);
}

static int32_t start(const rep_context_v3_t *context)
{
    return api->timer_start(context->instance, 1000) == 0 ? -14 : 0;
}

static int32_t event(const rep_context_v3_t *context, const rep_event_v3_t *event)
{
    uint32_t instance = context->instance;
    if (event->type == REP3_EVENT_CLOCK) {
        have_clock = true;
        base_days = event->value;
        base_ms = event->reserved;
        base_us = event->posted_us;
        return refresh(instance, event->posted_us);
    }
    if (event->type == REP2_EVENT_TIMER) {
        return have_clock ? refresh(instance, event->posted_us) : 0;
    }
    if (event->type == REP3_EVENT_BUTTON && event->value == hands) { /* long press */
        theme = (theme + 1U) % THEME_COUNT;
        return apply_theme(instance);
    }
    return 0;
}

static int32_t quiet(const rep_context_v3_t *context)
{
    (void)context;
    return 0;
}

static const rep_descriptor_v3_t descriptor = {
    3, sizeof(descriptor), initialize, start, event, quiet, quiet, quiet, quiet,
    REP3_CAP_UI | REP3_CAP_ROUND_UI | REP3_CAP_CLOCK | REP2_CAP_TIMER,
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
