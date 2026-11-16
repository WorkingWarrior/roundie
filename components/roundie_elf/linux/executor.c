/* Application Executor of the desktop (docs/native-apps.md, N5): the pthread
 * counterpart of executor.c, with the same one-slot request queue and the same
 * shared manager steps. Linux has no task watchdog here; the executor publishes
 * since when its current manager call has been running, and the desktop reports
 * an application that does not return. Nothing ever cancels or kills this
 * thread: native code cannot be stopped safely from outside. */

#include "manager_private.h"
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

/* Signal stack of the executor thread, so a fault report also runs after the
 * application overflowed the thread's stack. */
#define FAULT_STACK_BYTES (64U * 1024U)

typedef struct {
    uint8_t *bytes;
    size_t size;
} request_t;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t arrived = PTHREAD_COND_INITIALIZER;
static request_t pending;
static bool pending_valid;
static bool kicked;
static bool started;
static atomic_bool input_owned;
static _Atomic int64_t busy_since_us;
static _Atomic int64_t stepped_at_us; /* the time the last manager step was given */
static _Thread_local bool executor_thread;

bool roundie_elf_input_busy(void)
{
    return atomic_load(&input_owned);
}

int64_t roundie_elf_executor_busy_since(void)
{
    return atomic_load(&busy_since_us);
}

void roundie_elf_executor_kick(void)
{
    pthread_mutex_lock(&mutex);
    kicked = true;
    pthread_cond_signal(&arrived);
    pthread_mutex_unlock(&mutex);
}

bool roundie_elf_on_executor(void)
{
    return executor_thread;
}

/* Keeps a signal stack a sanitizer runtime already installed. */
static void install_fault_stack(void)
{
    stack_t current;
    if (sigaltstack(NULL, &current) != 0 || (current.ss_flags & SS_DISABLE) == 0) {
        return;
    }
    stack_t fault_stack = {.ss_sp = malloc(FAULT_STACK_BYTES), .ss_size = FAULT_STACK_BYTES, .ss_flags = 0};
    if (fault_stack.ss_sp == NULL || sigaltstack(&fault_stack, NULL) != 0) {
        free(fault_stack.ss_sp); /* reports of a stack overflow are then lost */
    }
}

bool roundie_elf_executor_settled(int64_t now_us)
{
    pthread_mutex_lock(&mutex);
    bool idle = !pending_valid;
    pthread_mutex_unlock(&mutex);
    if (!idle || atomic_load(&busy_since_us) != 0) {
        return false;
    }
    roundie_elf_status_t current;
    roundie_elf_status(&current);
    if (current.control_submitted != current.control_completed || current.queue_depth != 0) {
        return false;
    }
    return current.state == ROUNDIE_ELF_UNLOADED || atomic_load(&stepped_at_us) >= now_us;
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
    while (!pending_valid && !kicked) {
        if (pthread_cond_timedwait(&arrived, &mutex, &deadline) != 0) {
            break;
        }
    }
    kicked = false;
    bool received = pending_valid;
    if (received) {
        *request = pending;
        pending_valid = false;
        /* Under the lock, so the request is never seen neither pending nor busy. */
        atomic_store(&busy_since_us, roundie_time_us());
    }
    pthread_mutex_unlock(&mutex);
    return received;
}

static void *execute(void *unused)
{
    (void)unused;
    executor_thread = true;
    install_fault_stack(); /* lives as long as this thread, which never ends */
    roundie_elf_bind_executor(roundie_thread_self());
    uint8_t *retained = NULL;
    for (;;) {
        request_t request;
        if (receive(&request)) {
            roundie_elf_execute(request.bytes, request.size);
            atomic_store(&busy_since_us, 0);
            retained = request.bytes;
        }
        roundie_elf_status_t current;
        roundie_elf_status(&current);
        if (current.state != ROUNDIE_ELF_UNLOADED || current.control_submitted != current.control_completed) {
            int64_t now = roundie_time_us();
            atomic_store(&busy_since_us, now);
            roundie_elf_step(now);
            atomic_store(&busy_since_us, 0);
            atomic_store(&stepped_at_us, now);
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
