#pragma once
/* Guarded stack of the Application Executor (M9.1, ESP-IDF only).
 * docs/elf-runtime-hardening-m9-1.md describes the layout and its limits. */
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Creates the executor task on its guarded PSRAM stack, pinned to the
 * monitored core. Called once from a task with an internal-RAM stack (the
 * guard page is unmapped with the caches frozen). false: no executor; the
 * guard page, once unmapped, is kept and never returned to the heap. */
bool roundie_elf_stack_create(TaskFunction_t entry, TaskHandle_t *task);

/* Executor task only, before the first request: allocates the stack-pointer
 * monitor interrupt on this core, self-tests the monitor and arms it. false:
 * the guard is not active and native applications must not run. */
bool roundie_elf_stack_arm(void);
