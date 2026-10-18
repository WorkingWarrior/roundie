#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Internal firmware interface; not an application ABI. One owner initializes
 * before sharing, and quiesces all users before deinit. No external VFS users
 * or open files may remain when unmounting. No other component may mount this
 * partition. Lifecycle/status/capacity calls serialize with a bounded mutex wait.
 */
typedef struct roundie_storage roundie_storage_t;

typedef enum {
    ROUNDIE_STORAGE_OK,
    ROUNDIE_STORAGE_NOT_INITIALIZED,
    ROUNDIE_STORAGE_NOT_MOUNTED,
    ROUNDIE_STORAGE_MOUNT_FAILED,
    ROUNDIE_STORAGE_UNAVAILABLE,
    ROUNDIE_STORAGE_INVALID_STATE,
    ROUNDIE_STORAGE_NO_SPACE,
    ROUNDIE_STORAGE_IO_ERROR,
    ROUNDIE_STORAGE_NO_MEMORY,
    ROUNDIE_STORAGE_BUSY,
    ROUNDIE_STORAGE_INVALID_ARGUMENT,
} roundie_storage_result_t;

typedef struct {
    bool initialized;
    bool partition_available;
    bool mounted;
    bool capacity_valid;
    size_t total_bytes;
    size_t used_bytes;
    roundie_storage_result_t result;
    esp_err_t underlying_error;
} roundie_storage_status_t;

/* Fixed designated partition "storage" (data/littlefs), mounted at /storage.
 * A missing partition still produces an initialized manager with UNAVAILABLE.
 * *out must be NULL; ownership transfers only on successful initialization.
 */
roundie_storage_result_t roundie_storage_init(roundie_storage_t **out);
roundie_storage_result_t roundie_storage_mount(roundie_storage_t *storage);
roundie_storage_result_t roundie_storage_unmount(roundie_storage_t *storage);
roundie_storage_result_t roundie_storage_status(roundie_storage_t *storage,
                                               roundie_storage_status_t *out);
/* Refresh capacity. NO_SPACE means used == total, not a write-space guarantee. */
roundie_storage_result_t roundie_storage_capacity(roundie_storage_t *storage,
                                                 roundie_storage_status_t *out);
/* Unmount first. Failure retains ownership and the valid manager for retry. */
roundie_storage_result_t roundie_storage_deinit(roundie_storage_t **storage);
const char *roundie_storage_result_name(roundie_storage_result_t result);
