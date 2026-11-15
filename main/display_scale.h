#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Nearest-neighbour upscaling of the logical UI canvas to a larger panel
 * (docs/display-rendering.md). Each panel pixel samples the canvas pixel under
 * its centre, so every canvas area maps to one disjoint panel rectangle: a
 * strip rendered by LVGL is scaled once, and no panel pixel needs a neighbour
 * outside the strip. Hardware independent; GUI task only on the device. */
typedef struct {
    uint32_t logical_width;
    uint32_t logical_height;
    uint32_t physical_width;
    uint32_t physical_height;
    uint32_t align; /* 1 or 2: panel windows start and end on this grid */
} roundie_scale_geometry_t;

/* One scaled panel row held back because its partner row of an aligned pair
 * comes from the next strip of the same area. */
typedef struct {
    bool active;
    int32_t row;
    int32_t x1;
    int32_t x2;
} roundie_scale_carry_t;

/* Panel work for one logical strip. Coordinates are inclusive. */
typedef struct {
    int32_t logical_x1;
    int32_t logical_y1;
    int32_t logical_width;
    int32_t x1; /* panel columns */
    int32_t x2;
    int32_t y1; /* panel rows produced from this strip */
    int32_t y2;
    int32_t send_y1; /* panel rows to transfer now; none when send_y2 < send_y1 */
    int32_t send_y2;
    bool uses_carry; /* send_y1 is the carried row */
    bool keeps_last; /* row y2 becomes the next carry */
} roundie_scale_plan_t;

/* Logical index sampled by panel index. */
uint32_t roundie_scale_source(uint32_t physical_index, uint32_t logical_size,
                              uint32_t physical_size);
/* First and last panel index that samples logical index. */
uint32_t roundie_scale_first(uint32_t logical_index, uint32_t logical_size,
                             uint32_t physical_size);
uint32_t roundie_scale_last(uint32_t logical_index, uint32_t logical_size,
                            uint32_t physical_size);

/* LVGL rounder: widens a logical area until its panel rectangle starts and
 * ends on the alignment grid and the area is at least as tall as the smallest
 * such area at the top of the canvas (LVGL's strip-height probe). The canvas
 * edges are always aligned. */
void roundie_scale_round(const roundie_scale_geometry_t *geometry,
                         int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2);

/* False when the strip breaks the rounder's guarantee or does not continue
 * the carried row; the caller then drops the carry and reports an error. */
bool roundie_scale_plan(const roundie_scale_geometry_t *geometry,
                        const roundie_scale_carry_t *carry,
                        int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                        roundie_scale_plan_t *plan);
void roundie_scale_update_carry(const roundie_scale_plan_t *plan,
                                roundie_scale_carry_t *carry);

/* Upper bound on the panel pixels of one transfer, carried row included, when
 * LVGL renders into a buffer of buffer_pixels: LVGL PARTIAL mode gives a
 * narrower area proportionally more rows per strip. */
uint32_t roundie_scale_max_output(const roundie_scale_geometry_t *geometry,
                                  uint32_t buffer_pixels);

/* columns[p] = logical column sampled by panel column p (physical_width). */
void roundie_scale_columns(const roundie_scale_geometry_t *geometry,
                           uint16_t *columns);

/* Writes the plan's send rows to output (row stride: plan width), the carried
 * row first, and the held-back row to carry_pixels. strip holds the logical
 * strip in LVGL's RGB565 byte order; swap writes it MSB first. */
void roundie_scale_strip(const roundie_scale_geometry_t *geometry,
                         const roundie_scale_plan_t *plan,
                         const uint16_t *columns, const uint16_t *strip,
                         uint16_t *output, uint16_t *carry_pixels, bool swap);
