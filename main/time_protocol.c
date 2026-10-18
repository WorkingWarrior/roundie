#include "time_protocol.h"

#include <string.h>

#include "roundie_rtc_time.h"

bool roundie_time_feed(roundie_time_frame_t *frame, char byte)
{
    if (byte == '\n') {
        frame->line[frame->used] = '\0';
        return true;
    }
    if (frame->discard) {
        return false;
    }
    if (frame->used == sizeof(frame->line) - 1 || byte < 0x20 || byte > 0x7e) {
        frame->discard = true;
        return false;
    }
    frame->line[frame->used++] = byte;
    return false;
}

const char *roundie_time_parse(const roundie_time_frame_t *frame,
                               roundie_time_command_t *command)
{
    *command = (roundie_time_command_t){.id = "00000000"};
    if (frame->used < 14 || memcmp(frame->line, "!RT1 ", 5) != 0) {
        return "BAD_COMMAND";
    }
    for (size_t i = 5; i < 13; i++) {
        char digit = frame->line[i];
        if (!((digit >= '0' && digit <= '9') ||
              (digit >= 'a' && digit <= 'f'))) {
            return "BAD_COMMAND";
        }
    }
    memcpy(command->id, frame->line + 5, 8);
    if (frame->discard || frame->used >= 64) {
        return "FRAME";
    }
    if (frame->line[13] != ' ') {
        return "BAD_COMMAND";
    }
    if (strcmp(frame->line + 14, "GET") == 0) {
        return "OK";
    }
    if (strcmp(frame->line + 14, "RESTART") == 0) {
        command->restart = true;
        return "OK";
    }
    if (strncmp(frame->line + 14, "SET ", 4) != 0) {
        return "BAD_COMMAND";
    }
    const char *number = frame->line + 18;
    size_t length = strlen(number);
    for (size_t i = 0; i < length; i++) {
        if (number[i] < '0' || number[i] > '9') {
            return "BAD_COMMAND";
        }
    }
    if (length == 0 || length > 10) {
        return "RANGE";
    }
    int64_t seconds = 0;
    for (size_t i = 0; i < length; i++) {
        seconds = seconds * 10 + number[i] - '0';
    }
    roundie_rtc_time_t calendar;
    if (!roundie_rtc_from_unix(seconds, &calendar)) {
        return "RANGE";
    }
    command->set = true;
    command->seconds = seconds;
    return "OK";
}
