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
static struct {
    uint32_t handle;
    uint32_t type;
    bool ready;
    bool visible;
    bool enabled;
} slots[REP_UI_WIDGET_LIMIT];
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

uint32_t roundie_ui_create(uint32_t generation, const rep_ui_widget_t *widget)
{
    int64_t started = esp_timer_get_time();
    bool format = widget->version == 1 && widget->struct_size == sizeof(*widget) &&
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
    uint32_t result = 0;
    if (format && parent && admission(generation) && handle_serial != UINT32_MAX) {
        for (unsigned i = 0; i < REP_UI_WIDGET_LIMIT; i++) {
            if (slots[i].handle != 0) {
                continue;
            }
            roundie_ui_command_t command = {
                .generation = generation, .handle = handle_serial + 1, .widget = *widget,
            };
            if (enqueue(&command)) {
                result = ++handle_serial;
                slots[i].handle = result;
                slots[i].type = widget->type;
                slots[i].visible = true;
                slots[i].enabled = true;
                status.widgets++;
                status.acquired++; /* logical widget */
                if (root) {
                    status.view = result;
                }
                if (widget->type == REP_UI_BUTTON) {
                    status.button = result;
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
         (update->property == REP_UI_VISIBLE || update->property == REP_UI_ENABLED) && update->value <= 1);
    portENTER_CRITICAL(&lock);
    int index = find(update->widget);
    bool valid = format && admission(generation) && index >= 0;
    if (valid) {
        valid = slots[index].type != REP_UI_VIEW; /* root visibility is lifecycle-owned */
    }
    if (valid) {
        roundie_ui_command_t command = {
            .generation = generation, .handle = update->widget,
            .operation = update->property, .value = update->value,
        };
        memcpy(command.widget.text, update->text, sizeof(update->text));
        valid = enqueue(&command);
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

static bool input_valid(uint32_t generation, uint32_t widget)
{
    int index = find(widget);
    return admission(generation) && status.foreground && index >= 0 &&
        slots[index].type == REP_UI_BUTTON && slots[index].ready &&
        slots[index].visible && slots[index].enabled;
}

bool roundie_ui_event_valid(uint32_t generation, uint32_t widget)
{
    portENTER_CRITICAL(&lock);
    bool valid = input_valid(generation, widget);
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
    bool accepted = sink(generation, widget, origin, esp_timer_get_time());
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

void roundie_ui_gui_pump(void)
{
    if (gui == NULL || gui != xTaskGetCurrentTaskHandle()) {
        return;
    }
    for (unsigned n = 0; n < ROUNDIE_UI_BATCH; n++) {
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
                    status.released += slots[i].handle != 0 && slots[i].type == REP_UI_BUTTON;
                }
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
            }
            status.last_widget = command.handle;
            if (command.operation == REP_UI_TEXT || command.operation == 0) {
                uint32_t hash = UINT32_C(2166136261);
                const unsigned char *text = (const unsigned char *)command.widget.text;
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
