/* Watch face store (docs/watch-face-packages.md): the sole system worker
 * owns every file under FACES_DIRECTORY. A face reaches the GUI only as an
 * owned buffer that passed the envelope, digest, ABI and definition checks;
 * the GUI never reads storage. Nothing here touches LVGL. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "private.h"
#include "psa/crypto.h"
#include "roundie_platform.h"

#ifndef FACES_DIRECTORY
#define FACES_DIRECTORY "/storage/faces"
#endif
#define TRANSFER FACES_DIRECTORY "/.transfer"
/* One VFS call holds the LittleFS lock; 4 KiB bounds that hold (E0 §14:
 * chunk_max 58-80 ms at 4 KiB, 184-240 ms at 16 KiB; docs/storage-io.md). */
#define VFS_CALL_MAX 4096U
#define PREFERENCE FACES_DIRECTORY "/active"
#define PREFERENCE_TEMPORARY FACES_DIRECTORY "/active.tmp"
#define PATH_SIZE (sizeof(FACES_DIRECTORY) + ROUNDIE_FACE_ID + 16)
#define RESERVE (256U * 1024U) /* as ROUNDIE_PACKAGES_RESERVE: faces never take the applications' reserve */
#define CHUNK_TIMEOUT_US 10000000
#define READY_TIMEOUT_US 60000000
#define PREFERENCE_SIZE 56U /* "RFA1", id[48], FNV-1a */
#define SETTINGS_SIZE 16U   /* "RFS1", count, values[4], 3 zero, FNV-1a */

static const roundie_watchface_store_platform_t *platform;
static roundie_lock_t lock = ROUNDIE_LOCK_INITIALIZER;
static roundie_watchface_status_t status; /* locked */
static int transfer = -1;
static uint8_t expected_digest[32];
static int64_t deadline;
static uint32_t next_serial;
static bool started; /* the directory was scanned and the preference offered */

/* Mailbox; locked. */
static struct {
    bool due;
    roundie_face_loaded_t *face;
    uint32_t purpose;
    uint32_t serial;
} offer;
static struct {
    bool due;
    uint32_t serial;
    uint32_t purpose;
    bool success;
    char id[ROUNDIE_FACE_ID];
} report;
static struct {
    bool due;
    char id[ROUNDIE_FACE_ID];
} preview_request, persist_request;
static struct {
    bool due;
    char id[ROUNDIE_FACE_ID];
    uint8_t values[ROUNDIE_FACE_SETTINGS_MAX];
    uint8_t count;
} settings_request;
static uint32_t select_serial; /* the SELECT offer whose success is persisted */
static char select_id[ROUNDIE_FACE_ID];

