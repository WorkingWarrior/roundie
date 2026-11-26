"""Compile the production LCD ISR callback with notification/scheduler mocks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DisplayCompletionTests(unittest.TestCase):
    def test_internal_buffer_allocation_failure(self):
        source = (ROOT / 'main/main.c').read_text()
        # The direct (unscaled) path; the scaled one has its own test.
        allocation = source[source.index('    const size_t buffer_bytes = DISPLAY_WIDTH'):
                            source.index('#endif\n\n    state.display = lv_display_create(')]
        harness = r'''
#include <assert.h>
#include <stddef.h>
#include <setjmp.h>
#define DISPLAY_WIDTH 360
#define DISPLAY_STRIP_LINES 20
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static int calls, frees, fail_at;
static char memory[2];
static jmp_buf failed;
static void *heap_caps_malloc(size_t bytes, int caps)
{
    assert(bytes == 14400 && caps == 3);
    calls++;
    return calls == fail_at ? NULL : &memory[calls - 1];
}
static void heap_caps_free(void *p) { assert(p == &memory[0]); frees++; }
static _Noreturn void headless_recovery(void) { longjmp(failed, 1); }
static void allocate(void)
{
'''
        harness += allocation + r'''
    assert(buffer_count == (CONFIG_ROUNDIE_LCD_DOUBLE_BUFFER ? 2U : 1U));
    assert(draw_buffer == &memory[0]);
    assert(draw_buffer_second == (CONFIG_ROUNDIE_LCD_DOUBLE_BUFFER ? &memory[1] : NULL));
}
int main(void)
{
    for (fail_at = 0; fail_at <= 2; fail_at++) {
        calls = frees = 0;
        if (setjmp(failed) == 0) {
            allocate();
            assert(fail_at == 0 || (!CONFIG_ROUNDIE_LCD_DOUBLE_BUFFER && fail_at == 2));
        } else {
            assert(calls == fail_at);
            assert(frees == (fail_at == 2));
        }
    }
    (void)heap_caps_free;
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='roundie-draw-allocation-') as temporary:
            c = Path(temporary) / 'allocation.c'
            exe = Path(temporary) / 'allocation'
            c.write_text(harness)
            for double in (0, 1):
                subprocess.run(['cc', '-std=c23', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=address,undefined', '-g',
                                f'-DCONFIG_ROUNDIE_LCD_DOUBLE_BUFFER={double}', str(c), '-o', str(exe)],
                               check=True, capture_output=True)
                subprocess.run([str(exe)], check=True, timeout=5)

    def test_buffer_wait_timeout_and_partial_submission(self):
        source = (ROOT / 'main/main.c').read_text()
        callbacks = source[source.index('static void wait_for_display('):
                           source.index('#else\nstatic const roundie_scale_geometry_t')]
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
    uint64_t transfer_id, iteration_id, transfer_iteration_id;
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
enum { ROUNDIE_RESTART_SITE_LCD_TIMEOUT = 1, ROUNDIE_RESTART_SITE_LCD_FLUSH };
static void roundie_boot_diag_restart_site(int site, int64_t now_us) { (void)site; (void)now_us; }
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
#define ROUNDIE_BOARD_DISPLAY_SWAP_RGB565 1
static const char *esp_err_to_name(int error) { (void)error; return "failed"; }
static int roundie_board_display_draw(void *board, int x1, int y1, int x2, int y2, void *p)
{
    assert(board == state.board && p != 0 && state.transfer_pending);
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

    def test_scaled_chunks_never_reuse_a_buffer_in_flight(self):
        source = (ROOT / 'main/main.c').read_text()
        scaled = source[source.index('static const roundie_scale_geometry_t scale_geometry'):
                        source.index('#endif\n\nstatic void display_event(')]
        harness = r"""
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "display_scale.h"
#include "gui_diagnostics.h"
#define CANVAS_WIDTH 360
#define CANVAS_HEIGHT 360
#define DISPLAY_WIDTH 466
#define DISPLAY_HEIGHT 466
#define DISPLAY_STRIP_LINES 24
#define ROUNDIE_BOARD_DISPLAY_ALIGN 2
#define RENDER_ROWS 17
typedef int BaseType_t;
typedef int esp_err_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_OK 0
#define TAG "test"
#define ESP_LOGE(...) ((void)esp_err_to_name(0))
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define portENTER_CRITICAL_ISR(lock) ((void)(lock))
#define portEXIT_CRITICAL_ISR(lock) ((void)(lock))
#define portYIELD_FROM_ISR(woken) ((void)(woken))
typedef struct { int32_t x1, y1, x2, y2; } lv_area_t;
typedef struct {
    int dma_lock;
    void *gui_task;
    void *board;
    int64_t dma_done_us;
    uint32_t dma_completions;
    uint64_t transfer_id;
    bool display_fault;
    uint32_t completed_transfers, transfer_errors;
    roundie_gui_work_t work;
    roundie_scale_carry_t carry;
    uint16_t *scale_output[2];
    uint16_t *scale_carry_pixels;
    uint32_t collected_completions;
    uint64_t collected_busy_us;
    int64_t chunk_submitted_us[2];
    uint64_t dma_busy_us;
    uint32_t dma_busy_max_us;
} gui_state_t;
typedef gui_state_t lv_display_t;
static gui_state_t state;
static int64_t now;
static unsigned notifications, releases, timeouts;
static bool last_part, fail_draw, stall, early;
static uint32_t random_state = 99;
static uint32_t next_random(void)
{
    random_state = random_state * 1664525U + 1013904223U;
    return random_state >> 8;
}
static jmp_buf restart;
/* Queued draws in order; the "DMA" reads a buffer only when it completes. */
typedef struct { int x1, y1, x2, y2; const uint16_t *pixels; } draw_t;
static draw_t queue[4];
static unsigned queued;
static uint16_t canvas[CANVAS_HEIGHT][CANVAS_WIDTH];
static uint16_t panel[DISPLAY_HEIGHT][DISPLAY_WIDTH];
static int64_t esp_timer_get_time(void) { now += 10; return now; }
static const char *esp_err_to_name(int error) { (void)error; return "failed"; }
static _Noreturn void esp_restart(void) { longjmp(restart, 1); }
enum { ROUNDIE_RESTART_SITE_LCD_TIMEOUT = 1, ROUNDIE_RESTART_SITE_LCD_FLUSH };
static void roundie_boot_diag_restart_site(int site, int64_t now_us) { (void)site; (void)now_us; }
static gui_state_t *lv_display_get_user_data(lv_display_t *d) { return d; }
static void lv_display_flush_ready(lv_display_t *d) { assert(d == &state); releases++; }
static bool lv_display_flush_is_last(lv_display_t *d) { assert(d == &state); return last_part; }
static uint32_t lv_area_get_size(const lv_area_t *a)
{
    return (uint32_t)((a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1));
}
static void vTaskNotifyGiveFromISR(void *task, BaseType_t *woken)
{
    assert(task == state.gui_task);
    notifications++;
    *woken = pdFALSE;
}
static bool lcd_chunk_done(void *user_context);
static void complete_oldest(void)
{
    assert(queued > 0);
    draw_t d = queue[0];
    for (int y = d.y1; y < d.y2; y++) {
        memcpy(&panel[y][d.x1], d.pixels + (size_t)(y - d.y1) * (size_t)(d.x2 - d.x1),
               (size_t)(d.x2 - d.x1) * 2U);
    }
    memmove(queue, queue + 1, sizeof(queue[0]) * (queued - 1U));
    queued--;
    now += 1000;
    lcd_chunk_done(&state);
}
/* The GUI task blocks: the oldest queued transfer finishes meanwhile. */
static uint32_t ulTaskNotifyTake(int clear, unsigned timeout)
{
    assert(clear == pdTRUE && timeout == 2000);
    if (!stall && queued > 0) {
        complete_oldest();
    }
    if (notifications == 0) {
        timeouts++;
        return 0;
    }
    uint32_t taken = notifications;
    notifications = 0;
    return taken;
}
static esp_err_t roundie_board_display_draw(void *board, int x1, int y1, int x2, int y2,
                                            const void *pixels)
{
    assert(board == state.board);
    if (fail_draw) {
        return -1;
    }
    /* Two queued transfers at most, never sharing a buffer, aligned windows. */
    assert(queued < 2);
    for (unsigned i = 0; i < queued; i++) {
        assert(queue[i].pixels != pixels);
    }
    assert(x1 % 2 == 0 && x2 % 2 == 0 && y1 % 2 == 0 && y2 % 2 == 0);
    assert((size_t)(x2 - x1) * (size_t)(y2 - y1) <= (size_t)DISPLAY_WIDTH * DISPLAY_STRIP_LINES);
    queue[queued++] = (draw_t){x1, y1, x2, y2, pixels};
    return ESP_OK;
}
"""
        harness += scaled
        harness += r"""
