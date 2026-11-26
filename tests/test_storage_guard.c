/* Bounded I/O over a fake VFS, the owner check and the VFS-under-mutex check
 * (docs/storage-io.md, P1 step 3). Built with -DROUNDIE_IO_READ=fake_read
 * -DROUNDIE_IO_WRITE=fake_write. */
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "roundie_bounded_io.h"
#include "roundie_storage_io.h"

/* --- Fake VFS: a byte source/sink with scripted call results --- */

static uint8_t source[3 * ROUNDIE_IO_CHUNK + 100];
static size_t source_size;
static size_t position;
static size_t short_limit; /* 0: no limit */
static size_t eof_at;      /* end of file at this offset */
static int fail_call;      /* this call number returns -1 (0: never) */
static int calls;
static size_t largest_call;
static int hooks;
static int hook_calls_seen[16];

ssize_t fake_read(int file, void *out, size_t size);
ssize_t fake_write(int file, const void *data, size_t size);

static ssize_t fake_step(size_t size)
{
    calls++;
    if (size > largest_call) {
        largest_call = size;
    }
    if (fail_call != 0 && calls == fail_call) {
        return -1;
    }
    size_t count = size;
    if (short_limit != 0 && count > short_limit) {
        count = short_limit;
    }
    if (position + count > eof_at) {
        count = eof_at - position;
    }
    return (ssize_t)count;
}

ssize_t fake_read(int file, void *out, size_t size)
{
    (void)file;
    ssize_t count = fake_step(size);
    if (count > 0) {
        memcpy(out, source + position, (size_t)count);
        position += (size_t)count;
    }
    return count;
}

ssize_t fake_write(int file, const void *data, size_t size)
{
    (void)file;
    ssize_t count = fake_step(size);
    if (count > 0) {
        memcpy(source + position, data, (size_t)count);
        position += (size_t)count;
    }
    return count;
}

static void hook(void)
{
    if (hooks < 16) {
        hook_calls_seen[hooks] = calls;
    }
    hooks++;
}

static void reset(size_t size)
{
    source_size = size;
    position = 0;
    short_limit = 0;
    eof_at = size;
    fail_call = 0;
    calls = 0;
    largest_call = 0;
    hooks = 0;
    for (size_t i = 0; i < sizeof(source); i++) {
        source[i] = (uint8_t)(i * 7U + 3U);
    }
}

static uint8_t out[sizeof(source)];

static void test_bounded_read(void)
{
    roundie_io_set_chunk_hook(hook);

    /* Whole chunks: 3 calls of 4 KiB plus the tail, hook between calls only. */
    reset(sizeof(source));
    assert(roundie_io_read_exact(3, out, source_size));
    assert(memcmp(out, source, source_size) == 0);
    assert(calls == 4 && largest_call == ROUNDIE_IO_CHUNK);
    assert(hooks == 3 && hook_calls_seen[0] == 1 && hook_calls_seen[2] == 3);

    /* Short reads everywhere: every byte arrives, no call above 4 KiB. */
    reset(sizeof(source));
    short_limit = 1000;
    assert(roundie_io_read_exact(3, out, source_size));
    assert(memcmp(out, source, source_size) == 0);
    assert(calls == (int)((source_size + 999) / 1000) && largest_call == ROUNDIE_IO_CHUNK);

    /* End of file in the middle of the second chunk: fails, never reads on. */
    reset(sizeof(source));
    eof_at = ROUNDIE_IO_CHUNK + 1234;
    assert(!roundie_io_read_exact(3, out, source_size));
    assert(position == eof_at && calls == 3);

    /* End of file after a short read inside one chunk. */
    reset(3000);
    short_limit = 700;
    eof_at = 2000;
    assert(!roundie_io_read_exact(3, out, 3000));
    assert(position == 2000);

    /* An error on the second call fails the transfer. */
    reset(sizeof(source));
    fail_call = 2;
    assert(!roundie_io_read_exact(3, out, source_size));
    assert(calls == 2);

    /* Zero bytes makes no call. */
    reset(10);
    assert(roundie_io_read_exact(3, out, 0) && calls == 0 && hooks == 0);
    roundie_io_set_chunk_hook(NULL);
}

static void test_bounded_write(void)
{
    static uint8_t data[2 * ROUNDIE_IO_CHUNK + 10];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 13U);
    }

    reset(sizeof(source));
    short_limit = 3000;
    assert(roundie_io_write_exact(4, data, sizeof(data)));
    assert(memcmp(source, data, sizeof(data)) == 0 && largest_call == ROUNDIE_IO_CHUNK);

    /* A call that writes nothing (full medium) fails. */
    reset(sizeof(source));
    eof_at = 5000;
    assert(!roundie_io_write_exact(4, data, sizeof(data)));
    assert(position == 5000);
}

/* --- Owner and claim-mutex checks through the guard (negative tests) --- */

static int violations[3];

static void recording_guard(void)
{
    violations[roundie_storage_access_check()]++;
}

static bool read_once(void)
{
    reset(16);
    return roundie_io_read_exact(3, out, 16);
}