static uint32_t fnv1a(const uint8_t *bytes, size_t length)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static void put32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint32_t get32(const uint8_t *in)
{
    return (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

static esp_err_t failed(esp_err_t error, const char *reason)
{
    roundie_lock(&lock);
    snprintf(status.error, sizeof(status.error), "%s", reason);
    roundie_unlock(&lock);
    return error;
}

static bool admitted(void)
{
    return platform != NULL && started && !status.fallback_only && platform->allowed();
}

static bool path_for(char out[PATH_SIZE], const char *id, const char *suffix)
{
    if (!roundie_face_id_valid(id)) {
        return false;
    }
    int length = snprintf(out, PATH_SIZE, "%s/%s%s", FACES_DIRECTORY, id, suffix);
    return length > 0 && (size_t)length < PATH_SIZE;
}

static bool read_exact(int file, void *out, size_t size)
{
    uint8_t *bytes = out;
    while (size > 0) {
        size_t step = size < VFS_CALL_MAX ? size : VFS_CALL_MAX;
        ssize_t count = read(file, bytes, step);
        if (count <= 0) {
            return false;
        }
        bytes += count;
        size -= (size_t)count;
    }
    return true;
}

static bool write_exact(int file, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    while (size > 0) {
        size_t step = size < VFS_CALL_MAX ? size : VFS_CALL_MAX;
        if (write(file, bytes, step) != (ssize_t)step) {
            return false;
        }
        size -= step;
        bytes += step;
    }
    return true;
}

/* tmp + fsync + rename: the previous file stays until the new one is complete. */
static bool replace_file(const char *path, const char *temporary, const void *data, size_t size)
{
    int file = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (file < 0) {
        return false;
    }
    bool written = write_exact(file, data, size);
    int synced = written ? fsync(file) : -1;
    int closed = close(file);
    if (!written || synced != 0 || closed != 0 || rename(temporary, path) != 0) {
        unlink(temporary);
        return false;
    }
    return true;
}

static bool read_preference(char id[ROUNDIE_FACE_ID])
{
    uint8_t bytes[PREFERENCE_SIZE];
    int file = open(PREFERENCE, O_RDONLY);
    if (file < 0) {
        return false;
    }
    bool valid = read_exact(file, bytes, sizeof(bytes)) && read(file, bytes, 1) == 0;
    close(file);
    if (!valid || memcmp(bytes, "RFA1", 4) != 0 || get32(bytes + 52) != fnv1a(bytes, 52) ||
        memchr(bytes + 4, '\0', ROUNDIE_FACE_ID) == NULL) {
        return false;
    }
    memcpy(id, bytes + 4, ROUNDIE_FACE_ID);
    return roundie_face_id_valid(id);
}

static bool write_preference(const char *id)
{
    if (strcmp(id, ROUNDIE_WATCHFACE_SYSTEM) == 0) {
        return unlink(PREFERENCE) == 0 || errno == ENOENT;
    }
    uint8_t bytes[PREFERENCE_SIZE] = {'R', 'F', 'A', '1'};
    strncpy((char *)bytes + 4, id, ROUNDIE_FACE_ID - 1);
    put32(bytes + 52, fnv1a(bytes, 52));
    return replace_file(PREFERENCE, PREFERENCE_TEMPORARY, bytes, sizeof(bytes));
}

/* Stored settings, or the definition's defaults when absent or invalid. */
static void read_settings(const char *id, const roundie_face_definition_t *definition, uint8_t *values)
{
    for (unsigned i = 0; i < definition->setting_count; i++) {
        values[i] = definition->settings[i].initial;
    }
    char path[PATH_SIZE];
    uint8_t bytes[SETTINGS_SIZE];
    if (!path_for(path, id, ".cfg")) {
        return;
    }
    int file = open(path, O_RDONLY);
    if (file < 0) {
        return;
    }
    bool valid = read_exact(file, bytes, sizeof(bytes)) && read(file, bytes, 1) == 0;
    close(file);
    if (!valid || memcmp(bytes, "RFS1", 4) != 0 || get32(bytes + 12) != fnv1a(bytes, 12) ||
        bytes[4] != definition->setting_count) {
        return;
    }
    for (unsigned i = 0; i < definition->setting_count; i++) {
        if (bytes[5 + i] >= definition->settings[i].options) {
            return;
        }
    }
    memcpy(values, bytes + 5, definition->setting_count);
}

static bool write_settings(const char *id, const uint8_t *values, unsigned count)
{
    char path[PATH_SIZE];
    char temporary[PATH_SIZE];
    if (count > ROUNDIE_FACE_SETTINGS_MAX || !path_for(path, id, ".cfg") || !path_for(temporary, id, ".cfg.tmp")) {
        return false;
    }
    uint8_t bytes[SETTINGS_SIZE] = {'R', 'F', 'S', '1', (uint8_t)count};
    memcpy(bytes + 5, values, count);
    put32(bytes + 12, fnv1a(bytes, 12));
    return replace_file(path, temporary, bytes, sizeof(bytes));
}

void roundie_face_loaded_free(roundie_face_loaded_t *loaded)
{
    if (loaded != NULL && platform != NULL) {
        platform->free(loaded);
    }
}

/* Reads and checks a whole package: envelope, digest, ABI, backend and
 * definition. expected may be NULL (the envelope's digest is the reference). */
static roundie_face_loaded_t *load_file(const char *path, const uint8_t *expected, char error[ROUNDIE_FACE_ERROR])
{
    struct stat info;
    int file = open(path, O_RDONLY);
    if (file < 0) {
        snprintf(error, ROUNDIE_FACE_ERROR, "face-open");
        return NULL;
    }
    if (fstat(file, &info) != 0 || info.st_size < (off_t)ROUNDIE_FACE_PREFIX ||
        info.st_size > (off_t)ROUNDIE_FACE_PACKAGE_MAX) {
        close(file);
        snprintf(error, ROUNDIE_FACE_ERROR, "package-size");
        return NULL;
    }
    size_t size = (size_t)info.st_size;
    roundie_face_loaded_t *loaded = platform->allocate(sizeof(*loaded) + size);
    if (loaded == NULL) {
        close(file);
        snprintf(error, ROUNDIE_FACE_ERROR, "face-memory");
        return NULL;
    }
    memset(loaded, 0, sizeof(*loaded));
    loaded->bytes = (uint8_t *)(loaded + 1);
    loaded->size = size;
    bool read_ok = read_exact(file, loaded->bytes, size);
    int closed = close(file);
    if (!read_ok || closed != 0) {
        snprintf(error, ROUNDIE_FACE_ERROR, "face-read");
        goto fail;
    }
    if (!roundie_face_decode_package(loaded->bytes, size, size, &loaded->package, error)) {
        goto fail;
    }
    uint8_t digest[32];
    size_t length = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, loaded->bytes + ROUNDIE_FACE_HEADER, size - ROUNDIE_FACE_HEADER, digest,
                         sizeof(digest), &length) != PSA_SUCCESS ||
        length != 32 || memcmp(digest, loaded->package.digest, 32) != 0 ||
        (expected != NULL && memcmp(expected, digest, 32) != 0)) {
        snprintf(error, ROUNDIE_FACE_ERROR, "package-digest");
        goto fail;
    }
    const roundie_face_manifest_t *manifest = &loaded->package.manifest;
    if (manifest->backend == ROUNDIE_FACE_BACKEND_NATIVE) {
        snprintf(error, ROUNDIE_FACE_ERROR, "native-face-unsupported");
        goto fail;
    }
    if (!roundie_face_abi_compatible(manifest->abi_major, manifest->abi_minor, manifest->requires,
                                     ROUNDIE_FACE_HOST_CAPS)) {
        snprintf(error, ROUNDIE_FACE_ERROR, "face-abi");
        goto fail;
    }
    if (!roundie_face_decode_definition(loaded->bytes + loaded->package.definition_offset,
                                        loaded->package.definition_size, &loaded->package, &loaded->definition,
                                        error)) {
        goto fail;
    }
    memcpy(loaded->id, manifest->id, sizeof(loaded->id));
    read_settings(loaded->id, &loaded->definition, loaded->settings);
    roundie_face_declarative_bind(loaded);
    return loaded;
fail:
    platform->free(loaded);
    return NULL;
}

