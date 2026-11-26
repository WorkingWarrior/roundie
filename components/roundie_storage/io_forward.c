#include "roundie_storage_io.h"

#include <stdio.h>
#include <string.h>

#define BUSY_CODE 259 /* ESP_ERR_INVALID_STATE */

size_t roundie_io_busy_reply(const char *line, char *out, size_t capacity)
{
    char id[9] = "00000000";
    size_t length = strlen(line);
    bool valid = length >= 14 && line[13] == ' ';
    for (size_t i = 5; valid && i < 13; i++) {
        if (line[i] < '0' || line[i] > '9') {
            valid = false;
        }
    }
    if (valid) {
        memcpy(id, line + 5, 8);
    }
    int written = snprintf(out, capacity, "\n%.5s%s %d {\"format\":1,\"error\":\"busy\"}\n",
                           line, id, BUSY_CODE);
    if (written < 0 || (size_t)written >= capacity) {
        return 0;
    }
    return (size_t)written;
}

bool roundie_io_forward(roundie_io_forwarder_t *forwarder, const char *line, bool discard)
{
    /* One caller (system_worker); static keeps 1.1 KiB off its stack. */
    static roundie_io_line_t item;
    size_t length = strlen(line);
    if (length >= sizeof(item.line)) {
        length = sizeof(item.line) - 1;
        discard = true;
    }
    memcpy(item.line, line, length);
    item.line[length] = '\0';
    item.discard = discard;
    if (forwarder->try_send(&item)) {
        forwarder->forwarded++;
        return true;
    }
    forwarder->busy++;
    char reply[64];
    size_t reply_length = roundie_io_busy_reply(item.line, reply, sizeof(reply));
    if (reply_length > 0) {
        forwarder->write(reply, reply_length);
    }
    return false;
}
