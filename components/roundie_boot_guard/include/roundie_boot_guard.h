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

#define ROUNDIE_BOOT_APP_ID 64U

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
    /* Package loaded when the previous session failed. Names what was loaded,
     * not the cause: the failure may be in the system instead. */
    char fault_app[ROUNDIE_BOOT_APP_ID];
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
/* Empty id means no package is loaded. Written only on launch and unload. */
void roundie_boot_app_loaded(const char *id);
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

/* Boot history (diagnostic only, docs/safe-mode.md "Boot history"): a ring of
 * the last raw reset reasons with the gauge voltage at startup, and a count of
 * brownout resets, in its own NVS namespace. It never influences Safe Mode.
 * At most two small NVS writes per boot; a malformed record is preserved and
 * reported, never overwritten or erased. */
#define ROUNDIE_BOOT_HISTORY_ENTRIES 8U
typedef enum {
    ROUNDIE_HISTORY_UNAVAILABLE, /* no NVS or an I/O error */
    ROUNDIE_HISTORY_OK,
    ROUNDIE_HISTORY_MALFORMED,   /* kept as found; nothing recorded */
} roundie_history_state_t;
typedef struct {
    uint32_t boot;      /* 1-based boot number since the record was created */
    uint16_t raw_reset; /* esp_reset_reason_t */
    uint16_t vbat_mv;   /* gauge voltage at startup, 0 when not read */
} roundie_boot_history_entry_t;
typedef struct {
    roundie_history_state_t state;
    uint32_t boots;
    uint32_t brownouts;
    uint32_t count;     /* valid entries, newest first */
    roundie_boot_history_entry_t entries[ROUNDIE_BOOT_HISTORY_ENTRIES];
} roundie_boot_history_t;
/* Boot owner, once, after nvs_flash_init(). */
void roundie_boot_history_record(bool nvs_available);
/* Once per boot with the first gauge reading; later calls do nothing. */
void roundie_boot_history_battery(uint16_t voltage_mv);
void roundie_boot_history_get(roundie_boot_history_t *out);
