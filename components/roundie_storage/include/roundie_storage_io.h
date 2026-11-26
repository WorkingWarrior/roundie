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

/* Reply code for a transient refusal: the request was not executed and the
 * host may send it again (io_worker queue full, or an io_worker step under way
 * for OTA BEGIN). A refusal because a precondition does not hold (an ELF still
 * loaded, Safe Mode, ...) keeps ESP_ERR_INVALID_STATE (259) and is not retried.
 * 503 is not an esp_err_t value. */
#define ROUNDIE_IO_BUSY_CODE 503

/* The BUSY reply for a forwarded line: "\n!RP1 <id> 503 {"format":1,"error":"busy"}\n".
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
typedef enum {
    ROUNDIE_STORAGE_CLAIM_GRANTED = 0,
    ROUNDIE_STORAGE_CLAIM_IO_STEP,      /* io_worker is between two jobs' check and claim: transient */
    ROUNDIE_STORAGE_CLAIM_RUNTIME_BUSY, /* runtime_busy() returned true: precondition */
} roundie_storage_claim_result_t;
roundie_storage_claim_result_t roundie_storage_claim_ota_try(bool (*runtime_busy)(void));
/* roundie_storage_claim_ota_try() == ROUNDIE_STORAGE_CLAIM_GRANTED. */
bool roundie_storage_claim_ota(bool (*runtime_busy)(void));
void roundie_storage_claim_ota_release(void);
bool roundie_storage_claim_ota_active(void);
/* True while the calling task holds the claim mutex (for the VFS assertion). */
bool roundie_storage_claim_held(void);

/* Owner check for /storage VFS calls (docs/storage-io.md, P1 step 3). The
 * calling task becomes the owner: the main task at startup, then io_worker.
 * Before any owner is set every caller passes. */
typedef enum {
    ROUNDIE_STORAGE_ACCESS_OK,
    ROUNDIE_STORAGE_ACCESS_NOT_OWNER,
    ROUNDIE_STORAGE_ACCESS_UNDER_CLAIM,
} roundie_storage_access_t;

void roundie_storage_owner_set(void);
/* Development storage bench only (main/storage_bench.c): its helper task runs
 * /storage I/O at a chosen priority and core while it is registered as the one
 * guest; clear it before the task ends. */
void roundie_storage_guest_set(void);
void roundie_storage_guest_clear(void);
/* The calling task's right to a VFS call now; UNDER_CLAIM wins over NOT_OWNER. */
roundie_storage_access_t roundie_storage_access_check(void);