/* Discovery reads the fixed prefix and the resource table only; the whole
 * package is checked when it is loaded. */
static esp_err_t scan(void)
{
    roundie_watchface_entry_t found[ROUNDIE_WATCHFACE_MAX];
    unsigned count = 0;
    DIR *directory = opendir(FACES_DIRECTORY);
    if (directory == NULL) {
        roundie_lock(&lock);
        status.count = 0;
        status.revision++;
        roundie_unlock(&lock);
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    struct dirent *item;
    while ((item = readdir(directory)) != NULL) {
        size_t length = strlen(item->d_name);
        if (length <= 4 || length - 4 >= ROUNDIE_FACE_ID || strcmp(item->d_name + length - 4, ".rwf") != 0) {
            continue;
        }
        char id[ROUNDIE_FACE_ID];
        memcpy(id, item->d_name, length - 4);
        id[length - 4] = '\0';
        char path[PATH_SIZE];
        if (!path_for(path, id, ".rwf") || count >= ROUNDIE_WATCHFACE_MAX) {
            continue;
        }
        roundie_watchface_entry_t *entry = &found[count];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->id, id, sizeof(id));
        uint8_t prefix[ROUNDIE_FACE_PREFIX + ROUNDIE_FACE_RESOURCES_MAX * ROUNDIE_FACE_RESOURCE_ENTRY];
        struct stat info;
        int file = open(path, O_RDONLY);
        if (file >= 0 && fstat(file, &info) == 0 && info.st_size >= (off_t)ROUNDIE_FACE_PREFIX &&
            info.st_size <= (off_t)ROUNDIE_FACE_PACKAGE_MAX) {
            size_t available = (size_t)info.st_size < sizeof(prefix) ? (size_t)info.st_size : sizeof(prefix);
            roundie_face_package_t package;
            char error[ROUNDIE_FACE_ERROR];
            entry->size = (uint32_t)info.st_size;
            if (read_exact(file, prefix, available) &&
                roundie_face_decode_package(prefix, available, (size_t)info.st_size, &package, error) &&
                strcmp(package.manifest.id, id) == 0) {
                memcpy(entry->name, package.manifest.name, sizeof(entry->name));
                memcpy(entry->version, package.manifest.version, sizeof(entry->version));
                entry->healthy = package.manifest.backend == ROUNDIE_FACE_BACKEND_DECLARATIVE;
                entry->preview = package.manifest.preview != ROUNDIE_FACE_NO_PREVIEW;
            }
        }
        if (file >= 0) {
            close(file);
        }
        count++;
    }
    closedir(directory);
    /* Sorted by id: the gallery order does not depend on the directory. */
    for (unsigned i = 1; i < count; i++) {
        for (unsigned j = i; j > 0 && strcmp(found[j - 1].id, found[j].id) > 0; j--) {
            roundie_watchface_entry_t swap = found[j - 1];
            found[j - 1] = found[j];
            found[j] = swap;
        }
    }
    roundie_lock(&lock);
    memcpy(status.entries, found, sizeof(found[0]) * count);
    status.count = count;
    status.revision++;
    roundie_unlock(&lock);
    return ESP_OK;
}

