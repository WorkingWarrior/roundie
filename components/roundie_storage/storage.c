#include "roundie_storage.h"

#include <stdlib.h>
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define STORAGE_LABEL "storage"
#define STORAGE_PATH "/storage"

struct roundie_storage {
    SemaphoreHandle_t mutex;
    const esp_partition_t *partition;
    roundie_storage_status_t status;
};

static roundie_storage_result_t record_error(roundie_storage_t *storage,
                                             roundie_storage_result_t result,
                                             esp_err_t error)
{
    storage->status.result = result;
    storage->status.underlying_error = error;
    return result;
}

static roundie_storage_result_t lock_storage(roundie_storage_t *storage)
{
    if (storage == NULL) {
        return ROUNDIE_STORAGE_NOT_INITIALIZED;
    }
    if (xSemaphoreTake(storage->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ROUNDIE_STORAGE_BUSY;
    }
    return ROUNDIE_STORAGE_OK;
}

static void clear_capacity(roundie_storage_t *storage)
{
    storage->status.capacity_valid = false;
    storage->status.total_bytes = 0;
    storage->status.used_bytes = 0;
}

roundie_storage_result_t roundie_storage_init(roundie_storage_t **out)
{
    if (out == NULL) {
        return ROUNDIE_STORAGE_INVALID_ARGUMENT;
    }
    if (*out != NULL) {
        return ROUNDIE_STORAGE_INVALID_STATE;
    }
    roundie_storage_t *storage = calloc(1, sizeof(*storage));
    if (storage == NULL) {
        return ROUNDIE_STORAGE_NO_MEMORY;
    }
    storage->mutex = xSemaphoreCreateMutex();
    if (storage->mutex == NULL) {
        free(storage);
        return ROUNDIE_STORAGE_NO_MEMORY;
    }
    storage->partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                  ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
                                                  STORAGE_LABEL);
    storage->status.initialized = true;
    storage->status.partition_available = storage->partition != NULL;
    record_error(storage, storage->partition != NULL ? ROUNDIE_STORAGE_NOT_MOUNTED :
                 ROUNDIE_STORAGE_UNAVAILABLE, storage->partition != NULL ? ESP_OK :
                 ESP_ERR_NOT_FOUND);
    *out = storage;
    return ROUNDIE_STORAGE_OK;
}

roundie_storage_result_t roundie_storage_mount(roundie_storage_t *storage)
{
    roundie_storage_result_t result = lock_storage(storage);
    if (result != ROUNDIE_STORAGE_OK) {
        return result;
    }
    if (storage->status.mounted) {
        result = record_error(storage, ROUNDIE_STORAGE_INVALID_STATE, ESP_ERR_INVALID_STATE);
    } else if (storage->partition == NULL) {
        result = record_error(storage, ROUNDIE_STORAGE_UNAVAILABLE, ESP_ERR_NOT_FOUND);
    } else {
        const esp_vfs_littlefs_conf_t config = {
            .base_path = STORAGE_PATH,
            .partition_label = STORAGE_LABEL,
            .format_if_mount_failed = false,
            .read_only = false,
            .dont_mount = false,
            .grow_on_mount = false,
        };
        esp_err_t error = esp_vfs_littlefs_register(&config);
        result = ROUNDIE_STORAGE_MOUNT_FAILED;
        if (error == ESP_OK) {
            storage->status.mounted = true;
            result = ROUNDIE_STORAGE_OK;
        } else if (error == ESP_ERR_INVALID_STATE) {
            result = ROUNDIE_STORAGE_INVALID_STATE;
        } else if (error == ESP_ERR_NOT_FOUND) {
            result = ROUNDIE_STORAGE_UNAVAILABLE;
        } else if (error == ESP_ERR_NO_MEM) {
            result = ROUNDIE_STORAGE_NO_MEMORY;
        }
        clear_capacity(storage);
        record_error(storage, result, error);
    }
    xSemaphoreGive(storage->mutex);
    return result;
}

static roundie_storage_result_t unmount_locked(roundie_storage_t *storage)
{
    if (!storage->status.mounted) {
        return record_error(storage, ROUNDIE_STORAGE_NOT_MOUNTED, ESP_ERR_INVALID_STATE);
    }
    clear_capacity(storage);
    esp_err_t error = esp_vfs_littlefs_unregister(STORAGE_LABEL);
    if (error != ESP_OK) {
        /* Retain ownership: never free state after an unsuccessful unmount. */
        return record_error(storage, ROUNDIE_STORAGE_IO_ERROR, error);
    }
    storage->status.mounted = false;
    clear_capacity(storage);
    return record_error(storage, ROUNDIE_STORAGE_OK, ESP_OK);
}