static uint16_t strip[CANVAS_WIDTH * RENDER_ROWS];
/* One rounded area, strip by strip as LVGL PARTIAL mode renders it. */
static void refresh(int32_t x1, int32_t y1, int32_t x2, int32_t y2, bool last_area)
{
    roundie_scale_round(&scale_geometry, &x1, &y1, &x2, &y2);
    int32_t width = x2 - x1 + 1;
    int32_t rows = (CANVAS_WIDTH * RENDER_ROWS) / width;
    for (int32_t row = y1; row <= y2; row += rows) {
        int32_t last = row + rows - 1 < y2 ? row + rows - 1 : y2;
        for (int32_t y = row; y <= last; y++) {
            memcpy(strip + (size_t)(y - row) * (size_t)width, &canvas[y][x1], (size_t)width * 2U);
        }
        lv_area_t area = {x1, row, x2, last};
        last_part = last_area && last == y2;
        unsigned before = releases;
        flush_scaled(&state, &area, (uint8_t *)strip);
        /* LVGL may reuse its buffer at once except after the last strip; after
         * a display fault every strip is released unsent. */
        assert(releases == before + (last_part && !state.display_fault ? 0U : 1U));
        /* The strip buffer is free: scribbling over it changes nothing. */
        memset(strip, 0xA5, sizeof(strip));
        /* DMA may finish while LVGL renders the next strip. */
        if (early && queued > 0 && (next_random() & 1U) != 0) {
            complete_oldest();
        }
    }
}
static void check_panel(void)
{
    for (uint32_t y = 0; y < DISPLAY_HEIGHT; y++) {
        uint32_t sy = roundie_scale_source(y, CANVAS_HEIGHT, DISPLAY_HEIGHT);
        for (uint32_t x = 0; x < DISPLAY_WIDTH; x++) {
            uint32_t sx = roundie_scale_source(x, CANVAS_WIDTH, DISPLAY_WIDTH);
            uint16_t expected = canvas[sy][sx];
            if (ROUNDIE_BOARD_DISPLAY_SWAP_RGB565 != 0) {
                expected = (uint16_t)((expected >> 8) | (expected << 8));
            }
            assert(panel[y][x] == expected);
        }
    }
}
int main(void)
{
    static uint16_t output[2][DISPLAY_WIDTH * DISPLAY_STRIP_LINES];
    static uint16_t carry[DISPLAY_WIDTH];
    state.gui_task = &state;
    state.board = &state;
    state.scale_output[0] = output[0];
    state.scale_output[1] = output[1];
    state.scale_carry_pixels = carry;
    roundie_scale_columns(&scale_geometry, scale_columns);
    assert(roundie_scale_max_output(&scale_geometry, CANVAS_WIDTH * RENDER_ROWS) <=
           DISPLAY_WIDTH * DISPLAY_STRIP_LINES);
    uint32_t seed = 7;
    for (int y = 0; y < CANVAS_HEIGHT; y++) {
        for (int x = 0; x < CANVAS_WIDTH; x++) {
            seed = seed * 1664525U + 1013904223U;
            canvas[y][x] = (uint16_t)(seed >> 16);
        }
    }
    /* Full frame, then two areas in one refresh, one of them narrow. */
    refresh(0, 0, CANVAS_WIDTH - 1, CANVAS_HEIGHT - 1, true);
    wait_for_scaled_display(&state);
    assert(queued == 0 && state.dma_completions == (uint32_t)state.transfer_id);
    check_panel();
    for (int y = 40; y < 300; y++) {
        canvas[y][101] ^= 0x1234;
        canvas[y][200] ^= 0x4321;
    }
    for (int x = 10; x < 350; x++) {
        canvas[180][x] ^= 0x0F0F;
    }
    refresh(101, 40, 101, 299, false);
    refresh(10, 180, 349, 180, false);
    refresh(200, 40, 200, 299, true);
    wait_for_scaled_display(&state);
    check_panel();
    /* Counters: every chunk queued, finished and timed. */
    assert(state.completed_transfers == state.transfer_id && state.work.chunks == state.transfer_id);
    assert(state.work.done == state.transfer_id);
    /* Each simulated transfer is on the bus for at least 1000 us. */
    assert(state.work.dma_us >= 1000U * state.transfer_id && state.work.dma_us <= (uint64_t)now);
    assert(state.work.dma_max_us >= 1000U && state.work.scale_us > 0);
    assert(state.transfer_errors == 0 && timeouts == 0);
    /* Many frames of random dirty areas, one to four per refresh, some one
     * pixel wide or high, with DMA finishing at random points: the panel
     * always equals the reference scaling of the whole canvas. */
    early = true;
    for (int frame = 0; frame < 300; frame++) {
        int areas = 1 + (int)(next_random() % 4U);
        for (int area = 0; area < areas; area++) {
            int32_t ax1 = (int32_t)(next_random() % CANVAS_WIDTH);
            int32_t ay1 = (int32_t)(next_random() % CANVAS_HEIGHT);
            int32_t ax2 = ax1 + (int32_t)(next_random() % (frame % 3 == 0 ? 2U : 120U));
            int32_t ay2 = ay1 + (int32_t)(next_random() % (frame % 5 == 0 ? 2U : 120U));
            ax2 = ax2 < CANVAS_WIDTH ? ax2 : CANVAS_WIDTH - 1;
            ay2 = ay2 < CANVAS_HEIGHT ? ay2 : CANVAS_HEIGHT - 1;
            for (int32_t y = ay1; y <= ay2; y++) {
                for (int32_t x = ax1; x <= ax2; x++) {
                    canvas[y][x] = (uint16_t)next_random();
                }
            }
            refresh(ax1, ay1, ax2, ay2, area == areas - 1);
        }
        wait_for_scaled_display(&state);
        check_panel();
    }
    early = false;
    assert(state.completed_transfers == state.transfer_id);
    assert(state.transfer_errors == 0 && timeouts == 0);
    /* A transfer that never finishes restarts instead of reusing its buffer. */
    stall = true;
    if (setjmp(restart) == 0) {
        refresh(0, 0, CANVAS_WIDTH - 1, CANVAS_HEIGHT - 1, true);
        assert(false);
    }
    assert(state.transfer_errors == 1 && timeouts == 1 && queued == 2);
    stall = false;
    while (queued > 0) {
        complete_oldest();
    }
    /* A misaligned strip is refused and released; a failed draw latches the fault. */
    lv_area_t odd = {1, 0, 358, 9};
    last_part = true;
    flush_scaled(&state, &odd, (uint8_t *)strip);
    assert(state.transfer_errors == 2 && !state.carry.active && queued == 0);
    fail_draw = true;
    unsigned before = releases;
    refresh(0, 0, CANVAS_WIDTH - 1, 20, true);
    assert(state.display_fault && state.transfer_errors == 3 && releases > before);
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix='roundie-lcd-scaled-') as temporary:
            c = Path(temporary) / 'scaled.c'
            exe = Path(temporary) / 'scaled'
            c.write_text(harness)
            # Byte swap off and on (the 1.75C sends RGB565 MSB first).
            for standard, swap in (('c11', 0), ('c23', 1), ('c11', 1)):
                subprocess.run(['cc', '-std=' + standard, '-Wall', '-Wextra', '-Werror',
                                f'-DROUNDIE_BOARD_DISPLAY_SWAP_RGB565={swap}',
                                '-fsanitize=address,undefined', '-g', '-I' + str(ROOT / 'main'),
                                str(c), str(ROOT / 'main/display_scale.c'), '-o', str(exe)],
                               check=True, capture_output=True)
                subprocess.run([str(exe)], check=True, timeout=60)

    def test_completed_dma_notifies_and_requests_deferred_switch(self):
        source = (ROOT / 'main/main.c').read_text()
        callback = source[source.index('static bool lcd_transfer_done('):source.index('static void wait_for_display(')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
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
    assert(lcd_transfer_done(&state));
    assert(notifications == 1 && switches == 1);
    wake = false;
    assert(!lcd_transfer_done(&state));
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