static const roundie_watchface_entry_t *find_entry(const char *id)
{
    for (unsigned i = 0; i < status.count; i++) {
        if (strcmp(status.entries[i].id, id) == 0) {
            return &status.entries[i];
        }
    }
    return NULL;
}

/* Replaces an untaken offer; the replaced face is freed outside the lock. */
static uint32_t post(roundie_face_loaded_t *face, uint32_t purpose)
{
    roundie_lock(&lock);
    roundie_face_loaded_t *replaced = offer.due ? offer.face : NULL;
    uint32_t serial = ++next_serial;
    offer.due = true;
    offer.face = face;
    offer.purpose = purpose;
    offer.serial = serial;
    status.pending = 1;
    roundie_unlock(&lock);
    roundie_face_loaded_free(replaced);
    return serial;
}

/* Loads an installed face for the GUI. */
static esp_err_t offer_face(const char *id, uint32_t purpose, uint32_t *serial)
{
    char path[PATH_SIZE];
    if (!path_for(path, id, ".rwf")) {
        return failed(ESP_ERR_INVALID_ARG, "face-id");
    }
    char error[ROUNDIE_FACE_ERROR] = "";
    int64_t started = platform->clock();
    roundie_face_loaded_t *loaded = load_file(path, NULL, error);
    roundie_lock(&lock);
    status.load_us = (uint32_t)(platform->clock() - started);
    status.loads++;
    status.load_failures += loaded == NULL;
    roundie_unlock(&lock);
    if (loaded == NULL) {
        return failed(errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_ARG, error);
    }
    if (strcmp(loaded->id, id) != 0) {
        platform->free(loaded);
        return failed(ESP_ERR_INVALID_ARG, "face-id-mismatch");
    }
    loaded->purpose = purpose;
    uint32_t posted = post(loaded, purpose);
    loaded = NULL; /* owned by the mailbox now */
    if (serial != NULL) {
        *serial = posted;
    }
    return ESP_OK;
}

static void start(void);

void roundie_watchface_init(const roundie_watchface_store_platform_t *store_platform, bool fallback_only)
{
    platform = store_platform;
    roundie_lock(&lock);
    /* Boot state: called once per boot (host tests call it to simulate one). */
    memset(&status, 0, sizeof(status));
    status.fallback_only = fallback_only;
    snprintf(status.active, sizeof(status.active), "%s", ROUNDIE_WATCHFACE_SYSTEM);
    roundie_unlock(&lock);
    started = false;
    if (fallback_only || !platform->allowed()) {
        /* Storage admission comes later on a normal boot (BootGuard STABLE):
         * the worker poll starts the store then. */
        failed(ESP_ERR_INVALID_STATE, fallback_only ? "fallback-only" : "storage-unavailable");
        return;
    }
    start();
}

