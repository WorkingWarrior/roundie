#include "roundie_face_text.h"
#include <stdio.h>
#include <string.h>
#include "roundie_face_format.h"

static const char *const months_short[12] = {
    "sty", "lut", "mar", "kwi", "maj", "cze", "lip", "sie", "wrz", "paź", "lis", "gru",
};
static const char *const months_long[12] = {
    "stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca",
    "lipca", "sierpnia", "września", "października", "listopada", "grudnia",
};
static const char *const weekdays_short[7] = {"pon", "wt", "śr", "czw", "pt", "sob", "nd"};
static const char *const weekdays_long[7] = {
    "poniedziałek", "wtorek", "środa", "czwartek", "piątek", "sobota", "niedziela",
};

typedef enum {
    TOKEN_HH, TOKEN_H, TOKEN_hh, TOKEN_h, TOKEN_MM, TOKEN_SS, TOKEN_DD, TOKEN_D, TOKEN_MO, TOKEN_MON,
    TOKEN_MONTH, TOKEN_WD, TOKEN_WEEKDAY, TOKEN_YYYY, TOKEN_UTC, TOKEN_OFF, TOKEN_TZ, TOKEN_BAT, TOKEN_CHG,
    TOKEN_COUNT,
} token_t;

static const char *const token_names[TOKEN_COUNT] = {
    "HH", "H", "hh", "h", "MM", "SS", "DD", "D", "MO", "MON", "MONTH", "WD", "WEEKDAY", "YYYY", "UTC", "OFF",
    "TZ", "BAT", "CHG",
};

/* At *cursor == '{': the token and the position after '}'; -1 if unknown. */
static int token_at(const char *cursor, const char **after)
{
    const char *close = strchr(cursor + 1, '}');
    if (close == NULL) {
        return -1;
    }
    size_t length = (size_t)(close - cursor - 1);
    for (int i = 0; i < TOKEN_COUNT; i++) {
        if (strlen(token_names[i]) == length && memcmp(cursor + 1, token_names[i], length) == 0) {
            *after = close + 1;
            return i;
        }
    }
    return -1;
}

bool roundie_face_template_valid(const char *template)
{
    for (const char *cursor = template; *cursor != '\0';) {
        if (*cursor == '{' && cursor[1] == '{') {
            cursor += 2;
        } else if (*cursor == '{') {
            if (token_at(cursor, &cursor) < 0) {
                return false;
            }
        } else if (*cursor == '}') {
            return false;
        } else {
            cursor++;
        }
    }
    return true;
}

bool roundie_face_template_uses(const char *template, unsigned what)
{
    for (const char *cursor = template; *cursor != '\0';) {
        if (*cursor == '{' && cursor[1] == '{') {
            cursor += 2;
            continue;
        }
        if (*cursor != '{') {
            cursor++;
            continue;
        }
        int token = token_at(cursor, &cursor);
        if (token < 0) {
            return false;
        }
        if (((what & ROUNDIE_FACE_USES_SECOND) != 0 && token == TOKEN_SS) ||
            ((what & ROUNDIE_FACE_USES_BATTERY) != 0 && (token == TOKEN_BAT || token == TOKEN_CHG))) {
            return true;
        }
    }
    return false;
}

roundie_face_date_t roundie_face_civil(uint32_t days)
{
    /* Howard Hinnant's civil_from_days, for days >= 0. */
    int64_t z = (int64_t)days + 719468;
    int64_t era = z / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    roundie_face_date_t date = {
        .year = (int32_t)(y + (m <= 2)),
        .month = (uint8_t)m,
        .day = (uint8_t)d,
        .weekday = (uint8_t)((days + 3U) % 7U), /* 1970-01-01 was a Thursday */
    };
    return date;
}

