#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "roundie_elf_validate.h"

typedef enum {
    ROUNDIE_ELF_UNLOADED, ROUNDIE_ELF_LOADING, ROUNDIE_ELF_READY,
    ROUNDIE_ELF_RUNNING, ROUNDIE_ELF_STOPPING, ROUNDIE_ELF_UNLOADING,
    ROUNDIE_ELF_INITIALIZED, ROUNDIE_ELF_STARTING, ROUNDIE_ELF_SUSPENDING,
    ROUNDIE_ELF_SUSPENDED, ROUNDIE_ELF_RESUMING, ROUNDIE_ELF_STOPPED,
} roundie_elf_state_t;

#define ROUNDIE_ELF_EVENT_CAPACITY 8U
typedef enum {
    ROUNDIE_ELF_SUSPEND = 1, ROUNDIE_ELF_RESUME, ROUNDIE_ELF_STOP, ROUNDIE_ELF_UNLOAD,
} roundie_elf_control_t;

typedef struct {
    uint32_t internal_free;
    uint32_t internal_largest;
    uint32_t psram_free;
    uint32_t exec_free; /* Heap used by loader code allocation (PSRAM in 3A),
                        * not a claim that every byte of that heap executes. */
    uint32_t exec_largest;
} roundie_elf_memory_t;

typedef struct {
    roundie_elf_state_t state;
    uint32_t instance;
    uint32_t completed;
    int64_t completed_us; /* Monotonic unload completion; input retirement follows. */
    uint32_t code_bytes;
    uint32_t data_bytes;
    uint32_t alloc_bytes;
    uint32_t alloc_peak;
    uint32_t api_calls;
    uint32_t load_us;
    uint32_t execute_us;
    uint32_t unload_us;
    int loader_result;
    int32_t app_result;
    bool executed;
    bool over_budget;
    uint32_t abi_version;
    uint32_t callbacks;
    uint32_t callback_max_us;
    uint32_t init_us;
    uint32_t suspend_us;
    uint32_t resume_us;
    uint32_t shutdown_us;
    uint32_t events_delivered;
    uint32_t timers_delivered;
    uint32_t users_delivered;
    uint32_t ui_delivered;
    uint32_t last_value;
    uint32_t latency_max_us;
    uint32_t queue_depth;
    uint32_t events_dropped;
    uint32_t events_coalesced;
    uint32_t events_rejected;
    uint32_t resources_acquired;
    uint32_t resources_released;
    uint32_t resources_reclaimed;
    uint32_t resources_outstanding;
    uint32_t timer_handle;
    uint32_t subscriptions;
    uint32_t control_submitted;
    uint32_t control_completed;
    int32_t control_result;
    char error[ROUNDIE_ELF_ERROR_SIZE];
    roundie_elf_memory_t before;
    roundie_elf_memory_t relocated;
    roundie_elf_memory_t during;
    roundie_elf_memory_t after;
} roundie_elf_status_t;

/* Single system worker owns staging/Flash policy. No initialization on boot.
 * Submission transfers input ONLY on ESP_OK. RUN accepts only ABI 1; START
 * accepts ABI 2/3. Controls acknowledge admission; observe completion via
 * control_completed/result. No synchronous waiting for application callbacks. */
esp_err_t roundie_elf_submit(uint8_t *bytes, size_t size);
esp_err_t roundie_elf_start(uint8_t *bytes, size_t size);
esp_err_t roundie_elf_control(uint32_t instance, roundie_elf_control_t operation);
esp_err_t roundie_elf_post(uint32_t instance, uint32_t value);
bool roundie_elf_busy(void);

/* App storage (rep_storage_v1), worker only, while roundie_elf_busy() is false.
 * set gives the next submitted instance its stored blob; it fails while a
 * written blob has not been taken. clear withdraws it after a failed
 * submission. take returns true, once, if the last instance wrote its blob,
 * copying up to REP_STORAGE_CAPACITY (rep/rep_storage_v1.h) bytes. */
bool roundie_elf_storage_set(const void *data, size_t length);
void roundie_elf_storage_clear(void);
bool roundie_elf_storage_take(void *data, size_t *length);
void roundie_elf_status(roundie_elf_status_t *out);
void roundie_elf_set_allowed(bool allowed);

/* Local wall time for REP3_CAP_CLOCK applications. The source is sampled by the
 * executor at most once per second, outside native callbacks; it returns false
 * while wall time is not valid. revision changes on time sync or zone change. */
typedef struct {
    int64_t monotonic_us; /* esp_timer time at which the fields below hold */
    uint32_t days;        /* local days since 1970-01-01 */
    uint32_t ms_of_day;   /* local milliseconds since midnight */
    uint32_t revision;
} roundie_elf_clock_t;
typedef bool (*roundie_elf_clock_source_t)(roundie_elf_clock_t *out);
/* Before the executor starts; NULL disables clock events. */
void roundie_elf_set_clock_source(roundie_elf_clock_source_t source);
