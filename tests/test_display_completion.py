"""Compile the production LCD ISR callback with notification/scheduler mocks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DisplayCompletionTests(unittest.TestCase):
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
