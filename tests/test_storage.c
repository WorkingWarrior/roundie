#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "roundie_storage.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "freertos/semphr.h"

static const esp_partition_t partition = {0x710000, 0x8d0000};
static bool partition_present = true;
static bool mounted;
static bool fail_mutex;
static esp_err_t mount_error;
static esp_err_t info_error;
static esp_err_t unmount_error;
static unsigned register_calls;
static unsigned unregister_calls;
static size_t total_bytes = 0x8d0000;
static size_t used_bytes = 8192;
static pthread_mutex_t barrier = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool pause_mount;
static bool mount_entered;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (fail_mutex) {
        return NULL;
    }
    SemaphoreHandle_t mutex = malloc(sizeof(*mutex));
    assert(mutex != NULL);
    assert(pthread_mutex_init(mutex, NULL) == 0);
    return mutex;
}

int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned ticks)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_nsec += (long)ticks * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    return pthread_mutex_timedlock(mutex, &deadline) == 0;
}

void xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(pthread_mutex_unlock(mutex) == 0);
}

void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    assert(pthread_mutex_destroy(mutex) == 0);
    free(mutex);
}

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    assert(type == ESP_PARTITION_TYPE_DATA);
    assert(subtype == ESP_PARTITION_SUBTYPE_DATA_LITTLEFS);
    assert(strcmp(label, "storage") == 0);
    return partition_present ? &partition : NULL;
}

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *config)
{
    assert(!config->format_if_mount_failed);
    assert(!config->grow_on_mount);
    assert(!config->dont_mount);
    assert(strcmp(config->base_path, "/storage") == 0);
    assert(strcmp(config->partition_label, "storage") == 0);
    register_calls++;
    assert(pthread_mutex_lock(&barrier) == 0);
    mount_entered = true;
    assert(pthread_cond_broadcast(&condition) == 0);
    while (pause_mount) {
        assert(pthread_cond_wait(&condition, &barrier) == 0);
    }
    assert(pthread_mutex_unlock(&barrier) == 0);
    if (mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mount_error != ESP_OK) {
        return mount_error;
    }
    mounted = true;
    return ESP_OK;
}

esp_err_t esp_vfs_littlefs_unregister(const char *label)
{
    assert(strcmp(label, "storage") == 0);
    assert(mounted);
    unregister_calls++;
    if (unmount_error == ESP_OK) {
        mounted = false;
    }
    return unmount_error;
}

esp_err_t esp_littlefs_info(const char *label, size_t *total, size_t *used)
{
    assert(strcmp(label, "storage") == 0);
    assert(mounted);
    *total = total_bytes;
    *used = used_bytes;
    return info_error;
}

static void *mount_thread(void *argument)
{
    assert(roundie_storage_mount(argument) == ROUNDIE_STORAGE_OK);
    return NULL;
}

