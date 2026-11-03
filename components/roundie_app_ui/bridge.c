#include "ui_private.h"
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t gui;
static roundie_ui_status_t status;
static roundie_ui_event_sink_t event_sink;
static uint32_t handle_serial;
/* Shadow state (REP section 12.6) lives here, in GUI order: committed changes
 * when the GUI applies a request or reports input; requested at acceptance. */
static struct {
    uint32_t handle;
    uint32_t type;
    bool ready;
    bool visible;
    bool enabled;
    uint32_t items; /* LIST items or DIALOG fields of the newest accepted text */
    int32_t minimum;
    int32_t maximum;
    int32_t committed;
    int32_t requested;
    uint32_t pending; /* accepted value-changing commands not yet applied */
} slots[REP_UI_WIDGET_LIMIT];
static struct {
    bool pending;
    uint32_t generation;
    uint32_t widget;
    int32_t value;
} injection;
static roundie_ui_command_t commands[ROUNDIE_UI_QUEUE_CAPACITY];
static uint32_t head;
static uint32_t count;
static roundie_ui_transition_t transition;
static uint32_t requested;
static uint32_t completed;
static bool transition_result;
static bool closed;

static void maximum(uint32_t *value, uint32_t sample)
{
    if (sample > *value) {
        *value = sample;
    }
}

static int find(uint32_t handle)
{
    for (unsigned i = 0; i < REP_UI_WIDGET_LIMIT; i++) {
        if (handle != 0 && slots[i].handle == handle) {
            return (int)i;
        }
    }
    return -1;
}

/* Well-formed UTF-8, no embedded NUL, no surrogate/overlong/out-of-range scalar.
 * The system's built-in font need not cover every accepted Unicode character. */
static bool text_valid(const char *text, uint32_t length)
{
    if (length >= REP_UI_TEXT_CAPACITY || text[length] != '\0') {
        return false;
    }
    uint32_t i = 0;
    while (i < length) {
        uint8_t c = (uint8_t)text[i++];
        if (c == 0) {
            return false;
        }
        if (c < 0x80) {
            continue;
        }
        uint32_t extra = c >= 0xc2 && c <= 0xdf ? 1 :
            c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (extra == 0 || extra > length - i) {
            return false;
        }
        uint8_t next = (uint8_t)text[i];
        if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) ||
            (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90)) {
            return false;
        }
        for (uint32_t n = 0; n < extra; n++) {
            if (((uint8_t)text[i++] & 0xc0) != 0x80) {
                return false;
            }
        }
    }
    return true;
}

uint32_t roundie_ui_text_fields(const char *text, uint32_t length)
{
    uint32_t fields = 1;
    uint32_t field_length = 0;
    for (uint32_t i = 0; i < length; i++) {
        if (text[i] == '\n') {
            if (field_length == 0) {
                return 0;
            }
            fields++;
            field_length = 0;
        } else {
            field_length++;
        }
    }
    return field_length == 0 ? 0 : fields;
}

bool roundie_ui_box_in_circle(int32_t x, int32_t y, int32_t width, int32_t height)
{
    if (x < 0 || y < 0 || width <= 0 || height <= 0 || width > 360 - x || height > 360 - y) {
        return false;
    }
    /* Every corner within radius 176 of the panel centre (180, 180). */
    int32_t xs[2] = {x, x + width};
    int32_t ys[2] = {y, y + height};
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned j = 0; j < 2; j++) {
            int32_t dx = xs[i] - 180;
            int32_t dy = ys[j] - 180;
            if (dx * dx + dy * dy > 176 * 176) {
                return false;
            }
        }
    }
    return true;
}

static bool round_type(uint32_t type)
{
    return type >= REP_UI_LIST && type <= REP_UI_CLOCK_HANDS;
}

static bool interactive(uint32_t type)
{
    return type == REP_UI_BUTTON || (type >= REP_UI_LIST && type <= REP_UI_DIALOG);
}

/* System-placed widgets would overlap: one of each per view. */
static bool single_per_view(uint32_t type)
{
    return type == REP_UI_ARC_SLIDER || type == REP_UI_EDGE_BUTTON || type == REP_UI_DIALOG ||
        type == REP_UI_CLOCK_HANDS;
}

