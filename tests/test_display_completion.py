"""Compile the production LCD ISR callback with notification/scheduler mocks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DisplayCompletionTests(unittest.TestCase):
    def test_buffer_wait_timeout_and_partial_submission(self):
        source = (ROOT / 'main/main.c').read_text()
        callbacks = source[source.index('static void wait_for_display('):
                           source.index('static void display_event(')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include "gui_diagnostics.h"
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_OK 0
#define ESP_LOGE(...) ((void)0)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
typedef int esp_err_t;
typedef struct { int x1, y1, x2, y2; } lv_area_t;
typedef struct {
    int dma_lock;
    int64_t dma_done_us, transfer_started_us;
    bool transfer_pending, display_fault;
    uint32_t completed_transfers, transfer_errors;
    roundie_gui_work_t work;
    void *board;
} gui_state_t;
typedef gui_state_t lv_display_t;
static gui_state_t state;
static int64_t now;
static unsigned notification, releases, submissions, swaps;
static bool fail_submission;
static jmp_buf restart;
static int64_t esp_timer_get_time(void) { now += 10; return now; }
static gui_state_t *lv_display_get_user_data(lv_display_t *d) { return d; }
static uint32_t ulTaskNotifyTake(int clear, unsigned timeout)
{
    assert(clear == pdTRUE && timeout == 2000);
    assert(state.transfer_pending);
    unsigned result = notification;
    notification = 0;
    return result;
}
static _Noreturn void esp_restart(void) { longjmp(restart, 1); }
static unsigned lv_area_get_size(const lv_area_t *a)
{
    return (unsigned)((a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1));
}
static void lv_display_flush_ready(lv_display_t *d)
{
    assert(d == &state && !state.transfer_pending);
    releases++;
}
static void lv_draw_sw_rgb565_swap(void *p, unsigned count)
{
    assert(p != 0 && count == 72 && state.transfer_pending);
    swaps++;
}
static void *roundie_board_panel(void *board) { return board; }
static const char *esp_err_to_name(int error) { (void)error; return "failed"; }
static int esp_lcd_panel_draw_bitmap(void *panel, int x1, int y1, int x2, int y2, void *p)
{
    assert(panel == state.board && p != 0 && state.transfer_pending);
    assert(x1 == 11 && y1 == 21 && x2 == 19 && y2 == 30);
    submissions++;
    return fail_submission ? -1 : 0;
}
'''
        # Keep the error-name call compiled even when logging is mocked away.
        harness = harness.replace('#define ESP_LOGE(...) ((void)0)',
                                  '#define ESP_LOGE(...) ((void)esp_err_to_name(0))')
        harness += callbacks
        harness += r'''
int main(void)
{
    uint8_t pixels[144] = {0};
    lv_area_t area = {11, 21, 18, 29};
    state.board = &state;
    flush_display(&state, &area, pixels);
    assert(submissions == 1 && swaps == 1 && releases == 0);
    assert(state.transfer_pending && state.work.pixels == 72 && state.work.flushes == 1);
    /* No notification yet: do not clear pending or allow LVGL to reuse DMA. */
    if (setjmp(restart) == 0) {
        wait_for_display(&state);
        assert(false);
    }
    assert(state.transfer_pending && state.completed_transfers == 0 && releases == 0);
    assert(state.transfer_errors == 1);
    /* A second submission while DMA owns the buffer must restart, not swap. */
    if (setjmp(restart) == 0) {
        flush_display(&state, &area, pixels);
        assert(false);
    }
    assert(submissions == 1 && swaps == 1 && state.transfer_errors == 2);
    state.dma_done_us = now;
    notification = 1;
    wait_for_display(&state);
    assert(!state.transfer_pending && state.completed_transfers == 1);
    assert(state.work.done == 1 && notification == 0 && releases == 0);
    /* LVGL's custom wait path clears its own flushing flag after return. */
    fail_submission = true;
    flush_display(&state, &area, pixels);
    assert(state.display_fault && !state.transfer_pending && releases == 1);
    assert(state.transfer_errors == 3 && submissions == 2 && swaps == 2);
    flush_display(&state, &area, pixels);
    assert(releases == 2 && submissions == 2 && swaps == 2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='roundie-lcd-wait-') as temporary:
            c = Path(temporary) / 'wait.c'
            exe = Path(temporary) / 'wait'
            c.write_text(harness)
            subprocess.run(['cc', '-std=c23', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-g', '-I' + str(ROOT / 'main'),
                            str(c), '-o', str(exe)], check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, timeout=5)

    def test_completed_dma_notifies_and_requests_deferred_switch(self):
        source = (ROOT / 'main/main.c').read_text()
        callback = source[source.index('static bool lcd_transfer_done('):source.index('static void wait_for_display(')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef void *esp_lcd_panel_io_handle_t;
typedef void esp_lcd_panel_io_event_data_t;
typedef int BaseType_t;
#define pdFALSE 0
#define pdTRUE 1
typedef struct {
    int dma_lock;
    int64_t dma_done_us;
    uint32_t dma_completions;
    void *gui_task;
} gui_state_t;
static bool wake;
static unsigned notifications;
static unsigned switches;
static gui_state_t state;
static int64_t esp_timer_get_time(void) { return INT64_C(5000000000); }
#define portENTER_CRITICAL_ISR(lock) ((void)(lock))
#define portEXIT_CRITICAL_ISR(lock) ((void)(lock))
static void vTaskNotifyGiveFromISR(void *task, BaseType_t *woken)
{
    assert(task == state.gui_task);
    assert(state.dma_done_us == INT64_C(5000000000));
    assert(state.dma_completions == notifications + 1);
    notifications++;
    *woken = wake;
}
#define portYIELD_FROM_ISR(woken) do { switches += (woken) != 0; } while (0)
'''
        harness += callback
        harness += r'''
int main(void)
{
    state.gui_task = &state;
    wake = true;
    assert(lcd_transfer_done(0, 0, &state));
    assert(notifications == 1 && switches == 1);
    wake = false;
    assert(!lcd_transfer_done(0, 0, &state));
    assert(notifications == 2 && switches == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='roundie-lcd-isr-') as temporary:
            c = Path(temporary) / 'callback.c'
            exe = Path(temporary) / 'callback'
            c.write_text(harness)
            subprocess.run(['cc', '-std=c23', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-g', str(c), '-o', str(exe)], check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, timeout=5)
