#include <assert.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../components/roundie_storage_mode/import.c"

#define MAX_PACKAGE 65760U
#define S(seconds) (INT64_C(1000000) * (seconds))

typedef struct {
    char name[64];
    uint8_t *data;
    size_t size;
    bool regular;
    bool removed;
} file_t;

static file_t files[40];
static unsigned file_count;
static int open_index = -1;
static size_t read_position;
static size_t read_budget; /* bytes still readable before the volume "vanishes"; 0 = unlimited */
static bool list_fails;
static unsigned reads_this_step, max_reads_per_step, open_count, close_count;
static EVP_MD_CTX *hash;

/* PackageManager mock */
static struct {
    bool open;
    uint32_t session;
    size_t size, received;
    uint8_t digest[32];
    EVP_MD_CTX *context;
    unsigned begins, aborts, finishes, installs;
    bool fail_begin, fail_finish, fail_install, fail_write;
    char installed_ids[8][64];
    unsigned installed_count;
    uint8_t last_digest[32];
    uint32_t sessions[16];
    unsigned session_count;
    uint8_t finished_digest[32];
} manager;

static int mock_list(void *context, roundie_import_entry_t *entries, unsigned capacity, unsigned *count)
{
    (void)context;
    if (list_fails) {
        return -1;
    }
    unsigned out = 0;
    for (unsigned index = 0; index < file_count && out < capacity; index++) {
        if (files[index].removed) {
            continue;
        }
        snprintf(entries[out].name, sizeof(entries[out].name), "%s", files[index].name);
        entries[out].size = files[index].size;
        entries[out].regular = files[index].regular;
        out++;
    }
    *count = out;
    return 0;
}

static int mock_open(void *context, const char *name)
{
    (void)context;
    for (unsigned index = 0; index < file_count; index++) {
        if (!files[index].removed && strcmp(files[index].name, name) == 0) {
            assert(open_index < 0);
            open_index = (int)index;
            read_position = 0;
            open_count++;
            return 0;
        }
    }
    return -1;
}

static int mock_read(void *context, void *buffer, size_t capacity, size_t *length)
{
    (void)context;
    assert(open_index >= 0);
    reads_this_step++;
    if (reads_this_step > max_reads_per_step) {
        max_reads_per_step = reads_this_step;
    }
    const file_t *file = &files[open_index];
    size_t take = file->size - read_position;
    if (take > capacity) {
        take = capacity;
    }
    if (read_budget != 0) {
        if (read_budget < take) {
            return -1;
        }
        read_budget -= take;
    }
    memcpy(buffer, file->data + read_position, take);
    read_position += take;
    *length = take;
    return 0;
}

static void mock_close(void *context)
{
    (void)context;
    assert(open_index >= 0);
    open_index = -1;
    close_count++;
}

static int mock_remove(void *context, const char *name)
{
    (void)context;
    for (unsigned index = 0; index < file_count; index++) {
        if (strcmp(files[index].name, name) == 0) {
            files[index].removed = true;
        }
    }
    return 0;
}

static void mock_sha_start(void *context)
{
    (void)context;
    EVP_DigestInit_ex(hash, EVP_sha256(), NULL);
}

static void mock_sha_update(void *context, const void *data, size_t length)
{
    (void)context;
    EVP_DigestUpdate(hash, data, length);
}

static void mock_sha_finish(void *context, uint8_t digest[32])
{
    (void)context;
    unsigned size = 32;
    EVP_DigestFinal_ex(hash, digest, &size);
}

/* A package in these tests starts with "RPK!", then the name and three version numbers. */
static bool mock_describe(void *context, const uint8_t *prefix, size_t available, size_t total,
                          roundie_import_candidate_t *out, char reason[ROUNDIE_IMPORT_REASON])
{
    (void)context;
    if (available < ROUNDIE_IMPORT_PREFIX || total < ROUNDIE_IMPORT_PREFIX || memcmp(prefix, "RPK!", 4) != 0) {
        snprintf(reason, ROUNDIE_IMPORT_REASON, "not a package");
        return false;
    }
    snprintf(out->name, sizeof(out->name), "%s", (const char *)prefix + 8);
    snprintf(out->id, sizeof(out->id), "org.test.%.40s", out->name);
    memcpy(out->version, prefix + 4, 4);
    out->version[0] = prefix[4];
    out->version[1] = prefix[5];
    out->version[2] = prefix[6];
    return true;
}