/* Widgets with shadow state: realized value, requested value, pending count. */
static bool value_type(uint32_t type)
{
    return type == REP_UI_LIST || type == REP_UI_ARC_SLIDER || type == REP_UI_CLOCK_HANDS;
}

static bool button_type(uint32_t type)
{
    return type == REP_UI_BUTTON || type == REP_UI_EDGE_BUTTON;
}

static bool system_geometry(const rep_ui_widget_t *base)
{
    return base->x == 0 && base->y == 0 && base->width == 0 && base->height == 0;
}

/* Format of a version-2 request, independent of other widgets. */
static bool round_format(const rep_ui_widget_v2_t *widget, uint32_t *items)
{
    const rep_ui_widget_t *base = &widget->base;
    *items = 0;
    bool label = base->type == REP_UI_LABEL;
    uint32_t allowed = REP_UI_OPTION_HIDDEN;
    if (label) {
        allowed |= REP_UI_OPTION_FONT_MEDIUM | REP_UI_OPTION_FONT_LARGE;
    } else if (base->type == REP_UI_CLOCK_HANDS) {
        allowed |= REP_UI_OPTION_NO_SECONDS;
    }
    if (base->version != 2 || base->struct_size != sizeof(*widget) || (!label && !round_type(base->type)) ||
        (widget->options & ~allowed) != 0 || !text_valid(base->text, base->text_length)) {
        return false;
    }
    if (label) {
        /* A styled label: version-1 label geometry plus one font size. */
        uint32_t fonts = widget->options & (REP_UI_OPTION_FONT_MEDIUM | REP_UI_OPTION_FONT_LARGE);
        return fonts != (REP_UI_OPTION_FONT_MEDIUM | REP_UI_OPTION_FONT_LARGE) &&
            base->x >= 0 && base->x <= 360 && base->y >= 0 && base->y <= 360 &&
            base->width > 0 && base->width <= 360 - base->x &&
            base->height > 0 && base->height <= 360 - base->y &&
            widget->value == 0 && widget->minimum == 0 && widget->maximum == 0;
    }
    if (base->type == REP_UI_LIST) {
        *items = roundie_ui_text_fields(base->text, base->text_length);
        return roundie_ui_box_in_circle(base->x, base->y, base->width, base->height) &&
            base->width >= 44 && base->height >= 44 &&
            *items >= 1 && *items <= REP_UI_LIST_ITEM_LIMIT &&
            widget->value >= 0 && (uint32_t)widget->value < *items &&
            widget->minimum == 0 && widget->maximum == 0;
    }
    if (!system_geometry(base)) {
        return false;
    }
    if (base->type == REP_UI_CLOCK_HANDS) {
        return base->text_length == 0 && widget->minimum == 0 && widget->maximum == 0 &&
            widget->value >= 0 && widget->value < REP_UI_CLOCK_HANDS_PERIOD;
    }
    if (base->type == REP_UI_ARC_SLIDER) {
        return base->text_length == 0 &&
            widget->minimum >= -REP_UI_SLIDER_RANGE_LIMIT && widget->maximum <= REP_UI_SLIDER_RANGE_LIMIT &&
            widget->minimum < widget->maximum &&
            widget->value >= widget->minimum && widget->value <= widget->maximum;
    }
    if (widget->value != 0 || widget->minimum != 0 || widget->maximum != 0) {
        return false;
    }
    if (base->type == REP_UI_DIALOG) {
        *items = roundie_ui_text_fields(base->text, base->text_length);
        return *items == 2 || *items == 3;
    }
    return true; /* EDGE_BUTTON */
}

static bool admission(uint32_t generation)
{
    return generation != 0 && generation == status.generation && status.admitting &&
        !closed && status.error == 0 && requested == completed;
}

static bool enqueue(roundie_ui_command_t *command)
{
    if (count == ROUNDIE_UI_QUEUE_CAPACITY || status.submitted == UINT32_MAX) {
        return false;
    }
    command->posted_us = esp_timer_get_time();
    commands[(head + count) % ROUNDIE_UI_QUEUE_CAPACITY] = *command;
    count++;
    status.submitted++;
    status.acquired++;
    maximum(&status.max_depth, count);
    return true;
}

