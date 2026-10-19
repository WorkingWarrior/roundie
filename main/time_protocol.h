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

typedef struct {
    char id[9];
    bool set;
    bool restart;
    bool diagnostic;
    int64_t seconds;
} roundie_time_command_t;

/* Overflow discards through the next LF. */
bool roundie_time_feed(roundie_time_frame_t *frame, char byte);
/* Returns OK, BAD_COMMAND, FRAME, or RANGE. Always initializes id. */
const char *roundie_time_parse(const roundie_time_frame_t *frame,
                               roundie_time_command_t *command);
