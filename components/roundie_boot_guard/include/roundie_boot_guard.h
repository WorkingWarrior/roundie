#pragma once

#include <stdbool.h>
#include <stddef.h>
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

/* Safe Mode cause diagnostics (docs/safe-mode.md "Latch cause"; E0 1.85B
 * report §9.4 items 1, 2 and 4). Data only: nothing here is read by the boot
 * guard or Safe Mode logic, and no Safe Mode decision depends on it.
 * - A private RTC record keeps, across software, panic and watchdog resets of
 *   any image, the code of the last unplanned esp_restart() site and the last
 *   battery sample with the uptime; power-on and brownout clear RTC RAM.
 * - After the durable latch commit succeeds, a separate NVS commit writes a
 *   cause record (its failure cannot fail or change the latch): the entry site, the raw reset, the previous stage and session, the
 *   package loaded then, and the previous session's RTC record. */
typedef enum {
    ROUNDIE_RESTART_SITE_NONE,
    ROUNDIE_RESTART_SITE_LCD_TIMEOUT,  /* LCD transfer completion timeout */
    ROUNDIE_RESTART_SITE_LCD_FLUSH,    /* flush while a transfer is pending */
    ROUNDIE_RESTART_SITE_GUI_WATCHDOG, /* GUI task watchdog registration/feed failed */
} roundie_restart_site_t;
typedef enum {
    ROUNDIE_SAFE_SITE_NONE,
    ROUNDIE_SAFE_SITE_RETAINED,       /* decided in roundie_boot_begin from the RTC record */
    ROUNDIE_SAFE_SITE_SERIAL,         /* USB Serial/JTAG driver unavailable */
    ROUNDIE_SAFE_SITE_SYSTEM_WORKER,  /* system worker task not created */
    ROUNDIE_SAFE_SITE_OTHER,          /* any other roundie_boot_enter_safe() */
} roundie_safe_site_t;
typedef struct {
    bool valid;                 /* RTC record of the previous session survived */
    uint8_t image[8];           /* ELF SHA-256 prefix of the image that wrote it */
    uint32_t restart_site;      /* roundie_restart_site_t */
    uint32_t uptime_ms;         /* at the last battery sample or restart site */
    bool battery_valid;
    uint16_t voltage_mv;
    int16_t current_ma;         /* 0 unless the board reports a current */
    uint16_t soc_percent;
    uint16_t battery_flags;
    int32_t temperature_deci_c;
    uint32_t battery_uptime_ms; /* when the battery sample was taken */
    int64_t rtc_unix;           /* wall clock at that sample, 0 when invalid */
} roundie_boot_session_t;
typedef struct {
    bool valid;
    uint32_t latch;             /* roundie_safe_reason_t written with it */
    uint32_t site;              /* roundie_safe_site_t */
    uint32_t raw_reset;         /* esp_reset_reason_t of the boot that wrote it */
    uint32_t previous_stage;
    uint32_t failures;
    bool retained_valid;
    char fault_app[ROUNDIE_BOOT_APP_ID];
    uint8_t image[8];           /* ELF SHA-256 prefix of the image that wrote it */
    roundie_boot_session_t previous;
} roundie_boot_cause_t;
/* Boot owner, right after roundie_boot_begin(). */
void roundie_boot_diag_begin(uint32_t raw_reset, const uint8_t image[32]);
/* Before the esp_restart() of an unplanned restart. */
void roundie_boot_diag_restart_site(roundie_restart_site_t site, int64_t now_us);
/* Before a roundie_boot_enter_safe() whose site is not RETAINED/OTHER. */
void roundie_boot_diag_safe_site(roundie_safe_site_t site);
/* Battery sample from the system worker; kept at most once a minute. */
void roundie_boot_diag_battery(uint16_t voltage_mv, int16_t current_ma, uint16_t soc_percent,
                               uint16_t battery_flags, int32_t temperature_deci_c, int64_t rtc_unix,
                               int64_t now_us);
void roundie_boot_diag_previous(roundie_boot_session_t *out);
/* Fills the cause record for a latch written now (persistence backend). */
void roundie_boot_diag_cause(uint32_t latch, roundie_boot_cause_t *out);
/* The cause record stored after the latch (own NVS commit), read at persistence_init. */
void roundie_boot_diag_stored_cause(roundie_boot_cause_t *out);
void roundie_boot_diag_set_stored_cause(const roundie_boot_cause_t *cause);

/* Bounded diagnostic blobs in the boot_guard NVS namespace (persistence
 * backend). Diagnostics yield to settings: a blob is written only while the
 * partition keeps ROUNDIE_BOOT_DIAG_RESERVE_ENTRIES available entries after
 * the write, in its own commit, and never touches another key. */
#define ROUNDIE_BOOT_DIAG_BLOB_MAX 256U
#define ROUNDIE_BOOT_DIAG_RESERVE_ENTRIES 126U /* one 4 KiB NVS page */
typedef enum {
    ROUNDIE_DIAG_STORED = 0,
    ROUNDIE_DIAG_UNCHANGED = 1, /* identical copy already stored, nothing written */
    ROUNDIE_DIAG_TOO_LARGE = 2,
    ROUNDIE_DIAG_NO_ROOM = 3,   /* would cut into the reserve: skipped */
    ROUNDIE_DIAG_FAILED = 4,    /* NVS unavailable or the write/commit failed */
} roundie_diag_store_t;
typedef struct {
    bool valid;
    uint32_t used_entries;
    uint32_t available_entries;
    uint32_t total_entries;
} roundie_boot_nvs_stats_t;
/* NVS entries a blob of this size needs, as the ESP-IDF NVS stores it:
 * blob index, chunk header and one entry per 32 data bytes. */
uint32_t roundie_boot_diag_entries(size_t size);
roundie_diag_store_t roundie_boot_diag_store(const char *key, const void *data, size_t size);
/* Exact-size read; false when absent, of another size or NVS is unavailable. */
bool roundie_boot_diag_load(const char *key, void *data, size_t size);
void roundie_boot_diag_nvs_stats(roundie_boot_nvs_stats_t *out);