bool roundie_ui_open(uint32_t generation, roundie_ui_event_sink_t sink)
{
    portENTER_CRITICAL(&lock);
    bool valid = gui != NULL && generation != 0 && status.generation == 0;
    if (valid) {
        uint32_t flushes = status.flushes;
        uint32_t errors = status.flush_errors;
        memset(&status, 0, sizeof(status));
        status.flushes = flushes;
        status.flush_errors = errors;
        status.generation = generation;
        status.admitting = true;
        event_sink = sink;
        closed = false;
        requested = 0;
        completed = 0;
        transition = 0;
    }
    portEXIT_CRITICAL(&lock);
    return valid;
}

uint32_t roundie_ui_create(uint32_t generation, const rep_ui_widget_v2_t *request)
{
    int64_t started = esp_timer_get_time();
    const rep_ui_widget_t *widget = &request->base;
    uint32_t items = 0;
    bool round = widget->version == 2;
    bool format = round ? round_format(request, &items) :
        widget->version == 1 && widget->struct_size == sizeof(*widget) &&
        widget->type >= REP_UI_VIEW && widget->type <= REP_UI_BUTTON &&
        widget->x >= 0 && widget->x <= 360 && widget->y >= 0 && widget->y <= 360 &&
        widget->width > 0 && widget->width <= 360 - widget->x &&
        widget->height > 0 && widget->height <= 360 - widget->y &&
        (widget->type != REP_UI_BUTTON || (widget->width >= 44 && widget->height >= 44)) &&
        text_valid(widget->text, widget->text_length);
    portENTER_CRITICAL(&lock);
    bool root = widget->type == REP_UI_VIEW;
    bool parent = root ? widget->parent == 0 && status.view == 0 &&
        widget->x == 0 && widget->y == 0 && widget->width == 360 && widget->height == 360 :
        widget->parent != 0 && widget->parent == status.view;
    /* One system-placed ARC_SLIDER, EDGE_BUTTON and DIALOG per view. */
    if (round && single_per_view(widget->type)) {
        for (unsigned i = 0; i < REP_UI_WIDGET_LIMIT; i++) {
            if (slots[i].handle != 0 && slots[i].type == widget->type) {
                parent = false;
            }
        }
    }
    uint32_t result = 0;
    if (format && parent && admission(generation) && handle_serial != UINT32_MAX) {
        for (unsigned i = 0; i < REP_UI_WIDGET_LIMIT; i++) {
            if (slots[i].handle != 0) {
                continue;
            }
            roundie_ui_command_t command = {
                .generation = generation, .handle = handle_serial + 1, .widget = *request,
            };
            if (enqueue(&command)) {
                result = ++handle_serial;
                memset(&slots[i], 0, sizeof(slots[i]));
                slots[i].handle = result;
                slots[i].type = widget->type;
                slots[i].visible = (request->options & REP_UI_OPTION_HIDDEN) == 0;
                slots[i].enabled = true;
                slots[i].items = items;
                slots[i].minimum = request->minimum;
                slots[i].maximum = request->maximum;
                if (widget->type == REP_UI_CLOCK_HANDS) {
                    slots[i].maximum = REP_UI_CLOCK_HANDS_PERIOD - 1;
                }
                slots[i].committed = request->value;
                slots[i].requested = request->value;
                if (value_type(widget->type)) {
                    slots[i].pending = 1; /* creation not yet applied */
                }
                status.widgets++;
                status.acquired++; /* logical widget */
                if (root) {
                    status.view = result;
                }
                if (button_type(widget->type)) {
                    status.button = result;
                }
                if (interactive(widget->type)) {
                    status.acquired++; /* input registration */
                }
            }
            break;
        }
    }
    if (result == 0) {
        status.rejected++;
    }
    maximum(&status.enqueue_max_us, (uint32_t)(esp_timer_get_time() - started));
    portEXIT_CRITICAL(&lock);
    return result;
}

