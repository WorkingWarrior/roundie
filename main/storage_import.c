#include "storage_import.h"
#include <string.h>
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_USB_STORAGE
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "psa/crypto.h"
#include "roundie_package_format.h"
#include "roundie_packages.h"
#include "app_runtime.h"
#include "roundie_storage_mode.h"

#define INBOX_PATH "/usbdisk/Inbox"

static const char *TAG = "storage_import";

static roundie_import_t import;
static bool initialized;
static FILE *current_file;
static psa_hash_operation_t hash;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static roundie_import_status_t published;
static _Atomic bool request_flag;
static _Atomic bool cancel_flag;
static _Atomic int decision_value; /* 0 none, 1 install, 2 skip */

static int ops_list(void *context, roundie_import_entry_t *entries, unsigned capacity, unsigned *count)
{
    (void)context;
    DIR *directory = opendir(INBOX_PATH);
    if (directory == NULL) {
        return -1;
    }
    unsigned out = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL && out < capacity) {
        roundie_import_entry_t *item = &entries[out];
        memset(item, 0, sizeof(*item));
        /* A name that does not fit is left blank: the classifier ignores it. */
        if (strlen(entry->d_name) < sizeof(item->name)) {
            strcpy(item->name, entry->d_name);
            char path[sizeof(INBOX_PATH) + 1 + ROUNDIE_IMPORT_NAME];
            snprintf(path, sizeof(path), INBOX_PATH "/%s", entry->d_name);
            struct stat info;
            if (stat(path, &info) == 0 && S_ISREG(info.st_mode)) {
                item->regular = true;
                item->size = (size_t)info.st_size;
            }
        }
        out++;
    }
    closedir(directory);
    *count = out;
    return 0;
}

static int ops_open(void *context, const char *name)
{
    (void)context;
    char path[sizeof(INBOX_PATH) + 1 + ROUNDIE_IMPORT_NAME];
    snprintf(path, sizeof(path), INBOX_PATH "/%s", name);
    current_file = fopen(path, "rb");
    return current_file != NULL ? 0 : -1;
}

static int ops_read(void *context, void *buffer, size_t capacity, size_t *length)
{
    (void)context;
    if (current_file == NULL) {
        return -1;
    }
    *length = fread(buffer, 1, capacity, current_file);
    return ferror(current_file) ? -1 : 0;
}

static void ops_close(void *context)
{
    (void)context;
    if (current_file != NULL) {
        fclose(current_file);
        current_file = NULL;
    }
}

static int ops_remove(void *context, const char *name)
{
    (void)context;
    char path[sizeof(INBOX_PATH) + 1 + ROUNDIE_IMPORT_NAME];
    snprintf(path, sizeof(path), INBOX_PATH "/%s", name);
    return unlink(path);
}

static void ops_hash_start(void *context)
{
    (void)context;
    psa_hash_abort(&hash);
    hash = psa_hash_operation_init();
    psa_hash_setup(&hash, PSA_ALG_SHA_256);
}

static void ops_hash_update(void *context, const void *data, size_t length)
{
    (void)context;
    psa_hash_update(&hash, data, length);
}

static void ops_hash_finish(void *context, uint8_t digest[32])
{
    (void)context;
    size_t length = 0;
    if (psa_hash_finish(&hash, digest, 32, &length) != PSA_SUCCESS || length != 32) {
        memset(digest, 0, 32); /* PackageManager rejects it at END: the digest no longer matches. */
    }
}

static bool ops_describe(void *context, const uint8_t *prefix, size_t available, size_t total,
                         roundie_import_candidate_t *out, char reason[ROUNDIE_IMPORT_REASON])
{
    (void)context;
    roundie_package_metadata_t metadata;
    char error[ROUNDIE_ELF_ERROR_SIZE] = "";
    if (!roundie_package_decode(prefix, available, total, &metadata, error)) {
        snprintf(reason, ROUNDIE_IMPORT_REASON, "%.*s", (int)ROUNDIE_IMPORT_REASON - 1,
                 error[0] != '\0' ? error : "not a package");
        return false;
    }
    snprintf(out->id, sizeof(out->id), "%s", metadata.id);
    snprintf(out->name, sizeof(out->name), "%s", metadata.name);
    memcpy(out->version, metadata.version, sizeof(out->version));
    return true;
}

static bool ops_installed(void *context, const char *package_id)
{
    (void)context;
    roundie_package_record_t record;
    return roundie_packages_find(package_id, &record) == ESP_OK;
}

