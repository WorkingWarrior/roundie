#pragma once

/* io_worker hand-over and the OTA/package claim (docs/storage-io.md, P1).
 *
 * Portable C: the firmware supplies the queue and serial writer, the host
 * tests substitute them. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Same bound as the serial framer (ROUNDIE_TIME_LINE_CAPACITY); main/io_worker.c
 * asserts that the two agree. */
#define ROUNDIE_IO_LINE_CAPACITY 1100

typedef struct {
    char line[ROUNDIE_IO_LINE_CAPACITY];
    bool discard;
} roundie_io_line_t;

typedef struct {
    /* Must not wait for room: returns false at once when the queue is full. */
    bool (*try_send)(const roundie_io_line_t *item);
    /* Writes one whole reply line. */
    void (*write)(const char *bytes, size_t length);
    uint32_t forwarded;
    uint32_t busy;
} roundie_io_forwarder_t;

/* Hands one io-owned line ("!RP1 ", "!RF1 ", "!RE1 ") to io_worker. On a full
 * queue it answers BUSY at once and counts it. Returns true if enqueued. A line
 * longer than the queue entry is forwarded truncated with discard set, so the
 * handler answers it as an oversized frame. */
bool roundie_io_forward(roundie_io_forwarder_t *forwarder, const char *line, bool discard);

/* The BUSY reply for a forwarded line: "\n!RP1 <id> 259 {"format":1,"error":"busy"}\n".
 * 259 is ESP_ERR_INVALID_STATE, the code these protocols already use for a busy
 * device. An id that is not 8 decimal digits becomes "00000000", as in the
 * transfer parser. Returns the length, or 0 if it does not fit. */
size_t roundie_io_busy_reply(const char *line, char *out, size_t capacity);

/* OTA versus /storage jobs. One mutex guards only these flags: it is never held
 * across a VFS call. io_worker brackets each step with enter/leave; OTA BEGIN
 * claims only between steps and only when the runtime is idle. Package
 * admission refuses while the OTA claim is held. */
void roundie_storage_claim_init(void);
void roundie_storage_claim_io_enter(void);
void roundie_storage_claim_io_leave(void);
/* runtime_busy is called under the mutex and must not touch the filesystem. */
bool roundie_storage_claim_ota(bool (*runtime_busy)(void));
void roundie_storage_claim_ota_release(void);
bool roundie_storage_claim_ota_active(void);
/* True while the calling task holds the claim mutex (for the VFS assertion). */
bool roundie_storage_claim_held(void);
