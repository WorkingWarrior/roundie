#include "storage_diagnostics.h"

#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "roundie_bounded_io.h"
#include "roundie_storage.h"

static const char *TAG = "storage";
static roundie_storage_t *storage;

bool roundie_storage_usage(size_t *total, size_t *used)
{
    roundie_storage_status_t status;
    if (storage == NULL || roundie_storage_capacity(storage, &status) != ROUNDIE_STORAGE_OK ||
        !status.mounted || !status.capacity_valid || status.used_bytes > status.total_bytes) {
        return false;
    }
    *total = status.total_bytes;
    *used = status.used_bytes;
    return true;
}

bool roundie_storage_available(void)
{
    roundie_storage_status_t status;
    return storage != NULL && roundie_storage_status(storage, &status) == ROUNDIE_STORAGE_OK && status.mounted;
}

/* Only a deliberately provisioned request enables this bounded boot diagnostic.
 * Claim it before writing: an interrupted/failed test is never retried on boot.
 * This runs before GUI startup, with exclusive storage lifecycle ownership.
 */
static void migration_filesystem_test(void)
{
    const char *request = "/storage/.roundie-m2b-request";
    const char *claimed = "/storage/.roundie-m2b-running";
    const char *path = "/storage/.roundie-m2b-data";
    static const char token[] = "Roundie Milestone 2B filesystem check v1\n";
    uint8_t expected[256];
    uint8_t actual[257];
    struct stat info;
    int descriptor = roundie_io_open(request, O_RDONLY, 0);
    if (descriptor < 0) {
        if (errno != ENOENT) {
            ESP_LOGE(TAG, "M2B request open failed: errno=%d", errno);
        }
        return;
    }
    ssize_t count = roundie_io_read(descriptor, actual, sizeof(actual));
    int close_result = roundie_io_close(descriptor);
    if (count != (ssize_t)(sizeof(token) - 1) || close_result != 0 ||
        memcmp(actual, token, sizeof(token) - 1) != 0) {
        ESP_LOGE(TAG, "M2B request invalid; no test writes performed");
        return;
    }
    errno = 0;
    if (roundie_io_stat(claimed, &info) == 0 || errno != ENOENT) {
        ESP_LOGE(TAG, "M2B previous test state exists or cannot be checked; stopped");
        return;
    }
    if (roundie_io_rename(request, claimed) != 0) {
        ESP_LOGE(TAG, "M2B request claim failed: errno=%d", errno);
        return;
    }
    for (size_t index = 0; index < sizeof(expected); index++) {
        expected[index] = (uint8_t)(index ^ 0xa5);
    }
    const char *stage = "create";
    descriptor = roundie_io_open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) {
        goto failed;
    }
    stage = "write/sync/close";
    count = roundie_io_write(descriptor, expected, sizeof(expected));
    int sync_result = roundie_io_fsync(descriptor);
    close_result = roundie_io_close(descriptor);
    if (count != (ssize_t)sizeof(expected) || sync_result != 0 || close_result != 0) {
        goto failed;
    }
    stage = "unmount/remount";
    if (roundie_storage_unmount(storage) != ROUNDIE_STORAGE_OK ||
        roundie_storage_mount(storage) != ROUNDIE_STORAGE_OK) {
        goto failed;
    }
    stage = "read/integrity";
    descriptor = roundie_io_open(path, O_RDONLY, 0);
    if (descriptor < 0) {
        goto failed;
    }
    count = roundie_io_read(descriptor, actual, sizeof(actual));
    close_result = roundie_io_close(descriptor);
    if (count != (ssize_t)sizeof(expected) || close_result != 0 ||
        memcmp(actual, expected, sizeof(expected)) != 0) {
        goto failed;
    }
    stage = "delete";
    if (roundie_io_unlink(path) != 0 || roundie_io_unlink(claimed) != 0) {
        goto failed;
    }
    stage = "deletion persistence";
    if (roundie_storage_unmount(storage) != ROUNDIE_STORAGE_OK ||
        roundie_storage_mount(storage) != ROUNDIE_STORAGE_OK) {
        goto failed;
    }
    const char *removed[] = {path, request, claimed};
    for (size_t index = 0; index < sizeof(removed) / sizeof(removed[0]); index++) {
        errno = 0;
        if (roundie_io_stat(removed[index], &info) == 0 || errno != ENOENT) {
            goto failed;
        }
    }
    roundie_storage_status_t status;
    stage = "capacity";
    if (roundie_storage_capacity(storage, &status) != ROUNDIE_STORAGE_OK) {
        goto failed;
    }
    ESP_LOGI(TAG, "M2B filesystem PASS: create/write/sync/remount/read/integrity/delete/remount; "
             "bytes=%zu total=%zu used=%zu; request consumed", sizeof(expected),
             status.total_bytes, status.used_bytes);
    return;