int32_t roundie_ui_update(uint32_t generation, const rep_ui_update_t *update)
{
    int64_t started = esp_timer_get_time();
    bool format = update->version == 1 && update->struct_size == sizeof(*update) &&
        (update->property == REP_UI_TEXT ? text_valid(update->text, update->value) :
         update->property == REP_UI_VALUE ||
         (update->property == REP_UI_COLOR && update->value <= UINT32_C(0xffffff)) ||
         ((update->property == REP_UI_VISIBLE || update->property == REP_UI_ENABLED) && update->value <= 1));
    uint32_t fields = update->property == REP_UI_TEXT ? roundie_ui_text_fields(update->text, update->value) : 0;
    int32_t value = (int32_t)update->value;
    portENTER_CRITICAL(&lock);
    int index = find(update->widget);
    bool valid = format && admission(generation) && index >= 0;
    if (valid && update->property != REP_UI_COLOR) {
        valid = slots[index].type != REP_UI_VIEW; /* root visibility is lifecycle-owned */
    }
    bool value_change = false;
    if (valid && update->property == REP_UI_TEXT) {
        uint32_t type = slots[index].type;
        if (type == REP_UI_ARC_SLIDER || type == REP_UI_CLOCK_HANDS) {
            valid = false;
        } else if (type == REP_UI_LIST) {
            valid = fields >= 1 && fields <= REP_UI_LIST_ITEM_LIMIT;
            value_change = true;
            value = 0; /* new items select the first */
        } else if (type == REP_UI_DIALOG) {
            valid = fields == slots[index].items;
        }
    }
    if (valid && update->property == REP_UI_VALUE) {
        if (slots[index].type == REP_UI_LIST) {
            valid = value >= 0 && (uint32_t)value < slots[index].items;
        } else if (slots[index].type == REP_UI_ARC_SLIDER || slots[index].type == REP_UI_CLOCK_HANDS) {
            valid = value >= slots[index].minimum && value <= slots[index].maximum;
        } else {
            valid = false;
        }
        value_change = true;
    }
    if (valid) {
        roundie_ui_command_t command = {
            .generation = generation, .handle = update->widget,
            .operation = update->property, .value = update->value,
        };
        memcpy(command.widget.base.text, update->text, sizeof(update->text));
        valid = enqueue(&command);
    }
    if (valid && value_change) {
        slots[index].requested = value;
        slots[index].pending++;
        if (slots[index].type == REP_UI_LIST && update->property == REP_UI_TEXT) {
            slots[index].items = fields;
        }
    }
    if (!valid) {
        status.rejected++;
    }
    maximum(&status.enqueue_max_us, (uint32_t)(esp_timer_get_time() - started));
    portEXIT_CRITICAL(&lock);
    return valid ? 0 : -1;
}

void roundie_ui_close_admission(uint32_t generation)
{
    portENTER_CRITICAL(&lock);
    if (generation == status.generation) {
        status.admitting = false;
    }
    portEXIT_CRITICAL(&lock);
}

int32_t roundie_ui_destroy(uint32_t generation, uint32_t view)
{
    portENTER_CRITICAL(&lock);
    bool valid = admission(generation) && view != 0 && view == status.view;
    if (valid) {
        closed = true;
        status.admitting = false;
        transition = ROUNDIE_UI_DESTROY;
        requested++;
    } else {
        status.rejected++;
    }
    portEXIT_CRITICAL(&lock);
    return valid ? 0 : -1;
}

