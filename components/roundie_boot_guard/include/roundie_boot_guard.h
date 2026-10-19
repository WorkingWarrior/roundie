#pragma once

#include <stdbool.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* Internal core interface, not the application SDK. Engine calls serialize on
 * the IDF critical-section lock; persistence is boot-owner/worker-only. */
typedef enum {
    ROUNDIE_RESET_UNKNOWN, ROUNDIE_RESET_POWER, ROUNDIE_RESET_SOFTWARE,
    ROUNDIE_RESET_SLEEP, ROUNDIE_RESET_WATCHDOG, ROUNDIE_RESET_PANIC,
    ROUNDIE_RESET_BROWNOUT, ROUNDIE_RESET_EXTERNAL,
} roundie_reset_class_t;

typedef enum {
    ROUNDIE_BOOT_START, ROUNDIE_BOOT_CORE, ROUNDIE_BOOT_GUI,
    ROUNDIE_BOOT_OPERATIONAL, ROUNDIE_BOOT_STABLE,
} roundie_boot_stage_t;

typedef enum {
    ROUNDIE_SAFE_NONE, ROUNDIE_SAFE_REQUEST, ROUNDIE_SAFE_BOOT_LOOP,
    ROUNDIE_SAFE_STORAGE, ROUNDIE_SAFE_GUI, ROUNDIE_SAFE_CORE,
    ROUNDIE_SAFE_PERSISTENCE,
} roundie_safe_reason_t;

typedef struct {
    bool safe;
    bool retained_valid;
    bool persistence_ok;
    bool skip_gui;
    bool trial;
    uint32_t failures;
    uint32_t fault_remaining;
    uint32_t stage;
    uint32_t previous_stage;
    uint32_t reset;
    uint32_t reason;
    uint32_t latch;
} roundie_boot_status_t;

#define ROUNDIE_BOOT_FAILURE_LIMIT 3U
#ifndef CONFIG_ROUNDIE_BOOT_STABILITY_SECONDS
#define CONFIG_ROUNDIE_BOOT_STABILITY_SECONDS 10
#endif
#define ROUNDIE_BOOT_STABILITY_US (INT64_C(1000000) * CONFIG_ROUNDIE_BOOT_STABILITY_SECONDS)

/* begin is exclusive early boot, before NVS, storage or board initialization.
 * No state from another ELF identity is interpreted as a failed boot. */
void roundie_boot_begin(roundie_reset_class_t reset, const uint8_t image[32]);
void roundie_boot_persistence_loaded(bool available, roundie_safe_reason_t latch);
void roundie_boot_status(roundie_boot_status_t *out);
void roundie_boot_enter_safe(roundie_safe_reason_t reason);
void roundie_boot_stage(roundie_boot_stage_t stage);
void roundie_boot_operational(int64_t now_us, bool core_passed);
void roundie_boot_tick(int64_t now_us);
void roundie_boot_planned_restart(void);
/* CLEAR keeps the current recovery session active. REBOOT grants exactly one
 * normal trial; the durable latch is cleared only after a stable trial. */
bool roundie_boot_clear(void);
bool roundie_boot_normal_trial(void);
bool roundie_boot_fault_arm(void);
bool roundie_boot_fault_step(bool confirmed_valid);
const char *roundie_boot_reason_name(uint32_t reason);

/* Thin IDF backend. No erase fallback; writes only when latch changes. */
void roundie_boot_persistence_init(bool nvs_available);
bool roundie_boot_persistence_sync(void);
roundie_reset_class_t roundie_boot_reset_class(void);