/* Appends text, cutting at a UTF-8 boundary. */
static void append(char *out, size_t capacity, size_t *used, const char *text)
{
    size_t length = strlen(text);
    size_t room = capacity - 1 - *used;
    if (length > room) {
        length = room;
        while (length > 0 && ((unsigned char)text[length] & 0xc0) == 0x80) {
            length--;
        }
    }
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
}

static void two(char out[4], unsigned value)
{
    snprintf(out, 4, "%02u", value % 100U);
}

static void token_text(int token, const rwf_data_v1_t *data, char *text, size_t size)
{
    bool time_valid = (data->flags & RWF_DATA_TIME_VALID) != 0;
    bool time_token = token != TOKEN_BAT && token != TOKEN_CHG && token != TOKEN_TZ;
    if (time_token && !time_valid) {
        snprintf(text, size, "%s", token == TOKEN_UTC || token == TOKEN_OFF ? "--:--" : "--");
        return;
    }
    unsigned seconds = data->ms_of_day / 1000U;
    unsigned hours = seconds / 3600U;
    unsigned minutes = seconds / 60U % 60U;
    unsigned twelve = hours % 12U == 0 ? 12U : hours % 12U;
    roundie_face_date_t date = roundie_face_civil(data->local_days);
    char pair[4];
    switch (token) {
    case TOKEN_HH: two(pair, hours); snprintf(text, size, "%s", pair); break;
    case TOKEN_H: snprintf(text, size, "%u", hours); break;
    case TOKEN_hh: two(pair, twelve); snprintf(text, size, "%s", pair); break;
    case TOKEN_h: snprintf(text, size, "%u", twelve); break;
    case TOKEN_MM: two(pair, minutes); snprintf(text, size, "%s", pair); break;
    case TOKEN_SS: two(pair, seconds % 60U); snprintf(text, size, "%s", pair); break;
    case TOKEN_DD: two(pair, date.day); snprintf(text, size, "%s", pair); break;
    case TOKEN_D: snprintf(text, size, "%u", (unsigned)date.day); break;
    case TOKEN_MO: two(pair, date.month); snprintf(text, size, "%s", pair); break;
    case TOKEN_MON: snprintf(text, size, "%s", months_short[date.month - 1]); break;
    case TOKEN_MONTH: snprintf(text, size, "%s", months_long[date.month - 1]); break;
    case TOKEN_WD: snprintf(text, size, "%s", weekdays_short[date.weekday]); break;
    case TOKEN_WEEKDAY: snprintf(text, size, "%s", weekdays_long[date.weekday]); break;
    case TOKEN_YYYY: snprintf(text, size, "%04ld", (long)date.year); break;
    case TOKEN_UTC: {
        int64_t of_day = data->utc_seconds % 86400;
        if (of_day < 0) {
            of_day += 86400;
        }
        snprintf(text, size, "%02u:%02u", (unsigned)(of_day / 3600), (unsigned)(of_day / 60 % 60));
        break;
    }
    case TOKEN_OFF: {
        int32_t offset = data->utc_offset_s;
        unsigned magnitude = (unsigned)(offset < 0 ? -offset : offset) / 60U;
        snprintf(text, size, "%c%02u:%02u", offset < 0 ? '-' : '+', magnitude / 60U % 100U, magnitude % 60U);
        break;
    }
    case TOKEN_TZ: snprintf(text, size, "%.*s", (int)strnlen(data->zone, sizeof(data->zone)), data->zone); break;
    case TOKEN_BAT:
        if ((data->flags & RWF_DATA_BATTERY_VALID) != 0) {
            snprintf(text, size, "%u", (unsigned)data->battery_percent);
        } else {
            snprintf(text, size, "--");
        }
        break;
    case TOKEN_CHG: snprintf(text, size, "%s", (data->flags & RWF_DATA_CHARGING) != 0 ? "ładowanie" : ""); break;
    default: text[0] = '\0'; break;
    }
}