static void *other_task(void *argument)
{
    (void)argument;
    assert(roundie_storage_access_check() == ROUNDIE_STORAGE_ACCESS_NOT_OWNER);
    read_once();
    return NULL;
}

static bool busy_with_vfs(void)
{
    /* Wrong on purpose: a VFS call inside the claim mutex. */
    read_once();
    return false;
}

static void test_guard(void)
{
    roundie_storage_claim_init();
    roundie_io_set_access_guard(recording_guard);

    /* No owner yet: every caller passes. */
    assert(roundie_storage_access_check() == ROUNDIE_STORAGE_ACCESS_OK);

    roundie_storage_owner_set();
    assert(read_once());
    assert(violations[ROUNDIE_STORAGE_ACCESS_OK] == 1);

    /* Another task than the owner: NOT_OWNER on its call. */
    pthread_t thread;
    assert(pthread_create(&thread, NULL, other_task, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(violations[ROUNDIE_STORAGE_ACCESS_NOT_OWNER] == 1);

    /* The owner itself, but under the claim mutex: UNDER_CLAIM. */
    assert(roundie_storage_claim_ota(busy_with_vfs));
    assert(violations[ROUNDIE_STORAGE_ACCESS_UNDER_CLAIM] == 1);
    roundie_storage_claim_ota_release();

    /* After the mutex is released the owner passes again. */
    assert(read_once());
    assert(violations[ROUNDIE_STORAGE_ACCESS_OK] == 2);
    assert(violations[ROUNDIE_STORAGE_ACCESS_NOT_OWNER] == 1);
    assert(violations[ROUNDIE_STORAGE_ACCESS_UNDER_CLAIM] == 1);
    roundie_io_set_access_guard(NULL);
}

/* Every other VFS helper is guarded too: one call of each, on a real
 * temporary directory, must give exactly one guard verdict. */
enum { OPERATIONS = 15 };
static char directory[64];

static void every_operation(void)
{
    char file_path[96];
    char moved_path[96];
    char sub_path[96];
    snprintf(file_path, sizeof(file_path), "%s/f", directory);
    snprintf(moved_path, sizeof(moved_path), "%s/g", directory);
    snprintf(sub_path, sizeof(sub_path), "%s/d", directory);
    struct stat info;
    uint8_t byte = 7;
    int file = roundie_io_open(file_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(file >= 0);
    reset(1);
    (void)roundie_io_write(file, &byte, 1);
    (void)roundie_io_read(file, &byte, 1);
    (void)roundie_io_lseek(file, 0, SEEK_SET);
    assert(roundie_io_fstat(file, &info) == 0);
    assert(roundie_io_fsync(file) == 0);
    assert(roundie_io_close(file) == 0);
    assert(roundie_io_stat(file_path, &info) == 0);
    assert(roundie_io_rename(file_path, moved_path) == 0);
    assert(roundie_io_unlink(moved_path) == 0);
    assert(roundie_io_mkdir(sub_path, 0700) == 0);
    DIR *listing = roundie_io_opendir(directory);
    assert(listing != NULL);
    (void)roundie_io_readdir(listing);
    assert(roundie_io_closedir(listing) == 0);
    assert(roundie_io_rmdir(sub_path) == 0);
}

static void *other_task_operations(void *argument)
{
    (void)argument;
    every_operation();
    return NULL;
}

static void *guest_task_operations(void *argument)
{
    (void)argument;
    roundie_storage_guest_set();
    every_operation();
    roundie_storage_guest_clear();
    assert(roundie_storage_access_check() == ROUNDIE_STORAGE_ACCESS_NOT_OWNER);
    return NULL;
}

static bool busy_with_operations(void)
{
    every_operation(); /* Wrong on purpose: VFS inside the claim mutex. */
    return false;
}

static void test_every_operation(void)
{
    snprintf(directory, sizeof(directory), "/tmp/roundie-guard-XXXXXX");
    assert(mkdtemp(directory) != NULL);
    roundie_io_set_access_guard(recording_guard);
    memset(violations, 0, sizeof(violations));
    roundie_storage_owner_set();

    every_operation();
    assert(violations[ROUNDIE_STORAGE_ACCESS_OK] == OPERATIONS);

    pthread_t thread;
    assert(pthread_create(&thread, NULL, other_task_operations, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(violations[ROUNDIE_STORAGE_ACCESS_NOT_OWNER] == OPERATIONS);

    assert(roundie_storage_claim_ota(busy_with_operations));
    roundie_storage_claim_ota_release();
    assert(violations[ROUNDIE_STORAGE_ACCESS_UNDER_CLAIM] == OPERATIONS);

    /* The registered bench guest passes; once cleared it is refused again. */
    assert(pthread_create(&thread, NULL, guest_task_operations, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(violations[ROUNDIE_STORAGE_ACCESS_OK] == 2 * OPERATIONS);
    assert(violations[ROUNDIE_STORAGE_ACCESS_NOT_OWNER] == OPERATIONS);
    roundie_io_set_access_guard(NULL);
    assert(rmdir(directory) == 0);
}

int main(void)
{
    test_bounded_read();
    test_bounded_write();
    test_guard();
    test_every_operation();
    puts("storage guard: PASS");
    return 0;
}