/* Once per boot, when storage is admitted. */
static void start(void)
{
    started = true;
    failed(ESP_OK, "");
    if (mkdir(FACES_DIRECTORY, 0700) != 0 && errno != EEXIST) {
        failed(ESP_FAIL, "face-directory");
        return;
    }
    scan();
    char id[ROUNDIE_FACE_ID];
    if (!read_preference(id)) {
        return; /* none (or unreadable: the system face, the file is left as it is) */
    }
    roundie_lock(&lock);
    memcpy(status.preferred, id, sizeof(id));
    roundie_unlock(&lock);
    /* A corrupt or missing preferred face leaves the fallback on screen and
     * the preference in place: no automatic repair. */
    offer_face(id, ROUNDIE_FACE_PURPOSE_BOOT, NULL);
}

static esp_err_t cancel(void)
{
    int closed = transfer >= 0 ? close(transfer) : 0;
    transfer = -1;
    int removed = unlink(TRANSFER);
    roundie_lock(&lock);
    status.phase = ROUNDIE_WATCHFACE_IDLE;
    status.session = 0;
    status.received = 0;
    status.total = 0;
    roundie_unlock(&lock);
    return closed == 0 && (removed == 0 || errno == ENOENT) ? ESP_OK : failed(ESP_FAIL, "temporary-cleanup");
}

esp_err_t roundie_watchface_begin(uint32_t session, size_t size, const uint8_t digest[32])
{
    if (!admitted() || status.phase != ROUNDIE_WATCHFACE_IDLE) {
        return failed(ESP_ERR_INVALID_STATE, "face-admission");
    }
    if (session == 0 || size < ROUNDIE_FACE_PREFIX || size > ROUNDIE_FACE_PACKAGE_MAX) {
        return failed(ESP_ERR_INVALID_SIZE, "face-size-or-session");
    }
    size_t free_bytes;
    if (!platform->capacity(&free_bytes) || free_bytes < RESERVE + size + 16384U) {
        return failed(ESP_ERR_NO_MEM, "face-storage-reserve");
    }
    if (mkdir(FACES_DIRECTORY, 0700) != 0 && errno != EEXIST) {
        return failed(ESP_FAIL, "face-directory");
    }
    /* BEGIN retires the one known interrupted transfer. */
    if (unlink(TRANSFER) != 0 && errno != ENOENT) {
        return failed(ESP_FAIL, "temporary-cleanup");
    }
    transfer = open(TRANSFER, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (transfer < 0) {
        return failed(ESP_FAIL, "transfer-open");
    }
    memcpy(expected_digest, digest, 32);
    deadline = platform->clock() + CHUNK_TIMEOUT_US;
    roundie_lock(&lock);
    status.phase = ROUNDIE_WATCHFACE_RECEIVING;
    status.session = session;
    status.received = 0;
    status.total = (uint32_t)size;
    status.error[0] = '\0';
    roundie_unlock(&lock);
    return ESP_OK;
}

esp_err_t roundie_watchface_write(uint32_t session, size_t offset, const void *data, size_t size)
{
    if (!admitted() || transfer < 0 || status.phase != ROUNDIE_WATCHFACE_RECEIVING || session != status.session) {
        return failed(ESP_ERR_INVALID_STATE, "transfer-state");
    }
    if (offset != status.received || size == 0 || size > 512 || size > status.total - status.received) {
        return failed(ESP_ERR_INVALID_SIZE, "transfer-offset-or-length");
    }
    if (!write_exact(transfer, data, size)) {
        cancel();
        return failed(ESP_FAIL, "transfer-write");
    }
    deadline = platform->clock() + CHUNK_TIMEOUT_US;
    roundie_lock(&lock);
    status.received += (uint32_t)size;
    roundie_unlock(&lock);
    return ESP_OK;
}

esp_err_t roundie_watchface_finish(uint32_t session)
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
    if (synced != 0 || closed != 0) {
        cancel();
        return failed(ESP_FAIL, "transfer-sync");
    }
    char error[ROUNDIE_FACE_ERROR] = "";
    roundie_face_loaded_t *loaded = load_file(TRANSFER, expected_digest, error);
    if (loaded == NULL) {
        cancel();
        return failed(ESP_ERR_INVALID_ARG, error);
    }
    platform->free(loaded);
    deadline = platform->clock() + READY_TIMEOUT_US;
    roundie_lock(&lock);
    status.phase = ROUNDIE_WATCHFACE_READY;
    status.error[0] = '\0';
    roundie_unlock(&lock);
    return ESP_OK;
}