static bool mock_installed(void *context, const char *id)
{
    (void)context;
    for (unsigned index = 0; index < manager.installed_count; index++) {
        if (strcmp(manager.installed_ids[index], id) == 0) {
            return true;
        }
    }
    return false;
}

static bool ready_now = true;
static bool mock_ready(void *context)
{
    (void)context;
    return ready_now;
}

static int mock_begin(void *context, uint32_t session, size_t size, const uint8_t digest[32])
{
    (void)context;
    assert(!manager.open && session != 0);
    for (unsigned index = 0; index < manager.session_count; index++) {
        assert(manager.sessions[index] != session); /* sessions never repeat */
    }
    manager.sessions[manager.session_count++] = session;
    manager.begins++;
    if (manager.fail_begin) {
        return -1;
    }
    manager.open = true;
    manager.session = session;
    manager.size = size;
    manager.received = 0;
    memcpy(manager.digest, digest, 32);
    EVP_DigestInit_ex(manager.context, EVP_sha256(), NULL);
    return 0;
}

static int mock_write(void *context, uint32_t session, size_t offset, const void *data, size_t length)
{
    (void)context;
    assert(manager.open && session == manager.session && offset == manager.received);
    assert(length >= 1 && length <= ROUNDIE_IMPORT_CHUNK);
    if (manager.fail_write) {
        return -1;
    }
    EVP_DigestUpdate(manager.context, data, length);
    manager.received += length;
    return 0;
}

static int mock_finish(void *context, uint32_t session)
{
    (void)context;
    assert(manager.open && session == manager.session);
    manager.finishes++;
    if (manager.fail_finish || manager.received != manager.size) {
        return -1;
    }
    uint8_t digest[32];
    unsigned size = 32;
    EVP_DigestFinal_ex(manager.context, digest, &size);
    assert(memcmp(digest, manager.digest, 32) == 0); /* staged bytes are what was hashed */
    memcpy(manager.finished_digest, digest, 32);
    return 0;
}

static int mock_install(void *context, const uint8_t digest[32])
{
    (void)context;
    assert(manager.open && memcmp(digest, manager.finished_digest, 32) == 0);
    manager.installs++;
    if (manager.fail_install) {
        return -1;
    }
    manager.open = false;
    return 0;
}

static int mock_abort(void *context, uint32_t session)
{
    (void)context;
    assert(manager.open && session == manager.session);
    manager.aborts++;
    manager.open = false;
    return 0;
}

static void mock_last_error(void *context, char reason[ROUNDIE_IMPORT_REASON])
{
    (void)context;
    snprintf(reason, ROUNDIE_IMPORT_REASON, "manager said no");
}

static const roundie_import_ops_t ops = {
    .list = mock_list, .open = mock_open, .read = mock_read, .close = mock_close, .remove = mock_remove,
    .sha256_start = mock_sha_start, .sha256_update = mock_sha_update, .sha256_finish = mock_sha_finish,
    .describe = mock_describe, .installed = mock_installed, .ready = mock_ready, .begin = mock_begin, .write = mock_write,
    .finish = mock_finish, .install = mock_install, .abort = mock_abort, .last_error = mock_last_error,
};

static void reset(void)
{
    for (unsigned index = 0; index < file_count; index++) {
        free(files[index].data);
    }
    EVP_MD_CTX *keep_hash = hash;
    EVP_MD_CTX *keep_context = manager.context;
    memset(files, 0, sizeof(files));
    file_count = 0;
    memset(&manager, 0, sizeof(manager));
    manager.context = keep_context;
    hash = keep_hash;
    open_index = -1;
    read_budget = 0;
    list_fails = false;
    max_reads_per_step = open_count = close_count = 0;
}

static void add_file(const char *name, size_t size, bool regular, const char *package_name, unsigned major)
{
    file_t *file = &files[file_count++];
    snprintf(file->name, sizeof(file->name), "%s", name);
    file->size = size;
    file->regular = regular;
    file->data = calloc(1, size > 0 ? size : 1);
    for (size_t index = 0; index < size; index++) {
        file->data[index] = (uint8_t)((index * 31 + file_count) & 0xff);
    }
    if (package_name != NULL && size >= ROUNDIE_IMPORT_PREFIX) {
        memcpy(file->data, "RPK!", 4);
        file->data[4] = (uint8_t)major;
        file->data[5] = 0;
        file->data[6] = 1;
        memset(file->data + 8, 0, 64);
        snprintf((char *)file->data + 8, 64, "%s", package_name);
    }
}