bool roundie_ui_transition(uint32_t generation, roundie_ui_transition_t operation)
{
    portENTER_CRITICAL(&lock);
    if (generation != status.generation || operation < ROUNDIE_UI_SHOW || operation > ROUNDIE_UI_DESTROY) {
        portEXIT_CRITICAL(&lock);
        return false;
    }
    if (requested == completed) {
        if (operation == ROUNDIE_UI_DESTROY && closed && status.widgets == 0) {
            portEXIT_CRITICAL(&lock);
            return true;
        }
        transition = operation;
        requested++;
    } else if (transition != operation) {
        /* Do not overtake an in-flight GUI control. Retain and retry cleanup. */
        portEXIT_CRITICAL(&lock);
        return false;
    }
    uint32_t serial = requested;
    status.admitting = false;
    if (operation == ROUNDIE_UI_DESTROY) {
        closed = true;
    }
    portEXIT_CRITICAL(&lock);
    int64_t deadline = esp_timer_get_time() + 1000000;
    do {
        portENTER_CRITICAL(&lock);
        bool done = completed == serial;
        bool result = transition_result;
        portEXIT_CRITICAL(&lock);
        if (done) {
            return result;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (esp_timer_get_time() < deadline);
    return false;
}

bool roundie_ui_retire(uint32_t generation)
{
    portENTER_CRITICAL(&lock);
    bool valid = generation == status.generation && closed && status.widgets == 0 &&
        count == 0 && completed == requested;
    if (valid) {
        status.generation = 0;
        status.admitting = false;
        event_sink = NULL;
    }
    portEXIT_CRITICAL(&lock);
    return valid;
}

void roundie_ui_status(roundie_ui_status_t *out)
{
    portENTER_CRITICAL(&lock);
    *out = status;
    out->pending = count;
    out->outstanding = status.acquired - status.released;
    portEXIT_CRITICAL(&lock);
}

bool roundie_ui_shadow(uint32_t generation, uint32_t widget, int32_t *committed,
                       int32_t *requested, uint32_t *pending)
{
    portENTER_CRITICAL(&lock);
    int index = find(widget);
    bool valid = generation != 0 && generation == status.generation && index >= 0 &&
        (value_type(slots[index].type) || slots[index].type == REP_UI_DIALOG);
    if (valid) {
        *committed = slots[index].committed;
        *requested = slots[index].requested;
        *pending = slots[index].pending != 0;
    }
    portEXIT_CRITICAL(&lock);
    return valid;
}

/* Interactive, realized, visible and enabled in the foreground. */
static bool target_valid(uint32_t generation, int index)
{
    return admission(generation) && status.foreground && index >= 0 &&
        slots[index].ready && slots[index].visible && slots[index].enabled;
}

static bool input_valid(uint32_t generation, uint32_t widget)
{
    int index = find(widget);
    return target_valid(generation, index) && button_type(slots[index].type);
}

static bool value_valid(int index, int32_t value)
{
    if (slots[index].type == REP_UI_LIST) {
        return value >= 0 && (uint32_t)value < slots[index].items;
    }
    if (slots[index].type == REP_UI_ARC_SLIDER) {
        return value >= slots[index].minimum && value <= slots[index].maximum;
    }
    if (slots[index].type == REP_UI_DIALOG) {
        return value >= 1 && (uint32_t)value < slots[index].items;
    }
    return false;
}

/* At delivery. LIST/ARC_SLIDER events describe state: one superseded by a later
 * commit (input or applied request) is stale. A DIALOG choice is an action and
 * the dialog hides itself, so it stays valid while the widget is enabled. */
bool roundie_ui_event_valid(uint32_t generation, uint32_t type, uint32_t widget, uint32_t payload)
{
    portENTER_CRITICAL(&lock);
    bool valid = false;
    if (type == REP3_EVENT_BUTTON) {
        valid = input_valid(generation, widget);
    } else if (type == REP3_EVENT_VALUE) {
        int index = find(widget);
        if (index >= 0 && slots[index].type == REP_UI_DIALOG) {
            valid = admission(generation) && status.foreground && slots[index].ready &&
                slots[index].enabled && (int32_t)payload == slots[index].committed;
        } else {
            valid = target_valid(generation, index) && round_type(slots[index].type) &&
                (int32_t)payload == slots[index].committed;
        }
    }
    portEXIT_CRITICAL(&lock);
    return valid;
}

bool roundie_ui_input(uint32_t generation, uint32_t widget, uint32_t origin)
{
    portENTER_CRITICAL(&lock);
    bool valid = input_valid(generation, widget) && event_sink != NULL &&
        (origin == REP3_ORIGIN_TOUCH || origin == REP3_ORIGIN_INJECTED);
    roundie_ui_event_sink_t sink = event_sink;
    if (!valid) {
        status.input_rejected++;
    }
    portEXIT_CRITICAL(&lock);
    if (!valid) {
        return false;
    }
    bool accepted = sink(generation, REP3_EVENT_BUTTON, widget, origin, false, esp_timer_get_time());
    portENTER_CRITICAL(&lock);
    if (accepted) {
        status.input_accepted++;
        status.physical += origin == REP3_ORIGIN_TOUCH;
        status.injected += origin == REP3_ORIGIN_INJECTED;
    } else {
        status.input_dropped++;
    }
    portEXIT_CRITICAL(&lock);
    return accepted;
}

bool roundie_ui_value_input(uint32_t generation, uint32_t widget, int32_t value, uint32_t origin)
{
    portENTER_CRITICAL(&lock);
    int index = find(widget);
    bool valid = target_valid(generation, index) && event_sink != NULL && value_valid(index, value) &&
        (origin == REP3_ORIGIN_TOUCH || origin == REP3_ORIGIN_INJECTED);
    bool dialog = valid && slots[index].type == REP_UI_DIALOG;
    if (valid && !dialog) {
        /* LVGL already shows this value; a pending app request will replace it. */
        slots[index].committed = value;
        if (slots[index].pending == 0) {
            slots[index].requested = value;
        }
    }
    roundie_ui_event_sink_t sink = event_sink;
    if (!valid) {
        status.input_rejected++;
    }
    portEXIT_CRITICAL(&lock);
    if (!valid) {
        return false;
    }
    bool accepted = sink(generation, REP3_EVENT_VALUE, widget, (uint32_t)value, !dialog, esp_timer_get_time());
    portENTER_CRITICAL(&lock);
    index = find(widget);
    if (accepted) {
        status.input_accepted++;
        status.values++;
        status.physical += origin == REP3_ORIGIN_TOUCH;
        status.injected += origin == REP3_ORIGIN_INJECTED;
        if (dialog && index >= 0) {
            /* The renderer hides the dialog after an accepted choice. */
            slots[index].committed = value;
            slots[index].requested = value;
            slots[index].visible = false;
        }
    } else {
        status.input_dropped++;
    }
    portEXIT_CRITICAL(&lock);
    return accepted;
}

bool roundie_ui_inject_value(uint32_t generation, uint32_t widget, int32_t value)
{
    portENTER_CRITICAL(&lock);
    bool valid = admission(generation) && find(widget) >= 0 && !injection.pending;
    if (valid) {
        injection.pending = true;
        injection.generation = generation;
        injection.widget = widget;
        injection.value = value;
    } else {
        status.input_rejected++;
    }
    portEXIT_CRITICAL(&lock);
    return valid;
}

void roundie_ui_delivered(uint32_t generation, uint32_t latency_us)
{
    portENTER_CRITICAL(&lock);
    if (generation == status.generation) {
        status.delivered++;
        maximum(&status.input_latency_max_us, latency_us);
    }
    portEXIT_CRITICAL(&lock);
}

void roundie_ui_gui_bind(void)
{
    portENTER_CRITICAL(&lock);
    if (gui == NULL) {
        gui = xTaskGetCurrentTaskHandle();
    }
    portEXIT_CRITICAL(&lock);
}

static unsigned batch_limit = ROUNDIE_UI_QUEUE_CAPACITY;

void roundie_ui_gui_batch(unsigned limit)
{
    if (gui == xTaskGetCurrentTaskHandle() && (limit == ROUNDIE_UI_BATCH || limit == ROUNDIE_UI_QUEUE_CAPACITY)) {
        batch_limit = limit;
    }
}

void roundie_ui_gui_pump(void)
{
    if (gui == NULL || gui != xTaskGetCurrentTaskHandle()) {
        return;
    }
    portENTER_CRITICAL(&lock);
    bool inject = injection.pending;
    uint32_t inject_generation = injection.generation;
    uint32_t inject_widget = injection.widget;
    int32_t inject_value = injection.value;
    injection.pending = false;
    int inject_index = find(inject_widget);
    if (inject) {
        /* Check before LVGL changes, so the shadow never diverges from it. */
        inject = target_valid(inject_generation, inject_index) && value_valid(inject_index, inject_value);
        if (!inject) {
            status.input_rejected++;
        }
    }
    portEXIT_CRITICAL(&lock);
    if (inject) {
        roundie_ui_render_inject(inject_generation, inject_widget, inject_value);
    }
    for (unsigned n = 0; n < batch_limit; n++) {
        portENTER_CRITICAL(&lock);
        roundie_ui_transition_t control = requested != completed ? transition : 0;
        bool destroy = control == ROUNDIE_UI_DESTROY;
        bool execute_control = control != 0 && (control != ROUNDIE_UI_SHOW || count == 0);
        uint32_t serial = requested;
        if (execute_control) {
            if (destroy) {
                status.canceled += count;
                status.released += count;
                count = 0;
                head = 0;
            }
            bool healthy = status.error == 0 && status.view != 0;
            portEXIT_CRITICAL(&lock);
            int64_t started = esp_timer_get_time();
            bool success = true;
            if (destroy) {
                roundie_ui_render_destroy();
            } else {
                success = roundie_ui_render_show(control == ROUNDIE_UI_SHOW && healthy);
                success = success && (control != ROUNDIE_UI_SHOW || healthy);
            }
            uint32_t bytes = roundie_ui_render_bytes();
            uint32_t pool_before;
            uint32_t pool_free;
            uint32_t objects;
            roundie_ui_render_metrics(&pool_before, &pool_free, &objects);
            portENTER_CRITICAL(&lock);
            status.pool_before = pool_before;
            status.pool_free = pool_free;
            status.realized_objects = objects;
            status.foreground = control == ROUNDIE_UI_SHOW && success;
            status.admitting = success && !closed && control == ROUNDIE_UI_SHOW;
            status.lvgl_bytes = bytes;
            if (destroy) {
                for (unsigned i = 0; i < REP_UI_WIDGET_LIMIT; i++) {
                    status.released += slots[i].handle != 0;
                    status.released += slots[i].handle != 0 && interactive(slots[i].type);
                }
                injection.pending = false;
                memset(slots, 0, sizeof(slots));
                status.widgets = 0;
                status.view = 0;
                status.button = 0;
                status.teardown_us = (uint32_t)(esp_timer_get_time() - started);
            }
            transition_result = success;
            completed = serial;
            portEXIT_CRITICAL(&lock);
            break;
        }
        if (count == 0) {
            portEXIT_CRITICAL(&lock);
            break;
        }
        roundie_ui_command_t command = commands[head];
        head = (head + 1) % ROUNDIE_UI_QUEUE_CAPACITY;
        count--;
        bool valid = command.generation == status.generation && !closed && status.error == 0;
        portEXIT_CRITICAL(&lock);
        int64_t started = esp_timer_get_time();
        bool success = valid && roundie_ui_render(&command);
        uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
        uint32_t bytes = roundie_ui_render_bytes();
        uint32_t pool_before;
        uint32_t pool_free;
        uint32_t objects;
        roundie_ui_render_metrics(&pool_before, &pool_free, &objects);
        portENTER_CRITICAL(&lock);
        status.pool_before = pool_before;
        status.pool_free = pool_free;
        status.realized_objects = objects;
        status.released++;
        if (success) {
            status.applied++;
            int index = find(command.handle);
            if (index >= 0) {
                slots[index].ready = true;
                if (command.operation == REP_UI_VISIBLE) {
                    slots[index].visible = command.value != 0;
                }
                if (command.operation == REP_UI_ENABLED) {
                    slots[index].enabled = command.value != 0;
                }
                bool list = slots[index].type == REP_UI_LIST;
                bool value_change = command.operation == REP_UI_VALUE ||
                    (command.operation == 0 && value_type(slots[index].type)) ||
                    (command.operation == REP_UI_TEXT && list);
                if (value_change && slots[index].pending != 0) {
                    slots[index].pending--;
                    if (command.operation == REP_UI_VALUE) {
                        slots[index].committed = (int32_t)command.value;
                    } else if (command.operation == REP_UI_TEXT) {
                        slots[index].committed = 0;
                    }
                    if (slots[index].pending == 0) {
                        slots[index].requested = slots[index].committed;
                    }
                }
            }
            status.last_widget = command.handle;
            if (command.operation == REP_UI_TEXT || command.operation == 0) {
                uint32_t hash = UINT32_C(2166136261);
                const unsigned char *text = (const unsigned char *)command.widget.base.text;
                while (*text != 0) {
                    hash = (hash ^ *text++) * UINT32_C(16777619);
                }
                status.text_hash = hash;
            }
        } else {
            status.failed++;
            status.error = -1;
            status.admitting = false;
        }
        maximum(&status.latency_max_us, (uint32_t)(started - command.posted_us));
        maximum(command.operation == 0 ? &status.create_max_us : &status.update_max_us, elapsed);
        status.lvgl_bytes = bytes;
        maximum(&status.lvgl_peak, bytes);
        portEXIT_CRITICAL(&lock);
    }
}

void roundie_ui_display_progress(uint32_t completed_flushes, uint32_t errors)
{
    portENTER_CRITICAL(&lock);
    status.flushes = completed_flushes;
    status.flush_errors = errors;
    portEXIT_CRITICAL(&lock);
}