static bool ops_ready(void *context)
{
    (void)context;
    /* Not before the boot is stable: PackageManager admits nothing earlier. */
    return roundie_app_runtime_packages_ready();
}

static int ops_begin(void *context, uint32_t session, size_t size, const uint8_t digest[32])
{
    (void)context;
    return roundie_packages_begin(session, size, digest) == ESP_OK ? 0 : -1;
}

static int ops_write(void *context, uint32_t session, size_t offset, const void *data, size_t length)
{
    (void)context;
    return roundie_packages_write(session, offset, data, length) == ESP_OK ? 0 : -1;
}

static int ops_finish(void *context, uint32_t session)
{
    (void)context;
    return roundie_packages_finish(session) == ESP_OK ? 0 : -1;
}

static int ops_install(void *context, const uint8_t digest[32])
{
    (void)context;
    return roundie_packages_install(digest) == ESP_OK ? 0 : -1;
}

static int ops_abort(void *context, uint32_t session)
{
    (void)context;
    return roundie_packages_abort(session) == ESP_OK ? 0 : -1;
}

static void ops_last_error(void *context, char reason[ROUNDIE_IMPORT_REASON])
{
    (void)context;
    roundie_packages_status_t status;
    roundie_packages_status(&status);
    snprintf(reason, ROUNDIE_IMPORT_REASON, "%.*s", (int)ROUNDIE_IMPORT_REASON - 1,
             status.error[0] != '\0' ? status.error : "refused");
}

static const roundie_import_ops_t operations = {
    .list = ops_list, .open = ops_open, .read = ops_read, .close = ops_close, .remove = ops_remove,
    .sha256_start = ops_hash_start, .sha256_update = ops_hash_update, .sha256_finish = ops_hash_finish,
    .describe = ops_describe, .installed = ops_installed, .ready = ops_ready, .begin = ops_begin, .write = ops_write,
    .finish = ops_finish, .install = ops_install, .abort = ops_abort, .last_error = ops_last_error,
};

void roundie_storage_import_request(void)
{
    atomic_store(&request_flag, true);
}

void roundie_storage_import_cancel(void)
{
    atomic_store(&cancel_flag, true);
}

void roundie_storage_import_decide(bool install)
{
    atomic_store(&decision_value, install ? 1 : 2);
}

void roundie_storage_import_snapshot(roundie_import_status_t *out)
{
    portENTER_CRITICAL(&lock);
    memcpy(out, &published, sizeof(*out));
    portEXIT_CRITICAL(&lock);
}

static void publish(void)
{
    portENTER_CRITICAL(&lock);
    memcpy(&published, &import.status, sizeof(published));
    portEXIT_CRITICAL(&lock);
}

void roundie_storage_import_poll(int64_t now_us)
{
    if (!roundie_storage_mode_active()) {
        return;
    }
    if (!initialized) {
        roundie_import_init(&import, &operations, ROUNDIE_PACKAGE_MAX);
        initialized = true;
        publish();
    }
    if (atomic_exchange(&request_flag, false) && roundie_import_start(&import)) {
        ESP_LOGI(TAG, "Import run started");
        atomic_store(&decision_value, 0);
        atomic_store(&cancel_flag, false);
        publish();
    }
    if (!roundie_import_active(&import)) {
        atomic_store(&cancel_flag, false);
        return;
    }
    if (atomic_exchange(&cancel_flag, false)) {
        roundie_import_cancel(&import);
    }
    int answer = atomic_exchange(&decision_value, 0);
    if (answer != 0) {
        roundie_import_decide(&import, answer == 1);
    }
    roundie_import_phase_t before = import.status.phase;
    unsigned results_before = import.status.results_count;
    roundie_import_step(&import, now_us);
    if (import.status.phase != before || import.status.results_count != results_before ||
        import.step == ROUNDIE_IMPORT_STEP_HASH || import.step == ROUNDIE_IMPORT_STEP_WRITE) {
        publish();
    }
    if (import.status.phase == ROUNDIE_IMPORT_DONE) {
        ESP_LOGI(TAG, "Import run finished: %u results", import.status.results_count);
    }
}
#else
void roundie_storage_import_poll(int64_t now_us)
{
    (void)now_us;
}
void roundie_storage_import_request(void)
{
}
void roundie_storage_import_cancel(void)
{
}
void roundie_storage_import_decide(bool install)
{
    (void)install;
}
void roundie_storage_import_snapshot(roundie_import_status_t *out)
{
    memset(out, 0, sizeof(*out));
}
#endif