failed:
    ESP_LOGE(TAG, "M2B filesystem FAIL at %s (errno=%d); no automatic retry/format", stage, errno);
}

typedef struct {
    const char *label;
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t offset;
    uint32_t size;
} layout_entry_t;

static bool rep_layout_matches(void)
{
    static const layout_entry_t expected[] = {
        {"nvs", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x9000, 0x4000},
        {"otadata", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, 0xd000, 0x2000},
        {"phy_init", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY, 0xf000, 0x1000},
        {"ota_0", ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x10000, 0x380000},
        {"ota_1", ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x390000, 0x380000},
        {"storage", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, 0x710000, 0x8d0000},
        {"coredump", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, 0xfe0000, 0x20000},
    };
    size_t count = 0;
    esp_partition_iterator_t iterator = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                           ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (iterator != NULL) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        bool found = false;
        for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); index++) {
            const layout_entry_t *entry = &expected[index];
            if (strcmp(partition->label, entry->label) == 0 &&
                partition->type == entry->type && partition->subtype == entry->subtype &&
                partition->address == entry->offset && partition->size == entry->size &&
                !partition->encrypted && !partition->readonly) {
                found = true;
                break;
            }
        }
        if (!found) {
            esp_partition_iterator_release(iterator);
            return false;
        }
        count++;
        iterator = esp_partition_next(iterator);
    }
    return count == sizeof(expected) / sizeof(expected[0]);
}

bool roundie_storage_diagnostics(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    bool rep = rep_layout_matches();
    const char *profile = strcmp(esp_app_get_description()->project_name,
                                 "roundie_rep") == 0 ? "rep" : "legacy";
    if (running != NULL) {
        ESP_LOGI(TAG, "Build profile=%s; active=%s type=%u subtype=0x%02x offset=0x%" PRIx32
                 " size=0x%" PRIx32 "; detected layout=%s", profile, running->label,
                 (unsigned)running->type, (unsigned)running->subtype,
                 running->address, running->size, rep ? "rep-v1" : "legacy/unknown");
    } else {
        ESP_LOGE(TAG, "Build profile=%s; active partition unavailable", profile);
    }
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "storage");
    ESP_LOGI(TAG, "LittleFS partition %s", partition != NULL ? "available" : "unavailable");
    if (!rep || strcmp(profile, "rep") != 0 || running == NULL ||
        running->type != ESP_PARTITION_TYPE_APP ||
        (running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) {
        ESP_LOGW(TAG, "Storage not mounted: REP build and exact REP layout required; total/used unavailable");
        return strcmp(profile, "rep") != 0;
    }
    roundie_storage_result_t result = roundie_storage_init(&storage);
    if (result == ROUNDIE_STORAGE_OK) {
        result = roundie_storage_mount(storage);
    }
    roundie_storage_status_t status;
    if (result == ROUNDIE_STORAGE_OK) {
        result = roundie_storage_capacity(storage, &status);
    }
    roundie_storage_status(storage, &status);
    ESP_LOGI(TAG, "LittleFS mounted=%u result=%s underlying=%s capacity_valid=%u total=%zu used=%zu",
             status.mounted, roundie_storage_result_name(result),
             esp_err_to_name(status.underlying_error), status.capacity_valid,
             status.total_bytes, status.used_bytes);
    if (!status.mounted) {
        roundie_storage_deinit(&storage);
    } else if (result == ROUNDIE_STORAGE_OK) {
        migration_filesystem_test();
    }
    return status.mounted && result == ROUNDIE_STORAGE_OK;
}
