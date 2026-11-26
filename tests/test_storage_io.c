/* io_worker hand-over and the OTA/package claim (docs/storage-io.md, P1). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "roundie_storage_io.h"

/* --- Forwarding: a full queue answers BUSY at once (requirement 1) --- */

static int send_calls;
static bool queue_full;
static roundie_io_line_t sent;
static char written[128];
static size_t written_length;
static int write_calls;

static bool fake_try_send(const roundie_io_line_t *item)
{
    send_calls++;
    if (queue_full) {
        return false;
    }
    sent = *item;
    return true;
}

static void fake_write(const char *bytes, size_t length)
{
    write_calls++;
    assert(length < sizeof(written));
    memcpy(written, bytes, length);
    written[length] = '\0';
    written_length = length;
}

static void reset(void)
{
    send_calls = 0;
    write_calls = 0;
    written_length = 0;
    memset(&sent, 0, sizeof(sent));
}

static void test_forward(void)
{
    roundie_io_forwarder_t forwarder = {.try_send = fake_try_send, .write = fake_write};

    reset();
    queue_full = false;
    assert(roundie_io_forward(&forwarder, "!RP1 12345678 STATUS", false));
    assert(send_calls == 1 && write_calls == 0);
    assert(strcmp(sent.line, "!RP1 12345678 STATUS") == 0 && !sent.discard);
    assert(forwarder.forwarded == 1 && forwarder.busy == 0);

    /* Full queue: one non-blocking attempt, then the BUSY reply, nothing queued. */
    reset();
    queue_full = true;
    assert(!roundie_io_forward(&forwarder, "!RE1 00000042 RUN", false));
    assert(send_calls == 1);
    assert(write_calls == 1);
    assert(strcmp(written, "\n!RE1 00000042 503 {\"format\":1,\"error\":\"busy\"}\n") == 0);
    assert(written_length == strlen(written));
    assert(forwarder.forwarded == 1 && forwarder.busy == 1);

    /* A hex id, as the hosts send, is answered under that id. */
    reset();
    assert(!roundie_io_forward(&forwarder, "!RP1 9f3ab0ce STATUS", false));
    assert(strcmp(written, "\n!RP1 9f3ab0ce 503 {\"format\":1,\"error\":\"busy\"}\n") == 0);

    /* An id the parser would refuse is answered as its "00000000". */
    reset();
    assert(!roundie_io_forward(&forwarder, "!RF1 12x45678 STATUS", true));
    assert(strcmp(written, "\n!RF1 00000000 503 {\"format\":1,\"error\":\"busy\"}\n") == 0);
    assert(!roundie_io_forward(&forwarder, "!RF1 1234567F STATUS", true));
    assert(strcmp(written, "\n!RF1 00000000 503 {\"format\":1,\"error\":\"busy\"}\n") == 0);

    /* A line longer than the queue entry goes over truncated, marked discard. */
    reset();
    queue_full = false;
    static char long_line[ROUNDIE_IO_LINE_CAPACITY + 40];
    memset(long_line, 'A', sizeof(long_line) - 1);
    memcpy(long_line, "!RP1 12345678 ", 14);
    long_line[sizeof(long_line) - 1] = '\0';
    assert(roundie_io_forward(&forwarder, long_line, false));
    assert(sent.discard && strlen(sent.line) == ROUNDIE_IO_LINE_CAPACITY - 1);

    char small[8];
    assert(roundie_io_busy_reply("!RP1 12345678 STATUS", small, sizeof(small)) == 0);
}

/* --- OTA versus /storage claim (requirement 2: flags only) --- */

static bool busy_value;
static int busy_calls;
static bool held_in_busy;

static bool fake_runtime_busy(void)
{
    busy_calls++;
    held_in_busy = roundie_storage_claim_held();
    return busy_value;
}

static void test_claim(void)
{
    roundie_storage_claim_init();
    assert(!roundie_storage_claim_ota_active());
    assert(!roundie_storage_claim_held());

    /* io_worker inside a step: OTA BEGIN is refused without asking the runtime. */
    roundie_storage_claim_io_enter();
    assert(!roundie_storage_claim_held()); /* the mutex is not kept across the step */
    busy_calls = 0;
    assert(!roundie_storage_claim_ota(fake_runtime_busy));
    assert(busy_calls == 0 && !roundie_storage_claim_ota_active());
    roundie_storage_claim_io_leave();

    /* Between steps with a package job running: refused. */
    busy_value = true;
    assert(!roundie_storage_claim_ota(fake_runtime_busy));
    assert(busy_calls == 1 && held_in_busy);
    assert(!roundie_storage_claim_ota_active());

    /* Idle: granted, and admission (which reads the flag) sees it. */
    busy_value = false;
    assert(roundie_storage_claim_ota(fake_runtime_busy));
    assert(roundie_storage_claim_ota_active());
    assert(!roundie_storage_claim_held());

    /* Held: granted again without asking the runtime. */
    busy_calls = 0;
    busy_value = true;
    assert(roundie_storage_claim_ota(fake_runtime_busy));
    assert(busy_calls == 0);

    /* io steps still run while OTA holds the claim; admission refuses inside. */
    roundie_storage_claim_io_enter();
    assert(roundie_storage_claim_ota_active());
    roundie_storage_claim_io_leave();

    roundie_storage_claim_ota_release();
    assert(!roundie_storage_claim_ota_active());
}

int main(void)
{
    test_forward();
    test_claim();
    puts("storage io: PASS");
    return 0;
}
