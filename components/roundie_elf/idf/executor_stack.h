#pragma once
/* Guarded stack of the Application Executor (M9.1, ESP-IDF only).
 * docs/elf-runtime-hardening-m9-1.md describes the layout and its limits. */
#include <stdbool.h>
#include <stdint.h>
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

/* Bits of roundie_elf_stack_recheck()'s lost mask: what differed from the
 * armed configuration when the check started. */
#define ROUNDIE_ELF_GUARD_LOST_MMU_GUARD 0x01U   /* the guard page is mapped again */
#define ROUNDIE_ELF_GUARD_LOST_MMU_STACK 0x02U   /* the stack page is no longer mapped */
#define ROUNDIE_ELF_GUARD_LOST_SP_MIN 0x04U      /* SP_MIN is not the red-zone limit */
#define ROUNDIE_ELF_GUARD_LOST_SP_MAX 0x08U
#define ROUNDIE_ELF_GUARD_LOST_ENABLE 0x10U      /* SP_SPILL_MIN monitor disabled */
#define ROUNDIE_ELF_GUARD_LOST_RELEASE 0x20U     /* its interrupt signal gated */
#define ROUNDIE_ELF_GUARD_LOST_UNSTABLE 0x40U    /* SP_UNSTABLE changed */
#define ROUNDIE_ELF_GUARD_LOST_NOT_ARMED 0x80U   /* never armed, or not on the executor */

/* Executor task only, after a light sleep (M8-B) and before any native code
 * runs: records which parts of the guard differ from the armed state, then
 * re-arms (SP_UNSTABLE, monitor bounds, self-test). A guard page that is
 * mapped again, or an unmapped stack page, is not repaired: false. false:
 * native applications must not run (fail closed, as M9.1). */
bool roundie_elf_stack_recheck(uint32_t *lost);
