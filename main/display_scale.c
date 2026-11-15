#include "display_scale.h"

#include <stddef.h>
#include <string.h>

uint32_t roundie_scale_source(uint32_t physical_index, uint32_t logical_size,
                              uint32_t physical_size)
{
    uint64_t numerator = (2U * (uint64_t)physical_index + 1U) * logical_size;
    return (uint32_t)(numerator / (2U * (uint64_t)physical_size));
}

uint32_t roundie_scale_first(uint32_t logical_index, uint32_t logical_size,
                             uint32_t physical_size)
{
    /* Smallest p with (2p + 1) * L >= 2 * P * l. */
    uint64_t target = 2U * (uint64_t)physical_size * logical_index;
    if (target <= logical_size) {
        return 0;
    }
    uint64_t numerator = target - logical_size;
    uint64_t denominator = 2U * (uint64_t)logical_size;
    return (uint32_t)((numerator + denominator - 1U) / denominator);
}

uint32_t roundie_scale_last(uint32_t logical_index, uint32_t logical_size,
                            uint32_t physical_size)
{
    if (logical_index + 1U >= logical_size) {
        return physical_size - 1U;
    }
    return roundie_scale_first(logical_index + 1U, logical_size, physical_size) - 1U;
}

static void round_axis(int32_t *first, int32_t *last, uint32_t logical_size,
                       uint32_t physical_size, uint32_t align)
{
    while (*first > 0 &&
           roundie_scale_first((uint32_t)*first, logical_size, physical_size) % align != 0) {
        (*first)--;
    }
    while (*last + 1 < (int32_t)logical_size &&
           (roundie_scale_last((uint32_t)*last, logical_size, physical_size) + 1U) % align != 0) {
        (*last)++;
    }
}

void roundie_scale_round(const roundie_scale_geometry_t *geometry,
                         int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2)
{
    uint32_t height = geometry->logical_height;
    round_axis(x1, x2, geometry->logical_width, geometry->physical_width, geometry->align);
    round_axis(y1, y2, height, geometry->physical_height, geometry->align);
    /* LVGL sizes PARTIAL strips by rounding the probe rows 0..h-1 with
     * h <= the area height and gives up (returns 0 rows) if none fits, so an
     * area must be at least as tall as the smallest aligned area at the top. */
    int32_t top_first = 0;
    int32_t top_last = 0;
    round_axis(&top_first, &top_last, height, geometry->physical_height, geometry->align);
    while (*y2 - *y1 < top_last) {
        if (*y2 + 1 < (int32_t)height) {
            (*y2)++;
        } else {
            (*y1)--;
        }
        round_axis(y1, y2, height, geometry->physical_height, geometry->align);
    }
}

bool roundie_scale_plan(const roundie_scale_geometry_t *geometry,
                        const roundie_scale_carry_t *carry,
                        int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                        roundie_scale_plan_t *plan)
{
    if (x1 < 0 || y1 < 0 || x2 < x1 || y2 < y1 ||
        x2 >= (int32_t)geometry->logical_width || y2 >= (int32_t)geometry->logical_height) {
        return false;
    }
    uint32_t align = geometry->align;
    plan->logical_x1 = x1;
    plan->logical_y1 = y1;
    plan->logical_width = x2 - x1 + 1;
    plan->x1 = (int32_t)roundie_scale_first((uint32_t)x1, geometry->logical_width,
                                            geometry->physical_width);
    plan->x2 = (int32_t)roundie_scale_last((uint32_t)x2, geometry->logical_width,
                                           geometry->physical_width);
    plan->y1 = (int32_t)roundie_scale_first((uint32_t)y1, geometry->logical_height,
                                            geometry->physical_height);
    plan->y2 = (int32_t)roundie_scale_last((uint32_t)y2, geometry->logical_height,
                                           geometry->physical_height);
    if ((uint32_t)plan->x1 % align != 0 || (uint32_t)(plan->x2 + 1) % align != 0) {
        return false;
    }
    if (carry->active) {
        if (carry->row != plan->y1 - 1 || carry->x1 != plan->x1 || carry->x2 != plan->x2) {
            return false;
        }
        plan->uses_carry = true;
        plan->send_y1 = carry->row;
    } else {
        if ((uint32_t)plan->y1 % align != 0) {
            return false;
        }
        plan->uses_carry = false;
        plan->send_y1 = plan->y1;
    }
    uint32_t rows = (uint32_t)(plan->y2 - plan->send_y1 + 1);
    plan->keeps_last = rows % align != 0;
    plan->send_y2 = plan->keeps_last ? plan->y2 - 1 : plan->y2;
    return true;
}

