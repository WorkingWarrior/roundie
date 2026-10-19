#pragma once
#include "roundie_packages.h"

typedef enum {
    ROUNDIE_NAV_WAITING, ROUNDIE_NAV_REFRESHING, ROUNDIE_NAV_LIST,
    ROUNDIE_NAV_REQUESTED, ROUNDIE_NAV_LOADING, ROUNDIE_NAV_ACTIVE,
    ROUNDIE_NAV_EXITING, ROUNDIE_NAV_ERROR,
} roundie_navigation_t;

typedef struct {
    char id[ROUNDIE_PACKAGE_ID];
    char name[ROUNDIE_PACKAGE_NAME];
    uint16_t version[3];
    uint16_t abi;
    roundie_package_health_t health;
} roundie_launcher_entry_t;

typedef struct {
    roundie_navigation_t state;
    uint32_t revision;
    uint32_t request;
    uint32_t generation;
    uint32_t count;
    uint32_t physical;
    uint32_t injected;
    uint32_t rejected;
    uint32_t refresh_us;
    uint32_t discovery_flushes;
    uint32_t copy_us;
    uint32_t enqueue_max_us;
    uint32_t admission_us;
    uint32_t started_us;
    uint32_t exit_admission_us;
    uint32_t return_us;
    uint32_t gui_create_us;
    uint32_t gui_latency_max_us;
    uint32_t gui_objects;
    uint32_t pool_free;
    uint32_t stack_min;
    int64_t changed_us;
    int32_t launch_result;
    int32_t unload_result;
    int32_t app_result;
    bool launch_pending;
    bool exit_pending;
    bool foreground;
    char selected[ROUNDIE_PACKAGE_ID];
    roundie_launcher_entry_t entries[ROUNDIE_PACKAGES_MAX];
} roundie_launcher_status_t;

/* Bounded, copied system admission. origin is TOUCH=1 or INJECTED=2.
 * Neither entry point calls filesystem, LVGL or native code. */
esp_err_t roundie_launcher_select(const char *id, uint32_t revision, uint32_t origin);
esp_err_t roundie_launcher_exit(uint32_t generation);
esp_err_t roundie_launcher_refresh(void);
void roundie_launcher_status(roundie_launcher_status_t *out);
/* Sole existing system worker; allowed means confirmed normal/STABLE policy. */
void roundie_launcher_poll(bool allowed, bool transfer_busy);
/* Existing GUI task only; safe closes UI admission before Safe Mode cleanup. */
void roundie_launcher_gui_bind(void);
void roundie_launcher_gui_pump(bool safe);
