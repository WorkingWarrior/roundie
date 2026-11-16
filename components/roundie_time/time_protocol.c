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
    if (strcmp(frame->line + 14, "DIAG") == 0) {
        command->diagnostic = true;
        return "OK";
    }
    if (strcmp(frame->line + 14, "RESTART") == 0) {
        command->restart = true;
        return "OK";
    }
    if (strcmp(frame->line + 14, "TZ") == 0) {
        command->zone = true;
        return "OK";
    }
    if (strncmp(frame->line + 14, "TZ ", 3) == 0) {
        const char *rule = frame->line + 17;
        if (!roundie_time_zone_valid(rule)) {
            return "RANGE";
        }
        command->zone = true;
        strcpy(command->zone_rule, rule);
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

bool roundie_time_zone_valid(const char *rule)
{
    size_t length = strlen(rule);
    if (length == 0 || length >= ROUNDIE_TIME_ZONE_CAPACITY) {
        return false;
    }
    char first = rule[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '<')) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        char c = rule[i];
        bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        bool digit = c >= '0' && c <= '9';
        if (!letter && !digit && strchr("<>+-,./:", c) == NULL) {
            return false;
        }
    }
    return true;
}

int64_t roundie_time_days_from_civil(int64_t year, unsigned month, unsigned day)
{
    /* Howard Hinnant's days_from_civil, with March-based years. */
    year -= month <= 2 ? 1 : 0;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int64_t year_of_era = year - era * 400;
    int64_t shifted_month = month > 2 ? (int64_t)month - 3 : (int64_t)month + 9;
    int64_t day_of_year = (153 * shifted_month + 2) / 5 + (int64_t)day - 1;
    int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}
