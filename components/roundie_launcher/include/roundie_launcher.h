#pragma once
#include "roundie_packages.h"

typedef enum {
    ROUNDIE_NAV_WAITING, ROUNDIE_NAV_REFRESHING, ROUNDIE_NAV_LIST,
    ROUNDIE_NAV_REQUESTED, ROUNDIE_NAV_LOADING, ROUNDIE_NAV_ACTIVE,
    ROUNDIE_NAV_EXITING, ROUNDIE_NAV_ERROR, ROUNDIE_NAV_RESULT,
    ROUNDIE_NAV_HOME, /* the system watch face (WatchFaceHost) is the foreground */
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
    uint32_t result_request;
    uint32_t completed;
    int32_t loader_result;
    bool executed;
    bool result_success;
    bool result_visible;
    uint32_t result_apply_us;
    uint32_t completion_to_gui_us;
    uint32_t painted_state;
    uint32_t painted_request;
    uint32_t painted_generation;
    uint32_t gui_ready_us;
    int64_t gui_applied_us;
    int64_t completed_us;
    char selected_name[ROUNDIE_PACKAGE_NAME];
    char error[ROUNDIE_ELF_ERROR_SIZE];
    bool launch_pending;
    bool exit_pending;
    bool foreground;
    bool home_active;    /* the observed application is the home package */
    bool home_available; /* the home package is listed and healthy */
    bool home_face;      /* Home is the system watch face (set_home_face) */
    char selected[ROUNDIE_PACKAGE_ID];
    roundie_launcher_entry_t entries[ROUNDIE_PACKAGES_MAX];
} roundie_launcher_status_t;

/* Bounded, copied system admission. origin is TOUCH=1 or INJECTED=2.
 * Neither entry point calls filesystem, LVGL or native code. */
esp_err_t roundie_launcher_select(const char *id, uint32_t revision, uint32_t origin);
esp_err_t roundie_launcher_exit(uint32_t generation);
/* Dismiss a copied, fully unloaded result; never submits native work. */
esp_err_t roundie_launcher_back(uint32_t request);
esp_err_t roundie_launcher_refresh(void);
/* The home package of every platform: the watch face (docs/system-navigation.md). */
#define ROUNDIE_LAUNCHER_HOME_PACKAGE "org.roundie.watch"
/* Home (docs/system-navigation.md): the package launched when the list first
 * appears (autostart) and after any other application exits normally. Its
 * exit shows the list. Call once, before the worker polls; "" disables. */
void roundie_launcher_set_home(const char *id, bool autostart);
/* Home is the system WatchFaceHost instead of a package (M10,
 * docs/watch-face-platform.md): HOME is shown when the list first appears
 * (autostart, also without storage admission) and after any application
 * exits normally. Call once, before the worker polls; replaces set_home. */
void roundie_launcher_set_home_face(bool autostart);
/* From the list: show Home (the face) or launch the home package. Same
 * admission as a selection for a package. */
esp_err_t roundie_launcher_home(void);
/* From Home (the face): show the application list; scans first when no
 * current list exists. */
esp_err_t roundie_launcher_open_list(void);
/* Sole worker, for light sleep and power-menu restarts: end the foreground
 * application through its normal lifecycle (ESP_OK also while that unload is
 * in progress); resume launches home, also when another application was
 * parked (M8-B: a running application does not block sleep by itself; one
 * whose unload does not finish in time still refuses it). */
esp_err_t roundie_launcher_park_home(void);
void roundie_launcher_resume_home(void);
void roundie_launcher_status(roundie_launcher_status_t *out);
/* Sole existing system worker; allowed means confirmed normal/STABLE policy. */
void roundie_launcher_poll(bool allowed, bool transfer_busy);
/* Quick settings, opened by a swipe down from the top edge of the home
 * application (docs/quick-settings.md). The actions run on the GUI task and
 * must not block. battery returns false without a valid charge reading;
 * brightness returns the backlight percent, after one step when cycle is set.
 * sleep returns whether the request was accepted. */
typedef struct {
    bool (*battery)(uint8_t *percent, bool *charging);
    uint8_t (*brightness)(bool cycle);
    void (*screen_off)(void);
    bool (*sleep)(void);
} roundie_launcher_quick_t;
/* GUI task, before the first pump; quick stays owned by the caller. NULL
 * disables the gesture. */
void roundie_launcher_gui_set_quick_settings(const roundie_launcher_quick_t *quick);
/* Existing GUI task only; safe closes UI admission before Safe Mode cleanup. */
void roundie_launcher_gui_bind(void);
void roundie_launcher_gui_pump(bool safe);
