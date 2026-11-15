#include "display_scale.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SIDE 512

static uint16_t logical_frame[MAX_SIDE * MAX_SIDE];
static uint16_t panel[MAX_SIDE * MAX_SIDE];
static uint16_t reference[MAX_SIDE * MAX_SIDE];
static uint16_t strip[MAX_SIDE * MAX_SIDE];
static uint16_t output[MAX_SIDE * MAX_SIDE];
static uint16_t carry_pixels[MAX_SIDE];
static uint16_t columns[MAX_SIDE];
static uint32_t random_state = 12345U;

static uint32_t next_random(void)
{
    random_state = random_state * 1664525U + 1013904223U;
    return random_state >> 8;
}

static uint16_t swap16(uint16_t value)
{
    return (uint16_t)((value >> 8) | (value << 8));
}

/* Brute force against the closed forms. */
static void check_mapping(uint32_t logical, uint32_t physical)
{
    for (uint32_t l = 0; l < logical; l++) {
        uint32_t first = UINT32_MAX;
        uint32_t last = 0;
        for (uint32_t p = 0; p < physical; p++) {
            if (roundie_scale_source(p, logical, physical) == l) {
                if (first == UINT32_MAX) {
                    first = p;
                }
                last = p;
            }
        }
        assert(first != UINT32_MAX);
        assert(roundie_scale_first(l, logical, physical) == first);
        assert(roundie_scale_last(l, logical, physical) == last);
    }
}

/* LVGL 9.5 get_max_row(): the buffer's rows at this area width, then shrink
 * until the rounded strip fits. */
static int32_t lvgl_max_row(const roundie_scale_geometry_t *geometry, int32_t buffer_pixels,
                            int32_t area_width, int32_t area_height)
{
    int32_t buffer_rows = buffer_pixels / area_width;
    if (buffer_rows > area_height) {
        buffer_rows = area_height;
    }
    int32_t height = buffer_rows;
    while (height > 0) {
        int32_t x1 = 0;
        int32_t y1 = 0;
        int32_t x2 = 0;
        int32_t y2 = height - 1;
        roundie_scale_round(geometry, &x1, &y1, &x2, &y2);
        if (y2 - y1 + 1 <= buffer_rows) {
            return y2 + 1;
        }
        height--;
    }
    return 0;
}

/* Renders one area strip by strip as LVGL PARTIAL mode does and applies the
 * plans as main.c does. Returns the largest transfer in pixels. */
static uint32_t refresh_area(const roundie_scale_geometry_t *geometry, int32_t x1, int32_t y1,
                             int32_t x2, int32_t y2, int32_t buffer_pixels, bool swap)
{
    roundie_scale_carry_t carry = {0};
    uint32_t largest = 0;
    int32_t width = x2 - x1 + 1;
    int32_t max_row = lvgl_max_row(geometry, buffer_pixels, width, y2 - y1 + 1);
    assert(max_row > 0);
    assert(max_row * width <= buffer_pixels);
    int32_t next_send = -1;
    for (int32_t row = y1; row <= y2; row += max_row) {
        int32_t last = row + max_row - 1 <= y2 ? row + max_row - 1 : y2;
        for (int32_t y = row; y <= last; y++) {
            for (int32_t x = x1; x <= x2; x++) {
                strip[(size_t)(y - row) * (size_t)width + (size_t)(x - x1)] =
                    logical_frame[(size_t)y * geometry->logical_width + (size_t)x];
            }
        }
        roundie_scale_plan_t plan;
        assert(roundie_scale_plan(geometry, &carry, x1, row, x2, last, &plan));
        roundie_scale_strip(geometry, &plan, columns, strip, output, carry_pixels, swap);
        roundie_scale_update_carry(&plan, &carry);
        if (plan.send_y2 < plan.send_y1) {
            continue;
        }
        /* Aligned window, contiguous with the previous transfer, never twice. */
        assert(plan.x1 % (int32_t)geometry->align == 0);
        assert((plan.x2 + 1) % (int32_t)geometry->align == 0);
        assert(plan.send_y1 % (int32_t)geometry->align == 0);
        assert((plan.send_y2 + 1) % (int32_t)geometry->align == 0);
        assert(next_send == -1 || plan.send_y1 == next_send);
        next_send = plan.send_y2 + 1;
        uint32_t rows = (uint32_t)(plan.send_y2 - plan.send_y1 + 1);
        int32_t panel_width = plan.x2 - plan.x1 + 1;
        if (rows * (uint32_t)panel_width > largest) {
            largest = rows * (uint32_t)panel_width;
        }
        for (int32_t y = plan.send_y1; y <= plan.send_y2; y++) {
            for (int32_t x = plan.x1; x <= plan.x2; x++) {
                uint16_t value = output[(size_t)(y - plan.send_y1) * (size_t)panel_width +
                                        (size_t)(x - plan.x1)];
                panel[(size_t)y * geometry->physical_width + (size_t)x] = swap ? swap16(value) : value;
            }
        }
    }
    assert(!carry.active);
    assert(next_send == (int32_t)roundie_scale_last((uint32_t)y2, geometry->logical_height,
                                                    geometry->physical_height) + 1);
    return largest;
}

static void build_reference(const roundie_scale_geometry_t *geometry)
{
    for (uint32_t y = 0; y < geometry->physical_height; y++) {
        uint32_t source_y = roundie_scale_source(y, geometry->logical_height, geometry->physical_height);
        for (uint32_t x = 0; x < geometry->physical_width; x++) {
            uint32_t source_x = roundie_scale_source(x, geometry->logical_width, geometry->physical_width);
            reference[(size_t)y * geometry->physical_width + x] =
                logical_frame[(size_t)source_y * geometry->logical_width + source_x];
        }
    }
}