esp_err_t roundie_watchface_abort(uint32_t session)
{
    if (status.phase == ROUNDIE_WATCHFACE_IDLE || session != status.session) {
        return failed(ESP_ERR_INVALID_STATE, "transfer-state");
    }
    return cancel();
}

esp_err_t roundie_watchface_install(const uint8_t digest[32])
{
    if (!admitted() || status.phase != ROUNDIE_WATCHFACE_READY || memcmp(digest, expected_digest, 32) != 0) {
        return failed(ESP_ERR_INVALID_STATE, "install-admission-or-digest");
    }
    /* Checked again: the staged file is what gets renamed into place. */
    char error[ROUNDIE_FACE_ERROR] = "";
    roundie_face_loaded_t *loaded = load_file(TRANSFER, digest, error);
    if (loaded == NULL) {
        cancel();
        return failed(ESP_ERR_INVALID_ARG, error);
    }
    char id[ROUNDIE_FACE_ID];
    uint16_t version[3];
    memcpy(id, loaded->id, sizeof(id));
    memcpy(version, loaded->package.manifest.version, sizeof(version));
    platform->free(loaded);
    scan();
    const roundie_watchface_entry_t *previous = find_entry(id);
    esp_err_t result = ESP_OK;
    if (previous != NULL && previous->healthy) {
        bool newer = false;
        for (unsigned i = 0; i < 3; i++) {
            if (version[i] != previous->version[i]) {
                newer = version[i] > previous->version[i];
                break;
            }
        }
        /* The scan checks only the prefix: a same or older version may
         * replace an installed file only when that file no longer loads
         * (repair after corruption). */
        char installed[PATH_SIZE];
        char ignored[ROUNDIE_FACE_ERROR] = "";
        roundie_face_loaded_t *current = NULL;
        if (!newer && path_for(installed, id, ".rwf")) {
            current = load_file(installed, NULL, ignored);
        }
        if (!newer && current != NULL) {
            platform->free(current);
            result = failed(ESP_ERR_INVALID_VERSION, "replace-requires-newer-version");
        }
    } else if (previous == NULL && status.count >= ROUNDIE_WATCHFACE_MAX) {
        result = failed(ESP_ERR_NO_MEM, "face-count-limit");
    }
    size_t free_bytes;
    if (result == ESP_OK && (!platform->capacity(&free_bytes) || free_bytes < RESERVE)) {
        result = failed(ESP_ERR_NO_MEM, "commit-storage-reserve");
    }
    char path[PATH_SIZE];
    if (result == ESP_OK && (!path_for(path, id, ".rwf") || rename(TRANSFER, path) != 0)) {
        result = failed(ESP_FAIL, "install-rename");
    }
    if (result == ESP_OK) {
        roundie_lock(&lock);
        status.error[0] = '\0';
        roundie_unlock(&lock);
    }
    cancel();
    scan();
    return result;
}

esp_err_t roundie_watchface_install_file(const char *source)
{
    if (!admitted() || status.phase != ROUNDIE_WATCHFACE_IDLE) {
        return failed(ESP_ERR_INVALID_STATE, "face-admission");
    }
    char error[ROUNDIE_FACE_ERROR] = "";
    roundie_face_loaded_t *loaded = load_file(source, NULL, error);
    if (loaded == NULL) {
        return failed(ESP_ERR_INVALID_ARG, error);
    }
    uint8_t digest[32];
    memcpy(digest, loaded->package.digest, sizeof(digest));
    esp_err_t result = roundie_watchface_begin(1, loaded->size, digest);
    for (size_t offset = 0; result == ESP_OK && offset < loaded->size; offset += 512U) {
        size_t chunk = loaded->size - offset < 512U ? loaded->size - offset : 512U;
        result = roundie_watchface_write(1, offset, loaded->bytes + offset, chunk);
    }
    platform->free(loaded);
    if (result == ESP_OK) {
        result = roundie_watchface_finish(1);
    }
    if (result == ESP_OK) {
        result = roundie_watchface_install(digest);
    } else if (status.phase != ROUNDIE_WATCHFACE_IDLE) {
        cancel();
    }
    return result;
}

