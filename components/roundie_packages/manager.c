#include "roundie_packages.h"
#include "platform.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "psa/crypto.h"

#ifndef PACKAGES_DIRECTORY
#define PACKAGES_DIRECTORY "/storage/packages"
#endif
#define TRANSFER PACKAGES_DIRECTORY "/.transfer"
#define PATH_SIZE (sizeof(PACKAGES_DIRECTORY) + ROUNDIE_PACKAGE_ID + 24)
#define SELECTION_SIZE 40U

static roundie_packages_status_t status;
static roundie_package_record_t registry[ROUNDIE_PACKAGES_MAX];
static int transfer = -1;
static uint8_t expected_digest[32];
static int64_t deadline;
static int64_t upload_started;

static esp_err_t failed(esp_err_t error, const char *reason)
{
    snprintf(status.error, sizeof(status.error), "%s", reason);
    return error;
}

static bool admitted(void)
{
    return roundie_packages_allowed() && !roundie_packages_runtime_busy();
}

static bool path_for(char out[PATH_SIZE], const char *id, const char *name)
{
    if (!roundie_package_id_valid(id)) {
        return false;
    }
    int length = snprintf(out, PATH_SIZE, "%s/%s/%s", PACKAGES_DIRECTORY, id, name);
    return length > 0 && (size_t)length < PATH_SIZE;
}

static bool read_exact(int file, void *out, size_t size)
{
    uint8_t *bytes = out;
    while (size > 0) {
        ssize_t count = read(file, bytes, size);
        if (count <= 0) {
            return false;
        }
        size -= (size_t)count;
        bytes += count;
    }
    return true;
}