void roundie_scale_update_carry(const roundie_scale_plan_t *plan,
                                roundie_scale_carry_t *carry)
{
    carry->active = plan->keeps_last;
    carry->row = plan->y2;
    carry->x1 = plan->x1;
    carry->x2 = plan->x2;
}

/* Panel indices sampling count consecutive logical indices: the p with
 * a <= (2p + 1) * L / (2P) < a + count lie in a half-open interval of length
 * count * P / L, so there are at most ceil(count * P / L) of them. */
static uint32_t span_bound(uint32_t count, uint32_t logical_size, uint32_t physical_size)
{
    uint64_t product = (uint64_t)count * physical_size;
    return (uint32_t)((product + logical_size - 1U) / logical_size);
}

uint32_t roundie_scale_max_output(const roundie_scale_geometry_t *geometry,
                                  uint32_t buffer_pixels)
{
    uint32_t maximum = 0;
    for (uint32_t width = 1; width <= geometry->logical_width; width++) {
        uint32_t rows = buffer_pixels / width;
        if (rows > geometry->logical_height) {
            rows = geometry->logical_height;
        }
        if (rows == 0) {
            break;
        }
        uint32_t columns = span_bound(width, geometry->logical_width, geometry->physical_width);
        uint32_t panel_rows = span_bound(rows, geometry->logical_height,
                                         geometry->physical_height) + 1U;
        if (columns * panel_rows > maximum) {
            maximum = columns * panel_rows;
        }
    }
    return maximum;
}

void roundie_scale_columns(const roundie_scale_geometry_t *geometry,
                           uint16_t *columns)
{
    for (uint32_t p = 0; p < geometry->physical_width; p++) {
        columns[p] = (uint16_t)roundie_scale_source(p, geometry->logical_width,
                                                    geometry->physical_width);
    }
}

static void scale_row(uint16_t *output, const uint16_t *input, const uint16_t *columns,
                      uint32_t count, uint32_t input_x1, bool swap)
{
    if (swap) {
        for (uint32_t i = 0; i < count; i++) {
            uint16_t value = input[columns[i] - input_x1];
            output[i] = (uint16_t)((value >> 8) | (value << 8));
        }
    } else {
        for (uint32_t i = 0; i < count; i++) {
            output[i] = input[columns[i] - input_x1];
        }
    }
}

void roundie_scale_strip(const roundie_scale_geometry_t *geometry,
                         const roundie_scale_plan_t *plan,
                         const uint16_t *columns, const uint16_t *strip,
                         uint16_t *output, uint16_t *carry_pixels, bool swap)
{
    size_t width = (size_t)(plan->x2 - plan->x1 + 1);
    size_t row_bytes = width * sizeof(uint16_t);
    uint16_t *row_out = output;
    if (plan->uses_carry) {
        memcpy(row_out, carry_pixels, row_bytes);
        row_out += width;
    }
    const uint16_t *previous = NULL;
    uint32_t previous_source = UINT32_MAX;
    for (int32_t row = plan->y1; row <= plan->y2; row++) {
        bool held = plan->keeps_last && row == plan->y2;
        uint16_t *target = held ? carry_pixels : row_out;
        uint32_t source = roundie_scale_source((uint32_t)row, geometry->logical_height,
                                               geometry->physical_height);
        if (source == previous_source) {
            memcpy(target, previous, row_bytes);
        } else {
            const uint16_t *input = strip +
                (size_t)(source - (uint32_t)plan->logical_y1) * (size_t)plan->logical_width;
            scale_row(target, input, columns + plan->x1, (uint32_t)width,
                      (uint32_t)plan->logical_x1, swap);
        }
        previous = target;
        previous_source = source;
        if (!held) {
            row_out += width;
        }
    }
}
