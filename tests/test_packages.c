/* Production POSIX transaction/registry + real ELF validator, ASan/UBSan.
 * IDF policy/allocator/executor are mocked; Flash durability is not simulated. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "roundie_packages.h"
#include "psa/crypto.h"
#ifdef ROUNDIE_IO_READ
#include "fake_vfs.h"
#endif

ssize_t injected_write(int file, const void *bytes, size_t size);
int injected_sync(int file);
int injected_rename(const char *source, const char *destination);
/* manager.c reaches the VFS through the roundie_io_* helpers: inject there. */
#define roundie_io_write injected_write
#define roundie_io_fsync injected_sync
#define roundie_io_rename injected_rename
#include "../components/roundie_packages/manager.c"
#undef write
#undef fsync
#undef rename

static unsigned write_failure;
static unsigned sync_failure;
static unsigned rename_failure;
static bool allowed = true;
static bool runtime_busy;
static bool allocation_failure;
static size_t free_space = 8 * 1024 * 1024;
static unsigned allocations;
static unsigned submissions;
static uint8_t *retained;
static uint16_t executed_abi;
static int64_t clock_us = 1000;
static uint8_t package[ROUNDIE_PACKAGE_MAX];
static size_t package_size;
static unsigned groups;

static bool fault(unsigned *counter)
{
    if (*counter > 0) {
        (*counter)--;
        if (*counter == 0) {
            errno = ENOSPC;
            return true;
        }
    }
    return false;
}

ssize_t injected_write(int file, const void *bytes, size_t size)
{
    assert(!runtime_busy && allowed);
    return fault(&write_failure) ? -1 : write(file, bytes, size);
}

int injected_sync(int file)
{
    assert(!runtime_busy && allowed);
    return fault(&sync_failure) ? -1 : fsync(file);
}

int injected_rename(const char *source, const char *destination)
{
    assert(!runtime_busy && allowed);
    return fault(&rename_failure) ? -1 : rename(source, destination);
}

bool roundie_packages_allowed(void) { return allowed; }
bool roundie_packages_runtime_busy(void) { return runtime_busy; }
bool roundie_packages_capacity(size_t *out) { *out = free_space; return true; }
int64_t roundie_packages_clock(void) { return clock_us++; }
void *roundie_packages_allocate(size_t size)
{
    if (allocation_failure) {
        return NULL;
    }
    void *memory = malloc(size);
    if (memory != NULL) {
        allocations++;
    }
    return memory;
}
void roundie_packages_free(void *memory)
{
    if (memory != NULL) {
        assert(allocations > 0);
        allocations--;
        free(memory);
    }
}
int roundie_packages_execute(const char *id, uint8_t *bytes, size_t size, uint16_t abi)
{
    assert(allowed && !runtime_busy && !roundie_packages_busy() && roundie_package_id_valid(id));
    assert(size == package_size - ROUNDIE_PACKAGE_PREFIX);
    assert(memcmp(bytes, package + ROUNDIE_PACKAGE_PREFIX, size) == 0);
    retained = bytes;
    runtime_busy = true;
    executed_abi = abi;
    submissions++;
    return ESP_OK;
}

static void unload(void)
{
    assert(runtime_busy && retained != NULL);
    roundie_packages_free(retained);
    retained = NULL;
    runtime_busy = false;
    assert(allocations == 0);
}

static void hash(const void *bytes, size_t size, uint8_t out[32])
{
    size_t length;
    assert(psa_hash_compute(PSA_ALG_SHA_256, bytes, size, out, 32, &length) == PSA_SUCCESS);
    assert(length == 32);
}

static void fixture(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    package_size = fread(package, 1, sizeof(package), file);
    assert(fclose(file) == 0 && package_size > ROUNDIE_PACKAGE_PREFIX);
}

static void version(unsigned major)
{
    package[192] = (uint8_t)major;
    package[193] = 0;
}