esp_err_t roundie_watchface_select(const char *id)
{
    if (platform == NULL || id == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(id, ROUNDIE_WATCHFACE_SYSTEM) == 0) {
        uint32_t serial = post(NULL, ROUNDIE_FACE_PURPOSE_SELECT);
        roundie_lock(&lock);
        select_serial = serial;
        memcpy(select_id, ROUNDIE_WATCHFACE_SYSTEM, sizeof(ROUNDIE_WATCHFACE_SYSTEM));
        roundie_unlock(&lock);
        return ESP_OK;
    }
    if (!admitted()) {
        return failed(ESP_ERR_INVALID_STATE, "face-admission");
    }
    uint32_t serial = 0;
    esp_err_t result = offer_face(id, ROUNDIE_FACE_PURPOSE_SELECT, &serial);
    if (result == ESP_OK) {
        roundie_lock(&lock);
        select_serial = serial;
        snprintf(select_id, sizeof(select_id), "%s", id);
        roundie_unlock(&lock);
    }
    return result;
}

esp_err_t roundie_watchface_remove(const char *id)
{
    char path[PATH_SIZE];
    if (!admitted() || status.phase != ROUNDIE_WATCHFACE_IDLE) {
        return failed(ESP_ERR_INVALID_STATE, "face-admission");
    }
    if (!path_for(path, id, ".rwf")) {
        return failed(ESP_ERR_INVALID_ARG, "face-id");
    }
    if (unlink(path) != 0) {
        return failed(errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL, "face-remove");
    }
    if (path_for(path, id, ".cfg")) {
        unlink(path);
    }
    roundie_lock(&lock);
    bool preferred = strcmp(status.preferred, id) == 0;
    bool active = strcmp(status.active, id) == 0;
    roundie_unlock(&lock);
    esp_err_t result = ESP_OK;
    if (preferred) {
        if (write_preference(ROUNDIE_WATCHFACE_SYSTEM)) {
            roundie_lock(&lock);
            status.preferred[0] = '\0';
            roundie_unlock(&lock);
        } else {
            result = failed(ESP_FAIL, "preference-remove");
        }
    }
    if (active) {
        post(NULL, ROUNDIE_FACE_PURPOSE_REMOVED);
    }
    scan();
    return result;
}

static void persist(const char *id)
{
    if (status.fallback_only || !platform->allowed()) {
        failed(ESP_ERR_INVALID_STATE, "preference-admission");
        return;
    }
    if (!write_preference(id)) {
        failed(ESP_FAIL, "preference-write");
        return;
    }
    roundie_lock(&lock);
    if (strcmp(id, ROUNDIE_WATCHFACE_SYSTEM) == 0) {
        status.preferred[0] = '\0';
    } else {
        snprintf(status.preferred, sizeof(status.preferred), "%s", id);
    }
    roundie_unlock(&lock);
}

