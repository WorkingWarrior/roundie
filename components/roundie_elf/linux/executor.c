/* Application Executor of the desktop (docs/native-apps.md, N5): the pthread
 * counterpart of executor.c, with the same one-slot request queue and the same
 * shared manager steps. Linux has no task watchdog here; the executor publishes
 * since when its current manager call has been running, and the desktop reports
 * an application that does not return. Nothing ever cancels or kills this
 * thread: native code cannot be stopped safely from outside. */

#include "manager_private.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    uint8_t *bytes;
    size_t size;
} request_t;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t arrived = PTHREAD_COND_INITIALIZER;
static request_t pending;
static bool pending_valid;
static bool started;
static atomic_bool input_owned;
static _Atomic int64_t busy_since_us;

bool roundie_elf_input_busy(void)
{
    return atomic_load(&input_owned);
}

int64_t roundie_elf_executor_busy_since(void)
{
    return atomic_load(&busy_since_us);
}

static bool receive(request_t *request)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 10 * 1000 * 1000;
    if (deadline.tv_nsec >= 1000 * 1000 * 1000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000 * 1000 * 1000;
    }
    pthread_mutex_lock(&mutex);
    while (!pending_valid) {
        if (pthread_cond_timedwait(&arrived, &mutex, &deadline) != 0) {
            break;
        }
    }
    bool received = pending_valid;
    if (received) {
        *request = pending;
        pending_valid = false;
    }
    pthread_mutex_unlock(&mutex);
    return received;
}

static void *execute(void *unused)
{
    (void)unused;
    roundie_elf_bind_executor(roundie_thread_self());
    uint8_t *retained = NULL;
    for (;;) {
        request_t request;
        if (receive(&request)) {
            atomic_store(&busy_since_us, roundie_time_us());
            roundie_elf_execute(request.bytes, request.size);
            atomic_store(&busy_since_us, 0);
            retained = request.bytes;
        }
        roundie_elf_status_t current;
        roundie_elf_status(&current);
        if (current.state != ROUNDIE_ELF_UNLOADED || current.control_submitted != current.control_completed) {
            atomic_store(&busy_since_us, roundie_time_us());
            roundie_elf_step(roundie_time_us());
            atomic_store(&busy_since_us, 0);
        }
        roundie_elf_status(&current);
        if (current.state == ROUNDIE_ELF_UNLOADED && retained != NULL) {
            free(retained);
            retained = NULL;
            atomic_store(&input_owned, false);
        }
    }
    return NULL;
}

static esp_err_t submit(uint8_t *bytes, size_t size, uint32_t abi_version)
{
    roundie_elf_info_t info;
    char error[ROUNDIE_ELF_ERROR_SIZE];
    if (!roundie_elf_validate(bytes, size, &info, error) ||
        (info.abi_version != abi_version && !(abi_version == 2 && info.abi_version == 3))) {
        return ESP_ERR_INVALID_ARG;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&input_owned, &expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!roundie_elf_prepare()) {
        atomic_store(&input_owned, false);
        return ESP_ERR_INVALID_STATE;
    }
    pthread_mutex_lock(&mutex);
    if (!started) {
        pthread_t thread;
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        started = pthread_create(&thread, &attributes, execute, NULL) == 0;
        pthread_attr_destroy(&attributes);
    }
    if (!started || pending_valid) {
        bool no_thread = !started;
        pthread_mutex_unlock(&mutex);
        roundie_elf_cancel_prepare();
        atomic_store(&input_owned, false);
        return no_thread ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }
    pending = (request_t){bytes, size};
    pending_valid = true;
    pthread_cond_signal(&arrived);
    pthread_mutex_unlock(&mutex);
    return ESP_OK;
}

esp_err_t roundie_elf_submit(uint8_t *bytes, size_t size)
{
    return submit(bytes, size, 1);
}

esp_err_t roundie_elf_start(uint8_t *bytes, size_t size)
{
    return submit(bytes, size, 2);
}
