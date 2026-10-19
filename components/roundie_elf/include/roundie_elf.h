#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "roundie_elf_validate.h"

typedef enum {
    ROUNDIE_ELF_UNLOADED, ROUNDIE_ELF_LOADING, ROUNDIE_ELF_READY,
    ROUNDIE_ELF_RUNNING, ROUNDIE_ELF_STOPPING, ROUNDIE_ELF_UNLOADING,
} roundie_elf_state_t;

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
    char error[ROUNDIE_ELF_ERROR_SIZE];
    roundie_elf_memory_t before;
    roundie_elf_memory_t relocated;
    roundie_elf_memory_t during;
    roundie_elf_memory_t after;
} roundie_elf_status_t;

/* System worker owns policy and staging. No initialization/task on boot.
 * submit transfers the input buffer ONLY on ESP_OK. No stop/unload API while
 * executing: this slice has one short callback, followed by automatic cleanup.
 */
esp_err_t roundie_elf_submit(uint8_t *bytes, size_t size);
bool roundie_elf_busy(void);
void roundie_elf_status(roundie_elf_status_t *out);
void roundie_elf_set_allowed(bool allowed);