void roundie_watchface_poll(int64_t now_us)
{
    if (platform == NULL) {
        return;
    }
    if (!started && !status.fallback_only && platform->allowed()) {
        start();
    }
    if (status.phase != ROUNDIE_WATCHFACE_IDLE && (!admitted() || now_us >= deadline)) {
        cancel();
        failed(ESP_ERR_INVALID_STATE, "transfer-cancelled-or-expired");
    }
    roundie_lock(&lock);
    bool reported = report.due && report.purpose == ROUNDIE_FACE_PURPOSE_SELECT && report.success &&
        report.serial == select_serial;
    char reported_id[ROUNDIE_FACE_ID];
    memcpy(reported_id, select_id, sizeof(reported_id));
    report.due = false;
    bool preview = preview_request.due;
    char preview_id[ROUNDIE_FACE_ID];
    memcpy(preview_id, preview_request.id, sizeof(preview_id));
    preview_request.due = false;
    bool keep = persist_request.due;
    char keep_id[ROUNDIE_FACE_ID];
    memcpy(keep_id, persist_request.id, sizeof(keep_id));
    persist_request.due = false;
    bool save = settings_request.due;
    char save_id[ROUNDIE_FACE_ID];
    uint8_t values[ROUNDIE_FACE_SETTINGS_MAX];
    uint8_t count = settings_request.count;
    memcpy(save_id, settings_request.id, sizeof(save_id));
    memcpy(values, settings_request.values, sizeof(values));
    settings_request.due = false;
    roundie_unlock(&lock);
    if (reported) {
        persist(reported_id);
    }
    if (keep) {
        persist(keep_id);
    }
    if (save && !status.fallback_only && platform->allowed() && !write_settings(save_id, values, count)) {
        failed(ESP_FAIL, "settings-write");
    }
    if (preview) {
        if (strcmp(preview_id, ROUNDIE_WATCHFACE_SYSTEM) == 0) {
            post(NULL, ROUNDIE_FACE_PURPOSE_PREVIEW);
        } else if (!admitted() || offer_face(preview_id, ROUNDIE_FACE_PURPOSE_PREVIEW, NULL) != ESP_OK) {
            post(NULL, ROUNDIE_FACE_PURPOSE_PREVIEW); /* the gallery shows the refusal on the fallback */
        }
    }
}

void roundie_watchface_status(roundie_watchface_status_t *out)
{
    roundie_lock(&lock);
    *out = status;
    roundie_unlock(&lock);
}

bool roundie_face_mailbox_take(roundie_face_loaded_t **face, uint32_t *purpose, uint32_t *serial)
{
    roundie_lock(&lock);
    bool due = offer.due;
    if (due) {
        *face = offer.face;
        *purpose = offer.purpose;
        *serial = offer.serial;
        offer.due = false;
        offer.face = NULL;
        status.pending = 0;
    }
    roundie_unlock(&lock);
    return due;
}

void roundie_face_mailbox_report(uint32_t serial, uint32_t purpose, bool success, const char *id)
{
    roundie_lock(&lock);
    report.due = true;
    report.serial = serial;
    report.purpose = purpose;
    report.success = success;
    snprintf(report.id, sizeof(report.id), "%s", id);
    roundie_unlock(&lock);
}

void roundie_face_mailbox_preview(const char *id)
{
    roundie_lock(&lock);
    preview_request.due = true;
    snprintf(preview_request.id, sizeof(preview_request.id), "%s", id);
    roundie_unlock(&lock);
}

void roundie_face_mailbox_persist(const char *id)
{
    roundie_lock(&lock);
    persist_request.due = true;
    snprintf(persist_request.id, sizeof(persist_request.id), "%s", id);
    roundie_unlock(&lock);
}

void roundie_face_mailbox_settings(const char *id, const uint8_t *values, unsigned count)
{
    roundie_lock(&lock);
    settings_request.due = true;
    snprintf(settings_request.id, sizeof(settings_request.id), "%s", id);
    settings_request.count = (uint8_t)(count < ROUNDIE_FACE_SETTINGS_MAX ? count : ROUNDIE_FACE_SETTINGS_MAX);
    memcpy(settings_request.values, values, settings_request.count);
    roundie_unlock(&lock);
}

void roundie_face_mailbox_gui_status(const roundie_watchface_status_t *gui)
{
    roundie_lock(&lock);
    memcpy(status.active, gui->active, sizeof(status.active));
    memcpy(status.failed, gui->failed, sizeof(status.failed));
    status.host_error = gui->host_error;
    status.home = gui->home;
    status.parked = gui->parked;
    status.visible = gui->visible;
    status.gallery = gui->gallery;
    status.creates = gui->creates;
    status.destroys = gui->destroys;
    status.activations = gui->activations;
    status.deactivations = gui->deactivations;
    status.updates = gui->updates;
    status.switches = gui->switches;
    status.failures = gui->failures;
    status.refused = gui->refused;
    status.resumes = gui->resumes;
    status.objects = gui->objects;
    status.pool_free = gui->pool_free;
    status.face_bytes = gui->face_bytes;
    status.activate_us = gui->activate_us;
    status.activate_max_us = gui->activate_max_us;
    status.update_max_us = gui->update_max_us;
    status.resume_us = gui->resume_us;
    roundie_unlock(&lock);
}
