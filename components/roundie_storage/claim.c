#include "roundie_storage_io.h"

#include <stdatomic.h>

#include "roundie_platform.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;

void roundie_storage_claim_init(void)
{
    if (mutex == NULL) {
        mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    }
}

static void mutex_take(void)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
}

static void mutex_give(void)
{
    xSemaphoreGive(mutex);
}
#else
#include <pthread.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void roundie_storage_claim_init(void)
{
}

static void mutex_take(void)
{
    pthread_mutex_lock(&mutex);
}

static void mutex_give(void)
{
    pthread_mutex_unlock(&mutex);
}
#endif

/* Every holder only checks and sets these flags; no VFS call happens under the
 * mutex (asserted in development builds, docs/storage-io.md requirement 2). */
static _Atomic roundie_thread_t holder = ROUNDIE_THREAD_NONE;
static bool io_step;
static atomic_bool ota_claimed;

static void claim_lock(void)
{
    mutex_take();
    atomic_store(&holder, roundie_thread_self());
}

static void claim_unlock(void)
{
    atomic_store(&holder, ROUNDIE_THREAD_NONE);
    mutex_give();
}

void roundie_storage_claim_io_enter(void)
{
    claim_lock();
    io_step = true;
    claim_unlock();
}

void roundie_storage_claim_io_leave(void)
{
    claim_lock();
    io_step = false;
    claim_unlock();
}

bool roundie_storage_claim_ota(bool (*runtime_busy)(void))
{
    claim_lock();
    /* A held claim is granted again: OTA BEGIN during its own session is the OTA
     * manager's decision, not a conflict with /storage. */
    bool granted = !io_step && (atomic_load(&ota_claimed) || !runtime_busy());
    if (granted) {
        atomic_store(&ota_claimed, true);
    }
    claim_unlock();
    return granted;
}

void roundie_storage_claim_ota_release(void)
{
    claim_lock();
    atomic_store(&ota_claimed, false);
    claim_unlock();
}

bool roundie_storage_claim_ota_active(void)
{
    return atomic_load(&ota_claimed);
}

bool roundie_storage_claim_held(void)
{
    return atomic_load(&holder) == roundie_thread_self();
}

static _Atomic roundie_thread_t owner = ROUNDIE_THREAD_NONE;

void roundie_storage_owner_set(void)
{
    atomic_store(&owner, roundie_thread_self());
}

roundie_storage_access_t roundie_storage_access_check(void)
{
    if (roundie_storage_claim_held()) {
        return ROUNDIE_STORAGE_ACCESS_UNDER_CLAIM;
    }
    roundie_thread_t current = atomic_load(&owner);
    if (current != ROUNDIE_THREAD_NONE && current != roundie_thread_self()) {
        return ROUNDIE_STORAGE_ACCESS_NOT_OWNER;
    }
    return ROUNDIE_STORAGE_ACCESS_OK;
}