static unsigned run(roundie_import_t *import, int64_t *now, int answer, int64_t answer_delay_us)
{
    /* answer: 0 never, 1 install, 2 skip */
    unsigned steps = 0;
    int64_t asked_at = -1;
    while (roundie_import_step(import, *now)) {
        steps++;
        assert(steps < 100000);
        reads_this_step = 0;
        if (import->status.phase == ROUNDIE_IMPORT_AWAITING_CONFIRMATION) {
            if (asked_at < 0) {
                asked_at = *now;
            }
            if (answer != 0 && *now - asked_at >= answer_delay_us) {
                roundie_import_decide(import, answer == 1);
            }
            *now += S(1);
        } else {
            asked_at = -1;
            *now += 1000;
        }
    }
    return steps;
}

int main(void)
{
    hash = EVP_MD_CTX_new();
    manager.context = EVP_MD_CTX_new();
    roundie_import_t *import = calloc(1, sizeof(*import));

    /* Classification of directory entries. */
    assert(roundie_import_classify("APP.RPK", 100, true, MAX_PACKAGE) == ROUNDIE_IMPORT_ACCEPT);
    assert(roundie_import_classify("a.b-c_d e.rPk", 100, true, MAX_PACKAGE) == ROUNDIE_IMPORT_ACCEPT);
    assert(roundie_import_classify("APP.RPK", 100, false, MAX_PACKAGE) == ROUNDIE_IMPORT_IGNORE);
    assert(roundie_import_classify("._APP.RPK", 100, true, MAX_PACKAGE) == ROUNDIE_IMPORT_IGNORE);
    assert(roundie_import_classify(".Trash.rpk", 100, true, MAX_PACKAGE) == ROUNDIE_IMPORT_IGNORE);
    assert(roundie_import_classify("README.TXT", 100, true, MAX_PACKAGE) == ROUNDIE_IMPORT_IGNORE);
    assert(roundie_import_classify(".RPK", 100, true, MAX_PACKAGE) == ROUNDIE_IMPORT_IGNORE);
    assert(roundie_import_classify("RPK", 100, true, MAX_PACKAGE) == ROUNDIE_IMPORT_IGNORE);
    assert(roundie_import_classify("a/b.rpk", 100, true, MAX_PACKAGE) == ROUNDIE_IMPORT_IGNORE);
    assert(roundie_import_classify("zażółć.rpk", 100, true, MAX_PACKAGE) == ROUNDIE_IMPORT_IGNORE);
    assert(roundie_import_classify("a.rpk", 0, true, MAX_PACKAGE) == ROUNDIE_IMPORT_EMPTY);
    assert(roundie_import_classify("a.rpk", MAX_PACKAGE + 1, true, MAX_PACKAGE) == ROUNDIE_IMPORT_TOO_LARGE);
    assert(roundie_import_classify("a.rpk", MAX_PACKAGE, true, MAX_PACKAGE) == ROUNDIE_IMPORT_ACCEPT);

    /* A valid package: hash, stage, confirm, install, and the file is removed. */
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("HELLO.RPK", 3000, true, "hello", 1);
    add_file("README.TXT", 50, true, NULL, 0);
    add_file("Folder", 0, false, NULL, 0);
    int64_t now = 0;
    assert(roundie_import_start(import) && !roundie_import_start(import));
    run(import, &now, 1, 0);
    assert(import->status.phase == ROUNDIE_IMPORT_DONE && import->status.total == 1 && import->status.ignored == 2);
    assert(import->status.results_count == 1 && import->status.results[0].result == ROUNDIE_IMPORT_RESULT_INSTALLED);
    assert(strcmp(import->status.results[0].name, "hello") == 0 && import->status.results[0].version[0] == 1);
    assert(manager.begins == 1 && manager.finishes == 1 && manager.installs == 1 && manager.aborts == 0);
    assert(files[0].removed && !files[1].removed && open_count == 2 && close_count == 2);
    assert(max_reads_per_step <= ROUNDIE_IMPORT_SLICE_CHUNKS); /* the worker is never held long */

    /* A package that is already installed is reported as an update. */
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    snprintf(manager.installed_ids[0], 64, "org.test.hello");
    manager.installed_count = 1;
    add_file("HELLO.RPK", 1000, true, "hello", 2);
    now = 0;
    roundie_import_start(import);
    run(import, &now, 1, 0);
    assert(import->status.results[0].result == ROUNDIE_IMPORT_RESULT_UPDATED);

    /* The user skips; the session is aborted; the file stays for a later try. */
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("A.RPK", 800, true, "a", 1);
    now = 0;
    roundie_import_start(import);
    run(import, &now, 2, S(3));
    assert(import->status.results[0].result == ROUNDIE_IMPORT_RESULT_SKIPPED);
    assert(manager.aborts == 1 && manager.installs == 0 && !files[0].removed);

    /* No answer in time: aborted, reported as a timeout, before PackageManager's READY window. */
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("A.RPK", 800, true, "a", 1);
    now = 0;
    roundie_import_start(import);
    run(import, &now, 0, 0);
    assert(import->status.results[0].result == ROUNDIE_IMPORT_RESULT_TIMEOUT && manager.aborts == 1);
    assert(ROUNDIE_IMPORT_CONFIRM_US < S(60));

    /* A late answer after the deadline does nothing. */
    roundie_import_decide(import, true);
    assert(import->decision == 0 || import->status.phase == ROUNDIE_IMPORT_DONE);

    /* Rejections without staging: empty, too large, not a package. */
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("EMPTY.RPK", 0, true, NULL, 0);
    add_file("BIG.RPK", MAX_PACKAGE + 1, true, NULL, 0);
    add_file("JUNK.RPK", 400, true, NULL, 0);
    now = 0;
    roundie_import_start(import);
    run(import, &now, 1, 0);
    assert(import->status.results_count == 3 && manager.begins == 0);
    assert(import->status.results[0].result == ROUNDIE_IMPORT_RESULT_EMPTY);
    assert(import->status.results[1].result == ROUNDIE_IMPORT_RESULT_TOO_LARGE);
    assert(import->status.results[2].result == ROUNDIE_IMPORT_RESULT_BAD_PACKAGE);
    assert(strcmp(import->status.results[2].reason, "not a package") == 0);

    /* PackageManager refusals at each stage; the session is aborted exactly when one is open. */
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("A.RPK", 800, true, "a", 1);
    manager.fail_begin = true;
    now = 0;
    roundie_import_start(import);
    run(import, &now, 1, 0);
    assert(import->status.results[0].result == ROUNDIE_IMPORT_RESULT_REFUSED && manager.aborts == 0);
    assert(strcmp(import->status.results[0].reason, "manager said no") == 0);

    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("A.RPK", 2000, true, "a", 1);
    manager.fail_write = true;
    now = 0;
    roundie_import_start(import);
    run(import, &now, 1, 0);
    assert(import->status.results[0].result == ROUNDIE_IMPORT_RESULT_REFUSED && manager.aborts == 1);
    assert(open_index < 0);

    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("A.RPK", 800, true, "a", 1);
    manager.fail_finish = true;
    now = 0;
    roundie_import_start(import);
    run(import, &now, 1, 0);
    assert(import->status.results[0].result == ROUNDIE_IMPORT_RESULT_BAD_PACKAGE && manager.aborts == 1);

    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("A.RPK", 800, true, "a", 1);
    manager.fail_install = true; /* e.g. the version is not higher */
    now = 0;
    roundie_import_start(import);
    run(import, &now, 1, 0);
    assert(import->status.results[0].result == ROUNDIE_IMPORT_RESULT_REFUSED && manager.aborts == 1);
    assert(!files[0].removed);

    /* The volume vanishes mid-read: abort, a read error, and the next file is tried. */
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("A.RPK", 4000, true, "a", 1);
    now = 0;
    read_budget = 4000 + 1500; /* the first pass completes; the second dies part way */
    roundie_import_start(import);
    run(import, &now, 1, 0);
    assert(import->status.results[0].result == ROUNDIE_IMPORT_RESULT_READ_ERROR);
    assert(manager.aborts == 1 && open_index < 0 && !files[0].removed);

    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("A.RPK", 4000, true, "a", 1);
    now = 0;
    read_budget = 1000; /* dies while hashing, before any session exists */
    roundie_import_start(import);
    run(import, &now, 1, 0);
    assert(import->status.results[0].result == ROUNDIE_IMPORT_RESULT_READ_ERROR);
    assert(manager.begins == 0 && manager.aborts == 0 && open_index < 0);

    /* The list itself fails: a run-level error, nothing staged. */
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    list_fails = true;
    now = 0;
    roundie_import_start(import);
    run(import, &now, 1, 0);
    assert(import->status.phase == ROUNDIE_IMPORT_DONE && import->status.error[0] != '\0' && manager.begins == 0);

    /* At most eight candidates per run; the others are counted, not processed. */
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    for (unsigned index = 0; index < 10; index++) {
        char name[32];
        snprintf(name, sizeof(name), "P%u.RPK", index);
        add_file(name, 500, true, "p", 1 + index);
    }
    now = 0;
    roundie_import_start(import);
    run(import, &now, 1, 0);
    assert(import->status.total == 8 && import->status.over_limit == 2 && manager.installs == 8);

    /* Cancel in the middle of a transfer (the host took the volume back). */
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("A.RPK", 6000, true, "a", 1);
    add_file("B.RPK", 600, true, "b", 1);
    now = 0;
    roundie_import_start(import);
    for (unsigned count = 0; count < 6; count++) {
        roundie_import_step(import, now);
    }
    assert(roundie_import_active(import));
    roundie_import_cancel(import);
    assert(!roundie_import_step(import, now));
    assert(import->status.cancelled && import->status.phase == ROUNDIE_IMPORT_DONE && open_index < 0);
    assert(import->status.results[0].result == ROUNDIE_IMPORT_RESULT_CANCELLED && !manager.open);
    assert(manager.installs == 0);

    /* Cancel while waiting for the answer. */
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("A.RPK", 800, true, "a", 1);
    now = 0;
    roundie_import_start(import);
    while (import->status.phase != ROUNDIE_IMPORT_AWAITING_CONFIRMATION) {
        roundie_import_step(import, now);
    }
    roundie_import_cancel(import);
    roundie_import_step(import, now);
    assert(import->status.phase == ROUNDIE_IMPORT_DONE && manager.aborts == 1 && !manager.open);

    /* A second run starts clean and keeps session numbers increasing. */
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("A.RPK", 800, true, "a", 1);
    now = 0;
    roundie_import_start(import);
    run(import, &now, 1, 0);
    add_file("B.RPK", 900, true, "b", 1);
    assert(roundie_import_start(import));
    run(import, &now, 1, 0);
    assert(import->status.run == 2); /* each run has its own number */
    assert(import->status.results_count == 1 && import->status.results[0].result == ROUNDIE_IMPORT_RESULT_INSTALLED);
    assert(manager.session_count == 2 && manager.sessions[1] > manager.sessions[0]);

    /* Not ready yet: the run waits, then goes on; never ready: refused after the wait. */
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("A.RPK", 800, true, "a", 1);
    ready_now = false;
    now = 0;
    roundie_import_start(import);
    for (unsigned count = 0; count < 20; count++) {
        roundie_import_step(import, now);
        now += S(1);
    }
    assert(roundie_import_active(import) && manager.begins == 0);
    ready_now = true;
    run(import, &now, 1, 0);
    assert(import->status.results[0].result == ROUNDIE_IMPORT_RESULT_INSTALLED);
    reset();
    roundie_import_init(import, &ops, MAX_PACKAGE);
    add_file("A.RPK", 800, true, "a", 1);
    ready_now = false;
    now = 0;
    roundie_import_start(import);
    run(import, &now, 1, 0);
    assert(import->status.results[0].result == ROUNDIE_IMPORT_RESULT_REFUSED &&
           strcmp(import->status.results[0].reason, "system not ready") == 0 && manager.begins == 0);
    ready_now = true;

    reset();
    free(import);
    EVP_MD_CTX_free(hash);
    EVP_MD_CTX_free(manager.context);
    puts("storage import: classification, sequencing, refusals, vanished volume and cancel passed");
    return 0;
}