static bool read_selection(const char *id, uint8_t selection[SELECTION_SIZE])
{
    char path[PATH_SIZE];
    if (!path_for(path, id, "active")) {
        return false;
    }
    int file = open(path, O_RDONLY);
    if (file < 0) {
        return false;
    }
    struct stat info;
    bool valid = fstat(file, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_size == SELECTION_SIZE && read_exact(file, selection, SELECTION_SIZE);
    int closed = close(file);
    return valid && closed == 0 && memcmp(selection, "RPA1", 4) == 0 &&
        selection[4] <= 1 && selection[5] == 0 && selection[6] == 0 && selection[7] == 0;
}

/* Streaming integrity for discovery: no ELF input buffer, loader or executor.
 * The selection pins ALL bytes, including manifest, to the validated commit. */
static bool inspect_file(const char *path, const uint8_t digest[32],
                         roundie_package_metadata_t *metadata)
{
    int file = open(path, O_RDONLY);
    if (file < 0) {
        return false;
    }
    uint8_t prefix[ROUNDIE_PACKAGE_PREFIX];
    struct stat info;
    bool valid = fstat(file, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
        info.st_size <= ROUNDIE_PACKAGE_MAX && read_exact(file, prefix, sizeof(prefix)) &&
        roundie_package_decode(prefix, sizeof(prefix), (size_t)info.st_size, metadata, status.error);
    psa_hash_operation_t hash = psa_hash_operation_init();
    if (valid) {
        valid = psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS &&
            psa_hash_update(&hash, prefix, sizeof(prefix)) == PSA_SUCCESS;
    }
    uint8_t block[512];
    size_t remaining = valid ? (size_t)info.st_size - sizeof(prefix) : 0;
    while (valid && remaining > 0) {
        size_t size = remaining > sizeof(block) ? sizeof(block) : remaining;
        valid = read_exact(file, block, size) && psa_hash_update(&hash, block, size) == PSA_SUCCESS;
        remaining -= size;
    }
    uint8_t actual[32];
    size_t length = 0;
    if (valid) {
        valid = psa_hash_finish(&hash, actual, sizeof(actual), &length) == PSA_SUCCESS &&
            length == 32 && memcmp(actual, digest, 32) == 0;
    }
    psa_hash_abort(&hash);
    int closed = close(file);
    return valid && closed == 0;
}

/* Validation returns a separately owned ELF buffer. It never retains package
 * bytes or passes a pointer into an envelope to the executor. */
static uint8_t *validate_file(const char *path, const uint8_t digest[32],
                              roundie_package_metadata_t *metadata)
{
    int64_t started = roundie_packages_clock();
    uint8_t *elf = NULL;
    if (!inspect_file(path, digest, metadata)) {
        failed(ESP_ERR_INVALID_ARG, "package-integrity-or-envelope");
        goto done;
    }
    if (!roundie_package_compatible(metadata)) {
        failed(ESP_ERR_NOT_SUPPORTED, "package-incompatible");
        goto done;
    }
    int file = open(path, O_RDONLY);
    if (file < 0) {
        failed(ESP_FAIL, "package-open");
        goto done;
    }
    elf = roundie_packages_allocate(metadata->elf_size);
    bool valid = elf != NULL && lseek(file, ROUNDIE_PACKAGE_PREFIX, SEEK_SET) == ROUNDIE_PACKAGE_PREFIX &&
        read_exact(file, elf, metadata->elf_size);
    int closed = close(file);
    uint8_t actual[32];
    size_t length = 0;
    roundie_elf_info_t info;
    if (!valid || closed != 0) {
        failed(ESP_FAIL, "package-ELF-read");
        valid = false;
    } else if (psa_hash_compute(PSA_ALG_SHA_256, elf, metadata->elf_size, actual, sizeof(actual), &length) != PSA_SUCCESS ||
               length != 32 || memcmp(actual, metadata->elf_digest, 32) != 0) {
        failed(ESP_ERR_INVALID_ARG, "package-ELF-digest");
        valid = false;
    } else if (!roundie_elf_validate(elf, metadata->elf_size, &info, status.error)) {
        valid = false;
    } else if (info.abi_version != metadata->abi) {
        failed(ESP_ERR_INVALID_ARG, "package-ELF-ABI");
        valid = false;
    }
    if (!valid) {
        roundie_packages_free(elf);
        elf = NULL;
    }
done:
    status.validation_us = (uint32_t)(roundie_packages_clock() - started);
    return elf;
}

bool roundie_packages_busy(void)
{
    return status.phase != ROUNDIE_PACKAGES_IDLE;
}

void roundie_packages_status(roundie_packages_status_t *out)
{
    *out = status;
}

static esp_err_t cancel(void)
{
    int closed = transfer >= 0 ? close(transfer) : 0;
    transfer = -1;
    int removed = unlink(TRANSFER);
    bool okay = closed == 0 && (removed == 0 || errno == ENOENT);
    status.phase = ROUNDIE_PACKAGES_IDLE;
    status.session = 0;
    status.received = 0;
    status.total = 0;
    return okay ? ESP_OK : failed(ESP_FAIL, "temporary-cleanup");
}

void roundie_packages_poll(int64_t now_us)
{
    if (roundie_packages_busy() && (!roundie_packages_allowed() || now_us >= deadline) &&
        !roundie_packages_runtime_busy()) {
        cancel();
        failed(ESP_ERR_INVALID_STATE, "transfer-cancelled-or-expired");
    }
}

esp_err_t roundie_packages_begin(uint32_t session, size_t size, const uint8_t digest[32])
{
    if (!admitted() || roundie_packages_busy()) {
        return failed(ESP_ERR_INVALID_STATE, "package-admission");
    }
    if (session == 0 || size < ROUNDIE_PACKAGE_PREFIX + 52 || size > ROUNDIE_PACKAGE_MAX) {
        return failed(ESP_ERR_INVALID_SIZE, "package-size-or-session");
    }
    size_t free_bytes;
    if (!roundie_packages_capacity(&free_bytes) || free_bytes < ROUNDIE_PACKAGES_RESERVE + size + 16384) {
        return failed(ESP_ERR_NO_MEM, "package-storage-reserve");
    }
    if (mkdir(PACKAGES_DIRECTORY, 0700) != 0 && errno != EEXIST) {
        return failed(ESP_FAIL, "package-directory");
    }
    /* Explicit BEGIN retires only the one known interrupted transfer. */
    if (unlink(TRANSFER) != 0 && errno != ENOENT) {
        return failed(ESP_FAIL, "temporary-cleanup");
    }
    transfer = open(TRANSFER, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (transfer < 0) {
        return failed(ESP_FAIL, "transfer-open");
    }
    status.phase = ROUNDIE_PACKAGES_RECEIVING;
    status.session = session;
    status.received = 0;
    status.total = size;
    status.error[0] = '\0';
    memcpy(expected_digest, digest, 32);
    upload_started = roundie_packages_clock();
    deadline = upload_started + 10000000;
    return ESP_OK;
}

esp_err_t roundie_packages_write(uint32_t session, size_t offset, const void *data, size_t size)
{
    if (!admitted() || transfer < 0 || status.phase != ROUNDIE_PACKAGES_RECEIVING || session != status.session) {
        return failed(ESP_ERR_INVALID_STATE, "transfer-state");
    }
    if (offset != status.received || size == 0 || size > 512 || size > status.total - status.received) {
        return failed(ESP_ERR_INVALID_SIZE, "transfer-offset-or-length");
    }
    if (write(transfer, data, size) != (ssize_t)size) {
        cancel();
        return failed(ESP_FAIL, "transfer-write");
    }
    status.received += size;
    deadline = roundie_packages_clock() + 10000000;
    return ESP_OK;
}

esp_err_t roundie_packages_finish(uint32_t session)
{
    if (!admitted() || transfer < 0 || session != status.session) {
        return failed(ESP_ERR_INVALID_STATE, "transfer-state");
    }
    if (status.received != status.total) {
        return failed(ESP_ERR_INVALID_SIZE, "transfer-incomplete");
    }
    int synced = fsync(transfer);
    int closed = close(transfer);
    transfer = -1;
    status.upload_us = (uint32_t)(roundie_packages_clock() - upload_started);
    status.phase = ROUNDIE_PACKAGES_STAGED;
    if (synced != 0 || closed != 0) {
        cancel();
        return failed(ESP_FAIL, "transfer-sync");
    }
    status.phase = ROUNDIE_PACKAGES_VALIDATING;
    roundie_package_metadata_t metadata;
    uint8_t *elf = validate_file(TRANSFER, expected_digest, &metadata);
    if (elf == NULL) {
        cancel();
        return ESP_ERR_INVALID_ARG;
    }
    roundie_packages_free(elf);
    status.phase = ROUNDIE_PACKAGES_READY;
    deadline = roundie_packages_clock() + 60000000;
    status.error[0] = '\0';
    return ESP_OK;
}

esp_err_t roundie_packages_abort(uint32_t session)
{
    if (!roundie_packages_busy() || session != status.session || roundie_packages_runtime_busy()) {
        return failed(ESP_ERR_INVALID_STATE, "transfer-state");
    }
    return cancel();
}

static int compare_records(const void *left, const void *right)
{
    const roundie_package_record_t *a = left;
    const roundie_package_record_t *b = right;
    return strcmp(a->metadata.id, b->metadata.id);
}

esp_err_t roundie_packages_scan(void)
{
    if (!roundie_packages_allowed()) {
        return failed(ESP_ERR_INVALID_STATE, "registry-admission");
    }
    int64_t started = roundie_packages_clock();
    status.count = 0;
    status.orphans = 0;
    DIR *directory = opendir(PACKAGES_DIRECTORY);
    if (directory == NULL) {
        status.enumeration_us = (uint32_t)(roundie_packages_clock() - started);
        if (errno == ENOENT) {
            status.revision++;
        }
        return errno == ENOENT ? ESP_OK : failed(ESP_FAIL, "registry-open");
    }
    esp_err_t result = ESP_OK;
    unsigned visited = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (++visited > 32) {
            result = failed(ESP_ERR_INVALID_SIZE, "registry-directory-limit");
            break;
        }
        if (!roundie_package_id_valid(entry->d_name)) {
            continue;
        }
        char path[PATH_SIZE];
        path_for(path, entry->d_name, "active");
        struct stat info;
        if (stat(path, &info) != 0 && errno == ENOENT) {
            status.orphans++;
            errno = 0;
            continue;
        }
        if (status.count == ROUNDIE_PACKAGES_MAX) {
            result = failed(ESP_ERR_INVALID_SIZE, "registry-count-limit");
            break;
        }
        roundie_package_record_t *record = &registry[status.count++];
        *record = (roundie_package_record_t){.health = ROUNDIE_PACKAGE_CORRUPT};
        memcpy(record->metadata.id, entry->d_name, strlen(entry->d_name) + 1);
        uint8_t selection[SELECTION_SIZE];
        if (read_selection(entry->d_name, selection)) {
            record->slot = selection[4];
            memcpy(record->digest, selection + 8, 32);
            path_for(path, entry->d_name, record->slot == 0 ? "0.rpk" : "1.rpk");
            roundie_package_metadata_t metadata;
            if (inspect_file(path, record->digest, &metadata) && strcmp(metadata.id, entry->d_name) == 0) {
                record->metadata = metadata;
                record->health = roundie_package_compatible(&metadata) ?
                    ROUNDIE_PACKAGE_OK : ROUNDIE_PACKAGE_INCOMPATIBLE;
            }
        }
        errno = 0;
    }
    if (errno != 0 && result == ESP_OK) {
        result = failed(ESP_FAIL, "registry-read");
    }
    if (closedir(directory) != 0) {
        result = failed(ESP_FAIL, "registry-close");
    }
    if (result != ESP_OK) {
        status.count = 0; /* Never return an arbitrary prefix after overflow. */
    }
    qsort(registry, status.count, sizeof(registry[0]), compare_records);
    status.enumeration_us = (uint32_t)(roundie_packages_clock() - started);
    if (result == ESP_OK) {
        status.revision++;
    }
    return result;
}

esp_err_t roundie_packages_get(unsigned index, roundie_package_record_t *out)
{
    if (index >= status.count) {
        return ESP_ERR_NOT_FOUND;
    }
    *out = registry[index];
    return ESP_OK;
}

esp_err_t roundie_packages_find(const char *id, roundie_package_record_t *out)
{
    int64_t started = roundie_packages_clock();
    esp_err_t result = ESP_ERR_NOT_FOUND;
    if (!roundie_package_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (unsigned i = 0; i < status.count; i++) {
        if (strcmp(registry[i].metadata.id, id) == 0) {
            *out = registry[i];
            result = ESP_OK;
            break;
        }
    }
    status.lookup_us = (uint32_t)(roundie_packages_clock() - started);
    return result;
}

esp_err_t roundie_packages_install(const uint8_t digest[32])
{
    if (!admitted() || status.phase != ROUNDIE_PACKAGES_READY || memcmp(digest, expected_digest, 32) != 0) {
        return failed(ESP_ERR_INVALID_STATE, "install-admission-or-digest");
    }
    roundie_package_metadata_t metadata;
    uint8_t *elf = validate_file(TRANSFER, digest, &metadata);
    if (elf == NULL) {
        cancel();
        return ESP_ERR_INVALID_ARG;
    }
    roundie_packages_free(elf);
    esp_err_t result = roundie_packages_scan();
    if (result != ESP_OK) {
        return result;
    }
    roundie_package_record_t previous;
    bool exists = roundie_packages_find(metadata.id, &previous) == ESP_OK;
    if (exists && (previous.health == ROUNDIE_PACKAGE_CORRUPT ||
                   roundie_package_version_compare(metadata.version, previous.metadata.version) <= 0)) {
        return failed(ESP_ERR_INVALID_STATE, "replace-requires-healthy-older-version");
    }
    if (!exists && status.count + status.orphans >= ROUNDIE_PACKAGES_MAX) {
        return failed(ESP_ERR_NO_MEM, "package-count-limit");
    }
    size_t free_bytes;
    if (!roundie_packages_capacity(&free_bytes) || free_bytes < ROUNDIE_PACKAGES_RESERVE + 16384) {
        return failed(ESP_ERR_NO_MEM, "commit-storage-reserve");
    }
    /* The sole worker cannot process Safe Mode/OTA/RE1 in this protected,
     * bounded commit. No lock or callback is held across VFS operations. */
    status.phase = ROUNDIE_PACKAGES_COMMITTING;
    int64_t started = roundie_packages_clock();
    char path[PATH_SIZE];
    char temporary[PATH_SIZE];
    char directory[PATH_SIZE];
    path_for(directory, metadata.id, "");
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        result = failed(ESP_FAIL, "install-directory");
        goto done;
    }
    unsigned slot = exists ? 1U - previous.slot : 0;
    path_for(path, metadata.id, slot == 0 ? "0.rpk" : "1.rpk");
    if (rename(TRANSFER, path) != 0) {
        result = failed(ESP_FAIL, "install-artifact-rename");
        goto done;
    }
    uint8_t selection[SELECTION_SIZE] = {'R', 'P', 'A', '1', (uint8_t)slot, 0, 0, 0};
    memcpy(selection + 8, digest, 32);
    path_for(temporary, metadata.id, "active.tmp");
    int file = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (file < 0) {
        result = failed(ESP_FAIL, "selection-open");
        goto done;
    }
    bool written = write(file, selection, sizeof(selection)) == sizeof(selection);
    int synced = written ? fsync(file) : -1;
    int closed = close(file);
    path_for(path, metadata.id, "active");
    if (!written || synced != 0 || closed != 0 || rename(temporary, path) != 0) {
        result = failed(ESP_FAIL, "selection-commit");
        goto done;
    }
    result = ESP_OK;
    status.error[0] = '\0';
done:
    status.commit_us = (uint32_t)(roundie_packages_clock() - started);
    /* An inactive orphan is never selected on restart. Preserve it on failure;
     * retry/explicit removal owns cleanup, not a boot repair operation. */
    cancel();
    esp_err_t scanned = roundie_packages_scan();
    return result != ESP_OK ? result : scanned;
}

esp_err_t roundie_packages_launch(const char *id)
{
    if (!admitted() || roundie_packages_busy()) {
        return failed(ESP_ERR_INVALID_STATE, "launch-admission");
    }
    int64_t started = roundie_packages_clock();
    esp_err_t result = roundie_packages_scan();
    roundie_package_record_t record;
    if (result != ESP_OK) {
        return result;
    }
    result = roundie_packages_find(id, &record);
    if (result != ESP_OK) {
        return result;
    }
    if (record.health != ROUNDIE_PACKAGE_OK) {
        return failed(ESP_ERR_INVALID_STATE, "launch-unhealthy-package");
    }
    char path[PATH_SIZE];
    path_for(path, id, record.slot == 0 ? "0.rpk" : "1.rpk");
    roundie_package_metadata_t metadata;
    uint8_t *elf = validate_file(path, record.digest, &metadata);
    if (elf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    result = roundie_packages_execute(elf, metadata.elf_size, metadata.abi);
    if (result != ESP_OK) {
        roundie_packages_free(elf);
    }
    status.launch_us = (uint32_t)(roundie_packages_clock() - started);
    return result;
}

esp_err_t roundie_packages_remove(const char *id)
{
    if (!admitted() || roundie_packages_busy() || !roundie_package_id_valid(id)) {
        return failed(ESP_ERR_INVALID_STATE, "remove-admission");
    }
    int64_t started = roundie_packages_clock();
    /* Unlink active first: this is the logical removal commit. An interrupted
     * cleanup leaves only unselected artifacts, never a new active version. */
    static const char *const names[] = {"active", "active.tmp", "0.rpk", "1.rpk"};
    char path[PATH_SIZE];
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        path_for(path, id, names[i]);
        if (unlink(path) != 0 && errno != ENOENT) {
            roundie_packages_scan();
            return failed(ESP_FAIL, "remove-file");
        }
    }
    path_for(path, id, "");
    if (rmdir(path) != 0 && errno != ENOENT) {
        roundie_packages_scan();
        return failed(ESP_FAIL, "remove-directory");
    }
    status.removal_us = (uint32_t)(roundie_packages_clock() - started);
    status.error[0] = '\0';
    return roundie_packages_scan();
}