static void check_geometry(const roundie_scale_geometry_t *geometry, int32_t buffer_rows)
{
    check_mapping(geometry->logical_width, geometry->physical_width);
    check_mapping(geometry->logical_height, geometry->physical_height);
    roundie_scale_columns(geometry, columns);
    int32_t buffer_pixels = buffer_rows * (int32_t)geometry->logical_width;
    uint32_t limit = roundie_scale_max_output(geometry, (uint32_t)buffer_pixels);

    /* Full canvas: the edges need no widening. */
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = (int32_t)geometry->logical_width - 1;
    int32_t y2 = (int32_t)geometry->logical_height - 1;
    roundie_scale_round(geometry, &x1, &y1, &x2, &y2);
    assert(x1 == 0 && y1 == 0);
    assert(x2 == (int32_t)geometry->logical_width - 1 && y2 == (int32_t)geometry->logical_height - 1);

    for (size_t i = 0; i < (size_t)geometry->logical_width * geometry->logical_height; i++) {
        logical_frame[i] = (uint16_t)next_random();
    }
    build_reference(geometry);
    memset(panel, 0, sizeof(panel));
    assert(refresh_area(geometry, x1, y1, x2, y2, buffer_pixels, false) <= limit);
    assert(memcmp(panel, reference, (size_t)geometry->physical_width * geometry->physical_height * 2U) == 0);

    /* Random dirty areas update exactly their rectangles; every fourth one is
     * narrow, so LVGL gives it tall strips, and every third one is low. */
    for (int round = 0; round < 400; round++) {
        int32_t a = (int32_t)(next_random() % geometry->logical_width);
        int32_t b = (int32_t)(next_random() % geometry->logical_width);
        if (round % 4 == 0) {
            b = a + (int32_t)(next_random() % 4U);
            if (b >= (int32_t)geometry->logical_width) {
                b = (int32_t)geometry->logical_width - 1;
            }
        }
        int32_t c = (int32_t)(next_random() % geometry->logical_height);
        int32_t d = (int32_t)(next_random() % geometry->logical_height);
        if (round % 3 == 0) {
            d = c;
        }
        int32_t ax1 = a < b ? a : b;
        int32_t ax2 = a < b ? b : a;
        int32_t ay1 = c < d ? c : d;
        int32_t ay2 = c < d ? d : c;
        int32_t rx1 = ax1;
        int32_t ry1 = ay1;
        int32_t rx2 = ax2;
        int32_t ry2 = ay2;
        roundie_scale_round(geometry, &rx1, &ry1, &rx2, &ry2);
        assert(rx1 <= ax1 && ry1 <= ay1 && rx2 >= ax2 && ry2 >= ay2);
        assert(rx1 >= 0 && ry1 >= 0);
        assert(rx2 < (int32_t)geometry->logical_width && ry2 < (int32_t)geometry->logical_height);
        /* Rounding a rounded area changes nothing (LVGL joins and re-rounds). */
        int32_t sx1 = rx1;
        int32_t sy1 = ry1;
        int32_t sx2 = rx2;
        int32_t sy2 = ry2;
        roundie_scale_round(geometry, &sx1, &sy1, &sx2, &sy2);
        assert(sx1 == rx1 && sy1 == ry1 && sx2 == rx2 && sy2 == ry2);
        for (int32_t y = ay1; y <= ay2; y++) {
            for (int32_t x = ax1; x <= ax2; x++) {
                logical_frame[(size_t)y * geometry->logical_width + (size_t)x] = (uint16_t)next_random();
            }
        }
        build_reference(geometry);
        bool swap = (round & 1) != 0;
        assert(refresh_area(geometry, rx1, ry1, rx2, ry2, buffer_pixels, swap) <= limit);
        assert(memcmp(panel, reference, (size_t)geometry->physical_width * geometry->physical_height * 2U) == 0);
    }
}

int main(void)
{
    const roundie_scale_geometry_t amoled = {360, 360, 466, 466, 2};
    check_geometry(&amoled, 18);
    check_geometry(&amoled, 7);
    check_geometry(&amoled, 3); /* smallest aligned strip: canvas rows 0..2 */
    const roundie_scale_geometry_t unaligned = {360, 360, 466, 466, 1};
    check_geometry(&unaligned, 20);
    const roundie_scale_geometry_t identity = {360, 360, 360, 360, 1};
    check_geometry(&identity, 20);
    const roundie_scale_geometry_t uneven = {120, 90, 302, 158, 2};
    check_geometry(&uneven, 5);

    /* A strip that does not continue the carried row is refused. */
    roundie_scale_carry_t carry = {.active = true, .row = 10, .x1 = 0, .x2 = 465};
    roundie_scale_plan_t plan;
    assert(!roundie_scale_plan(&amoled, &carry, 0, 100, 359, 110, &plan));
    carry.active = false;
    assert(!roundie_scale_plan(&amoled, &carry, 0, 0, 360, 10, &plan));
    /* Unrounded edges are refused rather than sent misaligned. */
    assert(!roundie_scale_plan(&amoled, &carry, 1, 0, 359, 10, &plan));

    /* Transfer limit chosen by main.c for the 1.75C (24 panel rows). */
    assert(roundie_scale_max_output(&amoled, 360U * 17U) <= 466U * 24U);
    printf("display scale: OK, 17 canvas rows -> at most %u panel pixels per transfer\n",
           roundie_scale_max_output(&amoled, 360U * 17U));
    return 0;
}