size_t roundie_face_expand(const char *template, const rwf_data_v1_t *data, char *out, size_t capacity)
{
    size_t used = 0;
    out[0] = '\0';
    for (const char *cursor = template; *cursor != '\0' && used + 1 < capacity;) {
        if (*cursor == '{' && cursor[1] == '{') {
            append(out, capacity, &used, "{");
            cursor += 2;
            continue;
        }
        if (*cursor == '{') {
            int token = token_at(cursor, &cursor);
            if (token < 0) {
                break; /* validated before use; never reached for a decoded face */
            }
            char text[ROUNDIE_FACE_TEXT_MAX];
            token_text(token, data, text, sizeof(text));
            append(out, capacity, &used, text);
            continue;
        }
        /* Copy one UTF-8 sequence whole. */
        size_t length = 1;
        while (((unsigned char)cursor[length] & 0xc0) == 0x80) {
            length++;
        }
        char sequence[5] = {0};
        memcpy(sequence, cursor, length < 4 ? length : 4);
        append(out, capacity, &used, sequence);
        cursor += length;
    }
    return used;
}

uint32_t roundie_face_next_boundary_ms(uint32_t ms_of_day, uint32_t needs)
{
    uint32_t period = (needs & RWF_NEED_SECOND) != 0 ? 1000U : 60000U;
    uint32_t remaining = period - ms_of_day % period;
    return remaining == 0 ? 1U : remaining;
}

uint32_t roundie_face_hand_angle(uint32_t ms_of_day, unsigned hand)
{
    uint32_t seconds = ms_of_day / 1000U % 86400U;
    if (hand == ROUNDIE_FACE_HAND_HOUR) {
        return seconds % 43200U * 3600U / 43200U; /* 0.1 degree per 12 s */
    }
    if (hand == ROUNDIE_FACE_HAND_MINUTE) {
        return seconds % 3600U; /* 0.1 degree per second */
    }
    return seconds % 60U * 60U; /* the second hand steps */
}

unsigned roundie_face_changes(const rwf_data_v1_t *previous, const rwf_data_v1_t *current, uint32_t needs,
                              uint32_t *kinds, unsigned capacity)
{
    unsigned count = 0;
#define ADD(kind) \
    do { \
        if (count < capacity) { \
            kinds[count++] = (kind); \
        } \
    } while (0)
    if (previous == NULL) {
        ADD(RWF_EVENT_ACTIVATED);
        return count;
    }
    uint32_t time_flags = RWF_DATA_TIME_VALID;
    bool valid = (current->flags & RWF_DATA_TIME_VALID) != 0;
    if ((previous->flags & time_flags) != (current->flags & time_flags) ||
        previous->time_revision != current->time_revision || strcmp(previous->zone, current->zone) != 0 ||
        previous->utc_offset_s != current->utc_offset_s) {
        ADD(RWF_EVENT_ZONE);
    }
    if (valid && previous->local_days != current->local_days) {
        ADD(RWF_EVENT_DATE);
    }
    if (valid && (previous->local_days != current->local_days ||
                  previous->ms_of_day / 60000U != current->ms_of_day / 60000U)) {
        ADD(RWF_EVENT_MINUTE);
    }
    if (valid && (needs & RWF_NEED_SECOND) != 0 &&
        (previous->local_days != current->local_days || previous->ms_of_day / 1000U != current->ms_of_day / 1000U)) {
        ADD(RWF_EVENT_SECOND);
    }
    uint32_t battery_flags = RWF_DATA_BATTERY_VALID | RWF_DATA_CHARGING;
    if ((needs & RWF_NEED_BATTERY) != 0 &&
        ((previous->flags & battery_flags) != (current->flags & battery_flags) ||
         previous->battery_percent != current->battery_percent)) {
        ADD(RWF_EVENT_BATTERY);
    }
    if ((previous->flags & RWF_DATA_VISIBLE) != (current->flags & RWF_DATA_VISIBLE)) {
        ADD(RWF_EVENT_VISIBLE);
    }
#undef ADD
    return count;
}
