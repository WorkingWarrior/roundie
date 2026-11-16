#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared development serial framer: RT1 keeps its original 63-byte limit;
 * optional RO1 uses the same RX owner and bounded printable-line parser. */
#define ROUNDIE_TIME_LINE_CAPACITY 1100

typedef struct {
    char line[ROUNDIE_TIME_LINE_CAPACITY];
    size_t used;
    bool discard;
} roundie_time_frame_t;

/* POSIX TZ rule, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"; at most 47 bytes. */
#define ROUNDIE_TIME_ZONE_CAPACITY 48

typedef struct {
    char id[9];
    bool set;
    bool restart;
    bool diagnostic;
    int64_t seconds;
    bool zone;          /* TZ: query, or set when zone_rule is non-empty */
    char zone_rule[ROUNDIE_TIME_ZONE_CAPACITY];
} roundie_time_command_t;

/* Overflow discards through the next LF. */
bool roundie_time_feed(roundie_time_frame_t *frame, char byte);
/* Returns OK, BAD_COMMAND, FRAME, or RANGE. Always initializes id. */
const char *roundie_time_parse(const roundie_time_frame_t *frame,
                               roundie_time_command_t *command);
/* Conservative POSIX TZ syntax check: printable rule characters only, starting
 * with a zone name. newlib interprets the rule; this only bounds the input. */
bool roundie_time_zone_valid(const char *rule);
/* Days since 1970-01-01 of a proleptic Gregorian date (month 1..12). */
int64_t roundie_time_days_from_civil(int64_t year, unsigned month, unsigned day);