static esp_err_t stage(void)
{
    uint8_t digest[32];
    hash(package, package_size, digest);
    assert(roundie_packages_begin(42, package_size, digest) == ESP_OK);
    assert(roundie_packages_scan() == ESP_OK);
    for (size_t offset = 0; offset < package_size; offset += 512) {
        size_t size = package_size - offset > 512 ? 512 : package_size - offset;
        esp_err_t result = roundie_packages_write(42, offset, package + offset, size);
        if (result != ESP_OK) {
            return result;
        }
    }
    return roundie_packages_finish(42);
}

static esp_err_t install(void)
{
    uint8_t digest[32];
    hash(package, package_size, digest);
    return roundie_packages_install(digest);
}

static void active_version(unsigned expected)
{
    assert(roundie_packages_scan() == ESP_OK);
    roundie_package_record_t record;
    assert(roundie_packages_find("org.roundie.hello", &record) == ESP_OK);
    assert(record.health == ROUNDIE_PACKAGE_OK && record.metadata.version[0] == expected);
}

static void group(const char *name)
{
    assert(allocations == (retained != NULL ? 1U : 0U));
    groups++;
    printf("package group %u PASS: %s\n", groups, name);
}

#ifdef ROUNDIE_IO_READ
/* Substituted VFS: end of file in the middle of a chunk must refuse, never
 * accept a prefix. Needs a package whose ELF is larger than one 4 KiB chunk. */
static void expect_reason(const char *reason)
{
    if (strcmp(status.error, reason) != 0) {
        fprintf(stderr, "expected '%s', got '%s'\n", reason, status.error);
        abort();
    }
}

static void launch_with_eof(size_t bytes, esp_err_t expected, const char *reason)
{
    unsigned before = submissions;
    fake_vfs_eof_after(bytes);
    assert(roundie_packages_launch("org.roundie.stopwatch") == expected);
    fake_vfs_disarm();
    expect_reason(reason);
    assert(submissions == before && retained == NULL && allocations == 0 && !runtime_busy);
    assert(!roundie_packages_busy());
}

static void install_with_eof(size_t bytes, const char *reason)
{
    assert(stage() == ESP_OK);
    fake_vfs_eof_after(bytes);
    assert(install() == ESP_ERR_INVALID_ARG);
    fake_vfs_disarm();
    expect_reason(reason);
    assert(!roundie_packages_busy() && access(TRANSFER, F_OK) != 0 && allocations == 0);
    assert(roundie_packages_scan() == ESP_OK && status.count == 0);
}

static void data_read_with_eof(const char *id, size_t bytes, size_t capacity, const uint8_t *expected,
                               size_t expected_length, const char *path)
{
    static uint8_t buffer[8192];
    size_t length = 1;
    fake_vfs_eof_after(bytes);
    assert(roundie_packages_data_read(id, buffer, capacity, &length) == ESP_ERR_INVALID_CRC);
    fake_vfs_disarm();
    expect_reason("data-corrupt");
    assert(length == 0 && access(path, F_OK) == 0);
    assert(roundie_packages_data_read(id, buffer, capacity, &length) == ESP_OK && length == expected_length &&
           memcmp(buffer, expected, length) == 0);
}

