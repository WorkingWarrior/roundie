/* Real pinned LVGL: pixel correctness and partial-buffer reuse ordering.
 * Transfer completion is mocked; these are not hardware timing tests. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"

static _Alignas(4) uint8_t buffers[2][360 * 40 * 2];
static const uint8_t *pending;
static uint32_t pending_bytes;
static uint32_t pending_hash;
static unsigned flushes;
static unsigned completions;
static unsigned rendered_while_pending;
static uint32_t pixels_seen;
static lv_area_t bounds;

static uint32_t hash(const uint8_t *bytes, uint32_t size)
{
    uint32_t value = 2166136261U;
    for (uint32_t i = 0; i < size; i++) {
        value = (value ^ bytes[i]) * 16777619U;
    }
    return value;
}

static void wait_transfer(lv_display_t *display)
{
    (void)display;
    assert(pending != NULL);
    assert(hash(pending, pending_bytes) == pending_hash);
    pending = NULL;
    completions++;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *bytes)
{
    (void)display;
    assert(pending == NULL);
    assert(area->x1 >= bounds.x1 && area->x2 <= bounds.x2);
    assert(area->y1 >= bounds.y1 && area->y2 <= bounds.y2);
    uint32_t pixels = (uint32_t)lv_area_get_size(area);
    assert(pixels > 0 && pixels * 2 <= sizeof(buffers[0]));
    lv_draw_sw_rgb565_swap(bytes, pixels);
    for (uint32_t i = 0; i < pixels; i++) {
        assert(bytes[i * 2] == 0xf8 && bytes[i * 2 + 1] == 0x00);
    }
    pending = bytes;
    pending_bytes = pixels * 2;
    pending_hash = hash(bytes, pending_bytes);
    pixels_seen += pixels;
    flushes++;
}

static void drawing(lv_event_t *event)
{
    (void)event;
    if (pending != NULL) {
        assert(hash(pending, pending_bytes) == pending_hash);
        rendered_while_pending++;
    }
}

static void swap_colors(void)
{
    static const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff, 0, 0x1234};
    _Alignas(4) uint8_t bytes[160];
    for (unsigned offset = 0; offset <= 2; offset += 2) {
        for (unsigned count = 1; count <= 65; count++) {
            memset(bytes, 0xa5, sizeof(bytes));
            for (unsigned i = 0; i < count; i++) {
                uint16_t color = colors[i % 6];
                memcpy(bytes + offset + i * 2, &color, 2);
            }
            lv_draw_sw_rgb565_swap(bytes + offset, count);
            for (unsigned i = 0; i < count; i++) {
                uint16_t color = colors[i % 6];
                assert(bytes[offset + i * 2] == color >> 8);
                assert(bytes[offset + i * 2 + 1] == (color & 255));
            }
            assert(bytes[offset + count * 2] == 0xa5);
            if (offset != 0) {
                assert(bytes[0] == 0xa5 && bytes[1] == 0xa5);
            }
        }
    }
}

static void configuration(unsigned count, unsigned lines)
{
    flushes = completions = rendered_while_pending = pixels_seen = 0;
    pending = NULL;
    memset(buffers, 0, sizeof(buffers));
    lv_display_t *display = lv_display_create(360, 360);
    assert(display != NULL);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffers[0], count == 2 ? buffers[1] : NULL,
                           360 * lines * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_flush_wait_cb(display, wait_transfer);
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xff0000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(screen, drawing, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    bounds = (lv_area_t){0, 0, 359, 359};
    lv_refr_now(display);
    assert(pixels_seen == 129600 && flushes == 360 / lines);
    assert(completions == flushes - 1 && pending != NULL);
    assert((rendered_while_pending > 0) == (count == 2));
    /* A later refresh must consume the previous refresh's final DMA before
     * reuse. An odd-width dirty rectangle includes an incomplete last strip. */
    unsigned previous_flushes = flushes;
    uint32_t previous_pixels = pixels_seen;
    bounds = (lv_area_t){11, 21, 17, 99};
    lv_obj_invalidate_area(screen, &bounds);
    lv_refr_now(display);
    assert(pixels_seen - previous_pixels == 7 * 79);
    assert(flushes > previous_flushes);
    assert(completions == flushes - 1 && pending != NULL);
    wait_transfer(display);
    assert(completions == flushes);
    lv_display_flush_ready(display);
    lv_display_delete(display);
}

int main(void)
{
    lv_init();
    swap_colors();
    configuration(1, 20);
    configuration(1, 40);
    configuration(2, 20);
    puts("Pinned LVGL RGB565/partial buffering: PASS (mock DMA completion)");
    return 0;
}