roundie_storage_result_t roundie_storage_unmount(roundie_storage_t *storage)
{
    roundie_storage_result_t result = lock_storage(storage);
    if (result == ROUNDIE_STORAGE_OK) {
        result = unmount_locked(storage);
        xSemaphoreGive(storage->mutex);
    }
    return result;
}

static roundie_storage_result_t snapshot(roundie_storage_t *storage,
                                        roundie_storage_status_t *out, bool refresh)
{
    if (out == NULL) {
        return ROUNDIE_STORAGE_INVALID_ARGUMENT;
    }
    *out = (roundie_storage_status_t){
        .result = ROUNDIE_STORAGE_NOT_INITIALIZED,
        .underlying_error = ESP_ERR_INVALID_STATE,
    };
    roundie_storage_result_t result = lock_storage(storage);
    if (result != ROUNDIE_STORAGE_OK) {
        out->result = result;
        return result;
    }
    if (refresh) {
        clear_capacity(storage);
        if (!storage->status.mounted) {
            result = record_error(storage, storage->partition == NULL ?
                ROUNDIE_STORAGE_UNAVAILABLE : ROUNDIE_STORAGE_NOT_MOUNTED,
                ESP_ERR_INVALID_STATE);
        } else {
            size_t total = 0;
            size_t used = 0;
            esp_err_t error = esp_littlefs_info(STORAGE_LABEL, &total, &used);
            if (error != ESP_OK || total == 0 || used > total) {
                result = record_error(storage, ROUNDIE_STORAGE_IO_ERROR,
                                      error == ESP_OK ? ESP_ERR_INVALID_SIZE : error);
            } else {
                storage->status.total_bytes = total;
                storage->status.used_bytes = used;
                storage->status.capacity_valid = true;
                result = record_error(storage, used == total ? ROUNDIE_STORAGE_NO_SPACE :
                                      ROUNDIE_STORAGE_OK, ESP_OK);
            }
        }
    }
    *out = storage->status;
    xSemaphoreGive(storage->mutex);
    return result;
}

roundie_storage_result_t roundie_storage_status(roundie_storage_t *storage,
                                               roundie_storage_status_t *out)
{
    return snapshot(storage, out, false);
}

roundie_storage_result_t roundie_storage_capacity(roundie_storage_t *storage,
                                                 roundie_storage_status_t *out)
{
    return snapshot(storage, out, true);
}

roundie_storage_result_t roundie_storage_deinit(roundie_storage_t **storage)
{
    if (storage == NULL || *storage == NULL) {
        return ROUNDIE_STORAGE_NOT_INITIALIZED;
    }
    roundie_storage_t *owned = *storage;
    roundie_storage_result_t result = lock_storage(owned);
    if (result != ROUNDIE_STORAGE_OK) {
        return result;
    }
    if (owned->status.mounted) {
        result = unmount_locked(owned);
    }
    xSemaphoreGive(owned->mutex);
    if (result != ROUNDIE_STORAGE_OK) {
        return result;
    }
    vSemaphoreDelete(owned->mutex);
    free(owned);
    *storage = NULL;
    return ROUNDIE_STORAGE_OK;
}

const char *roundie_storage_result_name(roundie_storage_result_t result)
{
    switch (result) {
    case ROUNDIE_STORAGE_OK: return "ok";
    case ROUNDIE_STORAGE_NOT_INITIALIZED: return "not initialized";
    case ROUNDIE_STORAGE_NOT_MOUNTED: return "not mounted";
    case ROUNDIE_STORAGE_MOUNT_FAILED: return "mount failed";
    case ROUNDIE_STORAGE_UNAVAILABLE: return "filesystem unavailable";
    case ROUNDIE_STORAGE_INVALID_STATE: return "invalid state";
    case ROUNDIE_STORAGE_NO_SPACE: return "no space";
    case ROUNDIE_STORAGE_IO_ERROR: return "I/O error";
    case ROUNDIE_STORAGE_NO_MEMORY: return "no memory";
    case ROUNDIE_STORAGE_BUSY: return "busy";
    case ROUNDIE_STORAGE_INVALID_ARGUMENT: return "invalid argument";
    }
    return "unknown";
}