static void eof_cases(const char *path_elf)
{
    fixture(path_elf);
    size_t elf_size = package_size - ROUNDIE_PACKAGE_PREFIX;
    assert(elf_size > ROUNDIE_IO_CHUNK + 512);
    const char *id = "org.roundie.stopwatch";
    /* Install: the package hash loop (512 B blocks) and the ELF load (4 KiB
     * chunks) of validate_file() on the transfer file. */
    install_with_eof(20, "package-integrity-or-envelope");
    install_with_eof(ROUNDIE_PACKAGE_PREFIX + 512 + 100, "package-integrity-or-envelope");
    install_with_eof(package_size + 2000, "package-ELF-read");
    install_with_eof(package_size + ROUNDIE_IO_CHUNK + 100, "package-ELF-read");
    group("EOF mid-chunk: install hash loop and ELF load refused, nothing installed");
    assert(stage() == ESP_OK && install() == ESP_OK);
    /* launch: scan (selection + inspect), then validate_file (inspect + ELF). */
    fake_vfs_reset_read_total();
    assert(roundie_packages_launch(id) == ESP_OK);
    assert(fake_vfs_read_total() == SELECTION_SIZE + 2 * package_size + elf_size);
    unload();
    roundie_package_record_t record;
    fake_vfs_eof_after(10);
    assert(roundie_packages_scan() == ESP_OK);
    fake_vfs_disarm();
    assert(roundie_packages_find(id, &record) == ESP_OK && record.health == ROUNDIE_PACKAGE_CORRUPT);
    launch_with_eof(SELECTION_SIZE + ROUNDIE_PACKAGE_PREFIX + 512 + 100, ESP_ERR_INVALID_STATE,
                    "launch-unhealthy-package");
    launch_with_eof(SELECTION_SIZE + package_size + ROUNDIE_PACKAGE_PREFIX + 512 + 100, ESP_ERR_INVALID_ARG,
                    "package-integrity-or-envelope");
    launch_with_eof(SELECTION_SIZE + 2 * package_size + 2000, ESP_ERR_INVALID_ARG, "package-ELF-read");
    launch_with_eof(SELECTION_SIZE + 2 * package_size + ROUNDIE_IO_CHUNK + 100, ESP_ERR_INVALID_ARG,
                    "package-ELF-read");
    assert(roundie_packages_launch(id) == ESP_OK);
    unload();
    group("EOF mid-chunk: launch selection, hash loop and ELF load refused, still launchable");
    /* App data: the 512 B REP_STORAGE_CAPACITY keeps firmware reads below one
     * chunk, but the function takes a capacity: 6000 B spans two chunks. */
    static uint8_t blob[6000];
    static uint8_t other[6000];
    for (size_t i = 0; i < sizeof(blob); i++) {
        blob[i] = (uint8_t)(i * 7 + 1);
        other[i] = (uint8_t)(i * 3 + 2);
    }
    char path[PATH_SIZE];
    path_for(path, id, "data");
    assert(roundie_packages_data_write(id, blob, sizeof(blob)) == ESP_OK);
    data_read_with_eof(id, 20, sizeof(blob), blob, sizeof(blob), path);
    data_read_with_eof(id, DATA_HEADER + 2000, sizeof(blob), blob, sizeof(blob), path);
    data_read_with_eof(id, DATA_HEADER + ROUNDIE_IO_CHUNK + 100, sizeof(blob), blob, sizeof(blob), path);
    assert(roundie_packages_data_write(id, other, 400) == ESP_OK);
    data_read_with_eof(id, DATA_HEADER + 200, 512, other, 400, path);
    assert(roundie_packages_data_write(id, blob, sizeof(blob)) == ESP_OK);
    group("EOF mid-chunk: app data read refused, file kept");
    /* A write that returns 0 in the middle of a chunk keeps the old data. */
    path_for(path, id, "data.tmp");
    fake_vfs_write_fail_after(DATA_HEADER + ROUNDIE_IO_CHUNK + 100);
    assert(roundie_packages_data_write(id, other, sizeof(other)) == ESP_FAIL);
    fake_vfs_disarm();
    expect_reason("data-commit");
    assert(access(path, F_OK) != 0);
    path_for(path, id, "data");
    data_read_with_eof(id, DATA_HEADER + 2000, sizeof(blob), blob, sizeof(blob), path);
    group("write returning 0 mid-chunk: app data commit refused, old data kept");
    assert(roundie_packages_remove(id) == ESP_OK && status.count == 0 && allocations == 0);
}
#endif

