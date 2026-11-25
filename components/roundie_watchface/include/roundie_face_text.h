#pragma once
/* Pure time, text and schedule helpers of the watch face platform
 * (docs/watch-face-platform.md). No LVGL, no clock reads: everything works on
 * an rwf_data_v1_t sample, so host tests drive it with any time. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rwf/rwf_face_v1.h"

#define ROUNDIE_FACE_TEXT_MAX 64U /* expanded label, with the NUL */

/* Label template tokens (docs/watch-face-packages.md):
 *   {HH} {H} hours 00-23 / 0-23     {hh} {h} hours 01-12 / 1-12
 *   {MM} minutes  {SS} seconds       {DD} {D} day  {MO} month number
 *   {MON} sty..gru  {MONTH} stycznia..grudnia  {WD} pon..nd
 *   {WEEKDAY} poniedziałek..niedziela  {YYYY} year
 *   {UTC} UTC hh:mm  {OFF} +hh:mm  {TZ} zone rule
 *   {BAT} battery percent or "--"   {CHG} "ładowanie" while charging
 * Time tokens show "--" without a trusted time. Text outside braces is
 * copied; "{{" is a literal brace. */
bool roundie_face_template_valid(const char *template);

#define ROUNDIE_FACE_USES_SECOND 1U
#define ROUNDIE_FACE_USES_BATTERY 2U
bool roundie_face_template_uses(const char *template, unsigned what);

/* Expands into out (capacity bytes, at least 1); truncates at a UTF-8
 * boundary. Returns the length written. */
size_t roundie_face_expand(const char *template, const rwf_data_v1_t *data, char *out, size_t capacity);

typedef struct {
    int32_t year;
    uint8_t month;   /* 1..12 */
    uint8_t day;     /* 1..31 */
    uint8_t weekday; /* 0 Monday .. 6 Sunday */
} roundie_face_date_t;

roundie_face_date_t roundie_face_civil(uint32_t days);

/* Milliseconds until the next boundary the needs ask for: the next second
 * (RWF_NEED_SECOND) or the next minute. At least 1. */
uint32_t roundie_face_next_boundary_ms(uint32_t ms_of_day, uint32_t needs);

/* Hand angle in tenths of a degree clockwise from 12 o'clock. */
uint32_t roundie_face_hand_angle(uint32_t ms_of_day, unsigned hand);

/* Events between two samples, most significant first: ZONE, DATE, MINUTE,
 * SECOND, BATTERY, VISIBLE. Returns how many were written (at most capacity).
 * previous == NULL means the first sample after activation. */
unsigned roundie_face_changes(const rwf_data_v1_t *previous, const rwf_data_v1_t *current, uint32_t needs,
                              uint32_t *kinds, unsigned capacity);