int main(void)
{
    roundie_storage_t *storage = NULL;
    roundie_storage_status_t status;
    assert(roundie_storage_status(NULL, &status) == ROUNDIE_STORAGE_NOT_INITIALIZED);
    assert(!status.initialized && !status.capacity_valid);
    assert(roundie_storage_mount(NULL) == ROUNDIE_STORAGE_NOT_INITIALIZED);
    assert(roundie_storage_init(NULL) == ROUNDIE_STORAGE_INVALID_ARGUMENT);
    fail_mutex = true;
    assert(roundie_storage_init(&storage) == ROUNDIE_STORAGE_NO_MEMORY);
    assert(storage == NULL);
    fail_mutex = false;

    partition_present = false;
    assert(roundie_storage_init(&storage) == ROUNDIE_STORAGE_OK);
    assert(roundie_storage_mount(storage) == ROUNDIE_STORAGE_UNAVAILABLE);
    assert(register_calls == 0);
    assert(roundie_storage_status(storage, &status) == ROUNDIE_STORAGE_OK);
    assert(status.initialized && !status.partition_available && !status.mounted);
    assert(roundie_storage_capacity(storage, &status) == ROUNDIE_STORAGE_UNAVAILABLE);
    assert(!status.capacity_valid);
    assert(roundie_storage_deinit(&storage) == ROUNDIE_STORAGE_OK);
    partition_present = true;

    assert(roundie_storage_init(&storage) == ROUNDIE_STORAGE_OK);
    assert(roundie_storage_init(&storage) == ROUNDIE_STORAGE_INVALID_STATE);
    assert(roundie_storage_capacity(storage, &status) == ROUNDIE_STORAGE_NOT_MOUNTED);
    assert(roundie_storage_unmount(storage) == ROUNDIE_STORAGE_NOT_MOUNTED);
    const esp_err_t errors[] = {ESP_FAIL, ESP_ERR_NO_MEM, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_STATE};
    const roundie_storage_result_t results[] = {ROUNDIE_STORAGE_MOUNT_FAILED,
        ROUNDIE_STORAGE_NO_MEMORY, ROUNDIE_STORAGE_UNAVAILABLE, ROUNDIE_STORAGE_INVALID_STATE};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        mount_error = errors[i];
        assert(roundie_storage_mount(storage) == results[i]);
        assert(roundie_storage_status(storage, &status) == ROUNDIE_STORAGE_OK);
        assert(status.result == results[i] && status.underlying_error == errors[i]);
        assert(!status.mounted && !status.capacity_valid);
        assert(unregister_calls == 0);
    }
    mount_error = ESP_OK;
    assert(roundie_storage_mount(storage) == ROUNDIE_STORAGE_OK);
    unsigned calls = register_calls;
    assert(roundie_storage_mount(storage) == ROUNDIE_STORAGE_INVALID_STATE);
    assert(register_calls == calls);
    assert(roundie_storage_capacity(storage, &status) == ROUNDIE_STORAGE_OK);
    assert(status.capacity_valid && status.total_bytes == total_bytes && status.used_bytes == used_bytes);
    info_error = ESP_FAIL;
    assert(roundie_storage_capacity(storage, &status) == ROUNDIE_STORAGE_IO_ERROR);
    assert(!status.capacity_valid && status.total_bytes == 0 && status.used_bytes == 0);
    info_error = ESP_OK;
    used_bytes = total_bytes + 1;
    assert(roundie_storage_capacity(storage, &status) == ROUNDIE_STORAGE_IO_ERROR);
    used_bytes = total_bytes;
    assert(roundie_storage_capacity(storage, &status) == ROUNDIE_STORAGE_NO_SPACE);
    assert(status.capacity_valid);
    used_bytes = 8192;

    /* A second manager must not take ownership of an existing VFS mount. */
    roundie_storage_t *other = NULL;
    assert(roundie_storage_init(&other) == ROUNDIE_STORAGE_OK);
    assert(roundie_storage_mount(other) == ROUNDIE_STORAGE_INVALID_STATE);
    assert(roundie_storage_deinit(&other) == ROUNDIE_STORAGE_OK);
    assert(mounted && unregister_calls == 0);

    unmount_error = ESP_FAIL;
    assert(roundie_storage_deinit(&storage) == ROUNDIE_STORAGE_IO_ERROR);
    assert(storage != NULL && mounted);
    assert(roundie_storage_status(storage, &status) == ROUNDIE_STORAGE_OK);
    assert(status.mounted && status.underlying_error == ESP_FAIL);
    unmount_error = ESP_OK;
    assert(roundie_storage_unmount(storage) == ROUNDIE_STORAGE_OK);
    assert(roundie_storage_status(storage, &status) == ROUNDIE_STORAGE_OK);
    assert(!status.mounted && !status.capacity_valid);

    /* Hold a real pthread mutex inside a blocked backend; callers time out. */
    pause_mount = true;
    mount_entered = false;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, mount_thread, storage) == 0);
    assert(pthread_mutex_lock(&barrier) == 0);
    while (!mount_entered) {
        assert(pthread_cond_wait(&condition, &barrier) == 0);
    }
    assert(pthread_mutex_unlock(&barrier) == 0);
    assert(roundie_storage_status(storage, &status) == ROUNDIE_STORAGE_BUSY);
    assert(!status.capacity_valid);
    assert(roundie_storage_unmount(storage) == ROUNDIE_STORAGE_BUSY);
    assert(pthread_mutex_lock(&barrier) == 0);
    pause_mount = false;
    assert(pthread_cond_broadcast(&condition) == 0);
    assert(pthread_mutex_unlock(&barrier) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(roundie_storage_deinit(&storage) == ROUNDIE_STORAGE_OK);
    assert(storage == NULL && !mounted);
    assert(roundie_storage_deinit(&storage) == ROUNDIE_STORAGE_NOT_INITIALIZED);
    puts("StorageManager host tests: PASS (mock VFS/partition, pthread synchronization)");
    return 0;
}