int main(int argc, char **argv)
{
    assert(argc == 4 || argc == 5);
    fixture(argv[1]);
    roundie_package_metadata_t decoded;
    char error[ROUNDIE_ELF_ERROR_SIZE];
    assert(roundie_package_decode(package, package_size, package_size, &decoded, error));
    assert(roundie_package_id_valid("org.roundie.hello"));
    static const char *const bad_ids[] = {"", "x", "a.", ".a", "a..b", "../x", "a/b", "a\\b", "a.B", "a.1", "a.b\n"};
    for (unsigned i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); i++) {
        assert(!roundie_package_id_valid(bad_ids[i]));
    }
    group("canonical ID grammar/traversal");
    uint8_t original[ROUNDIE_PACKAGE_PREFIX];
    memcpy(original, package, sizeof(original));
    const unsigned header_bytes[] = {0, 4, 6, 8, 12, 16, 20, 56, 127, 191, 218};
    for (unsigned i = 0; i < sizeof(header_bytes) / sizeof(header_bytes[0]); i++) {
        package[header_bytes[i]] ^= 0xff;
        assert(!roundie_package_decode(package, package_size, package_size, &decoded, error));
        memcpy(package, original, sizeof(original));
    }
    assert(!roundie_package_decode(package, 0, package_size, &decoded, error));
    assert(!roundie_package_decode(package, 63, package_size, &decoded, error));
    assert(!roundie_package_decode(package, package_size, SIZE_MAX, &decoded, error));
    assert(!roundie_package_decode(package, package_size, package_size - 1, &decoded, error));
    group("header/version/reserved/manifest/overflow/truncation");
    assert(roundie_packages_scan() == ESP_OK && status.count == 0);
    assert(submissions == 0);
    group("empty registry/no autostart");
    uint8_t digest[32];
    hash(package, package_size, digest);
    assert(roundie_packages_begin(0, package_size, digest) != ESP_OK);
    assert(roundie_packages_begin(42, ROUNDIE_PACKAGE_MAX + 1, digest) != ESP_OK);
    free_space = ROUNDIE_PACKAGES_RESERVE;
    assert(roundie_packages_begin(42, package_size, digest) != ESP_OK);
    free_space = 8 * 1024 * 1024;
    group("package/storage limits and reserve");
    assert(roundie_packages_begin(42, package_size, digest) == ESP_OK);
    assert(roundie_packages_write(41, 0, package, 1) != ESP_OK);
    assert(roundie_packages_write(42, 1, package, 1) != ESP_OK);
    assert(roundie_packages_write(42, 0, package, 513) != ESP_OK);
    assert(roundie_packages_finish(42) != ESP_OK);
    assert(roundie_packages_scan() == ESP_OK && status.count == 0);
    assert(roundie_packages_abort(42) == ESP_OK);
    group("offset/session/incomplete transfer invisible/abort");
    assert(roundie_packages_begin(42, package_size, digest) == ESP_OK);
    roundie_packages_poll(clock_us + 10000001);
    assert(!roundie_packages_busy() && access(TRANSFER, F_OK) != 0);
    group("transfer timeout");
    assert(roundie_packages_begin(42, package_size, digest) == ESP_OK);
    allowed = false;
    roundie_packages_poll(clock_us);
    assert(!roundie_packages_busy());
    assert(roundie_packages_scan() != ESP_OK);
    assert(roundie_packages_launch("org.roundie.hello") != ESP_OK);
    allowed = true;
    group("Safe Mode cancellation and admission");
    write_failure = 1;
    assert(stage() != ESP_OK && !roundie_packages_busy());
    group("ENOSPC/write failure");
    sync_failure = 1;
    assert(stage() != ESP_OK && !roundie_packages_busy());
    group("transfer fsync failure");
    allocation_failure = true;
    assert(stage() != ESP_OK && allocations == 0);
    allocation_failure = false;
    group("validation allocation failure");
    package[24] ^= 1;
    assert(stage() != ESP_OK && submissions == 0);
    package[24] ^= 1;
    package[198] = 2;
    assert(stage() != ESP_OK && submissions == 0);
    package[198] = 1;
    package[200] = 'x';
    assert(stage() != ESP_OK);
    package[200] = 'e';
    group("ELF digest/ABI/target rejection before execution");
    assert(stage() == ESP_OK && status.phase == ROUNDIE_PACKAGES_READY);
    assert(status.count == 0);
    assert(install() == ESP_OK && !roundie_packages_busy());
    active_version(1);
    group("validated install and activation");
    assert(roundie_packages_launch("org.roundie.hello") == ESP_OK && executed_abi == 1);
    assert(allocations == 1);
    /* All loaded states share the same reservation. Simulate RUNNING,
     * SUSPENDED and STOPPED ownership, not merely a RUNNING state check. */
    for (unsigned state = 0; state < 3; state++) {
        assert(roundie_packages_begin(42, package_size, digest) != ESP_OK);
        assert(roundie_packages_install(digest) != ESP_OK);
        assert(roundie_packages_remove("org.roundie.hello") != ESP_OK);
        assert(roundie_packages_launch("org.roundie.hello") != ESP_OK);
    }
    unload();
    active_version(1);
    group("launch buffer ownership/single instance/loaded Flash exclusion/persistent unload");
    version(2);
    assert(stage() == ESP_OK);
    rename_failure = 1;
    assert(install() != ESP_OK);
    active_version(1);
    group("artifact rename failure preserves active version");
    assert(stage() == ESP_OK);
    rename_failure = 2;
    assert(install() != ESP_OK);
    active_version(1);
    assert(roundie_packages_launch("org.roundie.hello") == ESP_OK);
    unload();
    group("selection commit failure preserves launchable version/orphan ignored");
    assert(stage() == ESP_OK);
    sync_failure = 1;
    assert(install() != ESP_OK);
    active_version(1);
    group("selection fsync failure preserves previous version");
    assert(stage() == ESP_OK && install() == ESP_OK);
    active_version(2);
    group("transactional replacement");
    version(1);
    assert(stage() == ESP_OK && install() != ESP_OK);
    assert(roundie_packages_abort(42) == ESP_OK);
    active_version(2);
    version(2);
    assert(stage() == ESP_OK && install() != ESP_OK);
    assert(roundie_packages_abort(42) == ESP_OK);
    group("downgrade and duplicate version policy");
    package[ROUNDIE_PACKAGE_PREFIX] = 0;
    hash(package + ROUNDIE_PACKAGE_PREFIX, package_size - ROUNDIE_PACKAGE_PREFIX, package + 24);
    assert(stage() != ESP_OK);
    active_version(2);
    group("non-executable failed replacement preserves original");
    fixture(argv[2]);
    assert(stage() == ESP_OK && install() == ESP_OK);
    assert(roundie_packages_launch("org.roundie.events") == ESP_OK && executed_abi == 2);
    unload();
    fixture(argv[3]);
    assert(stage() == ESP_OK && install() == ESP_OK);
    assert(roundie_packages_launch("org.roundie.ui") == ESP_OK && executed_abi == 3);
    unload();
    assert(roundie_packages_scan() == ESP_OK && status.count == 3);
    group("multiple packages/ABI 2 and 3 submission");
    /* Reconstruct all metadata from disk with no persisted host cache. */
    memset(registry, 0, sizeof(registry));
    status.count = 0;
    assert(roundie_packages_scan() == ESP_OK && status.count == 3);
    roundie_package_record_t record;
    assert(roundie_packages_get(3, &record) == ESP_ERR_NOT_FOUND);
    assert(roundie_packages_find("org.roundie.ui", &record) == ESP_OK);
    char path[PATH_SIZE];
    path_for(path, "org.roundie.ui", "0.rpk");
    int file = open(path, O_WRONLY);
    assert(file >= 0 && write(file, "X", 1) == 1 && close(file) == 0);
    assert(roundie_packages_scan() == ESP_OK);
    assert(roundie_packages_find("org.roundie.ui", &record) == ESP_OK && record.health == ROUNDIE_PACKAGE_CORRUPT);
    unsigned before = submissions;
    assert(roundie_packages_launch("org.roundie.ui") != ESP_OK && submissions == before);
    group("registry reconstruction/stale metadata/corruption launch denial");
    assert(roundie_packages_remove("org.roundie.ui") == ESP_OK);
    assert(roundie_packages_find("org.roundie.ui", &record) == ESP_ERR_NOT_FOUND);
    assert(roundie_packages_launch("org.roundie.ui") != ESP_OK);
    assert(status.count == 2);
    assert(roundie_packages_remove("org.roundie.ui") == ESP_OK);
    active_version(2);
    group("scoped idempotent removal/other applications preserved");
    /* App storage: atomic replace, verified read, corruption left in place. */
    uint8_t stored[512];
    size_t stored_length = 1;
    assert(roundie_packages_data_read("org.roundie.events", stored, sizeof(stored), &stored_length) == ESP_OK &&
           stored_length == 0);
    assert(roundie_packages_data_write("org.roundie.events", "\x01\x02", 2) == ESP_OK);
    assert(roundie_packages_data_read("org.roundie.events", stored, sizeof(stored), &stored_length) == ESP_OK &&
           stored_length == 2 && memcmp(stored, "\x01\x02", 2) == 0);
    assert(roundie_packages_data_read("org.roundie.events", stored, 1, &stored_length) == ESP_ERR_INVALID_CRC &&
           stored_length == 0);
    rename_failure = 1;
    assert(roundie_packages_data_write("org.roundie.events", "\x03\x04", 2) == ESP_FAIL);
    assert(roundie_packages_data_read("org.roundie.events", stored, sizeof(stored), &stored_length) == ESP_OK &&
           stored_length == 2 && memcmp(stored, "\x01\x02", 2) == 0);
    path_for(path, "org.roundie.events", "data.tmp");
    assert(access(path, F_OK) != 0);
    assert(roundie_packages_data_write("org.roundie.missing", "\x01", 1) == ESP_ERR_INVALID_STATE);
    runtime_busy = true;
    assert(roundie_packages_data_write("org.roundie.events", "\x01", 1) == ESP_ERR_INVALID_STATE);
    runtime_busy = false;
    path_for(path, "org.roundie.events", "data");
    FILE *data = fopen(path, "r+b");
    assert(data != NULL && fseek(data, 41, SEEK_SET) == 0 && fputc(0xff, data) == 0xff && fclose(data) == 0);
    assert(roundie_packages_data_read("org.roundie.events", stored, sizeof(stored), &stored_length) ==
           ESP_ERR_INVALID_CRC && stored_length == 0 && access(path, F_OK) == 0);
    assert(roundie_packages_data_write("org.roundie.events", "", 0) == ESP_OK && access(path, F_OK) != 0);
    assert(roundie_packages_data_write("org.roundie.events", "\x05", 1) == ESP_OK && access(path, F_OK) == 0);
    group("app storage: replace, interrupted replace, corruption kept, admission");
    /* No active record: never choose a random orphan package. */
    path_for(path, "org.roundie.events", "active");
    assert(unlink(path) == 0);
    assert(roundie_packages_scan() == ESP_OK && status.count == 1 && status.orphans == 1);
    assert(roundie_packages_launch("org.roundie.events") != ESP_OK);
    assert(roundie_packages_remove("org.roundie.events") == ESP_OK);
    path_for(path, "org.roundie.events", "");
    assert(access(path, F_OK) != 0); /* the data file went with the package */
    group("missing selection fails closed/explicit orphan cleanup");
    fixture(argv[1]);
    assert(roundie_packages_begin(42, package_size, digest) == ESP_OK);
    /* Simulate a process restart with an unsynced incomplete transfer. */
    assert(close(transfer) == 0);
    transfer = -1;
    status.phase = ROUNDIE_PACKAGES_IDLE;
    assert(roundie_packages_scan() == ESP_OK && status.count == 1);
    assert(roundie_packages_begin(42, package_size, digest) == ESP_OK);
    assert(roundie_packages_abort(42) == ESP_OK);
    group("interrupted transfer restart/explicit bounded cleanup");
    /* Pin a future ABI manifest to emulate packages preserved after rollback.
     * Discovery reports incompatibility without deleting or executing bytes. */
    path_for(path, "org.roundie.hello", "active");
    uint8_t selection[SELECTION_SIZE];
    assert(read_selection("org.roundie.hello", selection));
    path_for(path, "org.roundie.hello", selection[4] == 0 ? "0.rpk" : "1.rpk");
    FILE *future = fopen(path, "r+b");
    assert(future != NULL);
    package_size = fread(package, 1, sizeof(package), future);
    package[198] = 4;
    rewind(future);
    assert(fwrite(package, 1, package_size, future) == package_size && fclose(future) == 0);
    hash(package, package_size, selection + 8);
    path_for(path, "org.roundie.hello", "active");
    file = open(path, O_WRONLY | O_TRUNC);
    assert(file >= 0 && write(file, selection, sizeof(selection)) == sizeof(selection) && close(file) == 0);
    assert(roundie_packages_scan() == ESP_OK);
    assert(roundie_packages_find("org.roundie.hello", &record) == ESP_OK && record.health == ROUNDIE_PACKAGE_INCOMPATIBLE);
    assert(roundie_packages_launch("org.roundie.hello") != ESP_OK);
    group("firmware rollback/incompatible ABI retained and reported");
    assert(roundie_packages_remove("org.roundie.hello") == ESP_OK);
    assert(status.count == 0 && allocations == 0 && !roundie_packages_busy());
    fixture(argv[1]);
    for (unsigned i = 0; i < ROUNDIE_PACKAGES_MAX; i++) {
        char id[64];
        snprintf(id, sizeof(id), "org.roundie.p%c", 'a' + i);
        memset(package + 64, 0, 64);
        memcpy(package + 64, id, strlen(id));
        assert(stage() == ESP_OK && install() == ESP_OK);
    }
    memset(package + 64, 0, 64);
    memcpy(package + 64, "org.roundie.extra", 17);
    assert(stage() == ESP_OK && install() != ESP_OK);
    assert(roundie_packages_abort(42) == ESP_OK);
    assert(status.count == ROUNDIE_PACKAGES_MAX);
    for (unsigned i = 0; i < ROUNDIE_PACKAGES_MAX; i++) {
        char id[64];
        snprintf(id, sizeof(id), "org.roundie.p%c", 'a' + i);
        assert(roundie_packages_remove(id) == ESP_OK);
    }
    group("bounded full registry/ninth application rejected");
    fixture(argv[1]);
    /* Import rejection and relocation rejection exercise the same production
     * preflight through package validation, after resealing payload integrity. */
    const char *symbol = "rep_abi_v1";
    uint8_t *import = NULL;
    for (size_t i = ROUNDIE_PACKAGE_PREFIX; i + strlen(symbol) < package_size; i++) {
        if (memcmp(package + i, symbol, strlen(symbol) + 1) == 0) {
            import = package + i;
            break;
        }
    }
    assert(import != NULL);
    import[0] = 'x';
    hash(package + ROUNDIE_PACKAGE_PREFIX, package_size - ROUNDIE_PACKAGE_PREFIX, package + 24);
    assert(stage() != ESP_OK && strstr(status.error, "import:") != NULL);
    group("unsupported imports rejected inside package validation");
    fixture(argv[1]);
    /* ELF32 LE section table: replace an actual RELA type by unsupported 255. */
    uint8_t *elf = package + ROUNDIE_PACKAGE_PREFIX;
    uint32_t sections;
    memcpy(&sections, elf + 32, 4);
    uint16_t section_count;
    memcpy(&section_count, elf + 48, 2);
    bool changed = false;
    for (unsigned i = 0; i < section_count; i++) {
        uint32_t type;
        uint32_t offset;
        memcpy(&type, elf + sections + i * 40 + 4, 4);
        memcpy(&offset, elf + sections + i * 40 + 16, 4);
        if (type == 4) {
            elf[offset + 4] = 255;
            changed = true;
            break;
        }
    }
    assert(changed);
    hash(elf, package_size - ROUNDIE_PACKAGE_PREFIX, package + 24);
    assert(stage() != ESP_OK && strstr(status.error, "relocation") != NULL);
    group("unsupported relocations rejected inside package validation");
#ifdef ROUNDIE_IO_READ
    assert(argc == 5);
    eof_cases(argv[4]);
#endif
    printf("Production packages PASS: %u scenario groups; no native instructions executed on host\n", groups);
}
