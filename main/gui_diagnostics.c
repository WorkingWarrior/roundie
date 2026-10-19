#include "gui_diagnostics.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "roundie_app_ui.h"
#include "roundie_elf.h"
#include "roundie_launcher.h"

#define TRACE_COUNT 32U
#define TRACE_FIELDS 22U
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned batch = ROUNDIE_UI_QUEUE_CAPACITY;
static uint32_t sequence;
static uint64_t traces[TRACE_COUNT][TRACE_FIELDS];
static uint64_t loops;
static uint64_t flushes;
static uint64_t pixels;
static uint32_t handler_max;
static uint32_t pump_max;
static uint32_t wake_max;
static uint32_t dma_max;
static uint32_t submit_max;
static uint32_t wait_max;
static uint32_t errors_seen;
static uint32_t dma_completions;
static uint32_t stack_min = UINT32_MAX;

static void maximum(uint32_t *value, uint32_t sample)
{
    if (sample > *value) {
        *value = sample;
    }
}

unsigned roundie_gui_batch(void)
{
    portENTER_CRITICAL(&lock);
    unsigned value = batch;
    portEXIT_CRITICAL(&lock);
    return value;
}

void roundie_gui_record(int64_t at_us, uint32_t pump_us, uint32_t handler_us,
    uint32_t wake_us, uint32_t before, uint32_t after, uint32_t applied,
    const roundie_gui_work_t *work, uint32_t errors, uint32_t isr_done)
{
    /* GUI-only scratch, outside app_main's stack. */
    static roundie_launcher_status_t nav;
    roundie_launcher_status(&nav);
    uint32_t stack = uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&lock);
    loops++;
    flushes += work->flushes;
    pixels += work->pixels;
    maximum(&handler_max, handler_us);
    maximum(&pump_max, pump_us);
    maximum(&wake_max, wake_us);
    maximum(&dma_max, work->dma_max_us);
    maximum(&submit_max, work->submit_max_us);
    maximum(&wait_max, work->wait_max_us);
    errors_seen = errors;
    dma_completions = isr_done;
    if (stack < stack_min) {
        stack_min = stack;
    }
    /* Idle polling cannot evict useful transitions. Stop at serial exhaustion. */
    if ((work->flushes != 0 || applied != 0 || before != 0 || handler_us > 20000) && sequence != UINT32_MAX) {
        sequence++;
        const uint64_t sample[TRACE_FIELDS] = {sequence, (uint64_t)at_us,
            nav.state, nav.request, nav.generation, batch, before, after, applied,
            pump_us, handler_us, wake_us, work->flushes, work->done, work->pixels,
            work->submit_us, work->dma_us, work->wait_us, work->render_us,
            work->invalidations, work->invalid_pixels, work->full_invalidations};
        memcpy(traces[(sequence - 1) % TRACE_COUNT], sample, sizeof(sample));
    }
    portEXIT_CRITICAL(&lock);
}

static bool cursor_number(const char *text, uint32_t *value)
{
    if (*text == '\0') {
        return false;
    }
    uint32_t result = 0;
    while (*text != '\0') {
        unsigned digit = (unsigned)(*text++ - '0');
        if (digit > 9 || result > (UINT32_MAX - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

bool roundie_gui_command(const char *operation, const char *id, int parsed)
{
    bool perf = strcmp(operation, "PERF") == 0;
    bool cursor = strncmp(operation, "PERF ", 5) == 0;
    bool tuning = strncmp(operation, "BATCH ", 6) == 0;
    if (!perf && !cursor && !tuning) {
        return false;
    }
    esp_err_t result = parsed;
    uint32_t requested = 0;
    if (result == ESP_OK && (cursor || tuning) &&
        !cursor_number(operation + (cursor ? 5 : 6), &requested)) {
        result = ESP_ERR_INVALID_ARG;
    }
    if (result == ESP_OK && tuning) {
        if (requested != ROUNDIE_UI_BATCH && requested != ROUNDIE_UI_QUEUE_CAPACITY) {
            result = ESP_ERR_INVALID_ARG;
        } else if (roundie_elf_busy()) {
            result = ESP_ERR_INVALID_STATE;
        } else {
            portENTER_CRITICAL(&lock);
            batch = requested;
            portEXIT_CRITICAL(&lock);
        }
    }
    /* One worker owns reply scratch. Serialize outside the lock. */
    static uint64_t trace[TRACE_FIELDS];
    static char response[1400];
    uint32_t oldest;
    uint32_t latest;
    uint64_t summary[14];
    portENTER_CRITICAL(&lock);
    oldest = sequence > TRACE_COUNT ? sequence - TRACE_COUNT + 1 : (sequence != 0 ? 1 : 0);
    latest = sequence;
    uint32_t wanted = requested < oldest ? oldest : requested != UINT32_MAX ? requested + 1 : 0;
    memset(trace, 0, sizeof(trace));
    if (cursor && wanted != 0 && wanted <= latest) {
        memcpy(trace, traces[(wanted - 1) % TRACE_COUNT], sizeof(trace));
    }
    const uint64_t values[14] = {batch, loops, flushes, pixels, errors_seen,
        handler_max, pump_max, wake_max, dma_max, submit_max, wait_max, stack_min, sequence, dma_completions};
    memcpy(summary, values, sizeof(values));
    portEXIT_CRITICAL(&lock);
    int length = snprintf(response, sizeof(response), "\n!RP1 %s %d {\"format\":1,\"oldest\":%" PRIu32
        ",\"latest\":%" PRIu32 ",\"summary\":[", id, (int)result, oldest, latest);
    for (unsigned i = 0; i < 14 + TRACE_FIELDS; i++) {
        if (length < 0 || (size_t)length >= sizeof(response)) {
            return true;
        }
        uint64_t value = i < 14 ? summary[i] : trace[i - 14];
        int added = snprintf(response + length, sizeof(response) - (size_t)length,
            "%s%" PRIu64, i == 14 ? "],\"trace\":[" : (i == 0 ? "" : ","), value);
        if (added < 0) {
            return true;
        }
        length += added;
    }
    if (length > 0 && (size_t)length + 4 < sizeof(response)) {
        memcpy(response + length, "]}\n", 4);
        usb_serial_jtag_write_bytes(response, (size_t)length + 3, pdMS_TO_TICKS(100));
    }
    return true;
}
