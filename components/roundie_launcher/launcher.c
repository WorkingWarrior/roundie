#include "private.h"
#include <limits.h>
#include <string.h>
#include "roundie_platform.h"
#include "roundie_elf.h"
#include "roundie_app_ui.h"

static roundie_lock_t lock = ROUNDIE_LOCK_INITIALIZER;
static roundie_launcher_status_t status;
static bool gui_ready;
static bool refresh_pending;
static bool snapshot_valid;
static bool observing;
static bool exit_submitted;
static uint32_t package_revision;
static int64_t requested_at;
static int64_t exit_at;
static uint32_t completion_before;
static uint32_t observed_request;
/* Sole worker scratch, deliberately outside its 12 KiB stack. */
static roundie_launcher_entry_t discovered[ROUNDIE_PACKAGES_MAX];
static roundie_package_record_t record;
static char home[ROUNDIE_PACKAGE_ID];
static bool home_due; /* launch home once the list is shown */
static bool home_parked; /* home unloaded for light sleep; relaunch on resume */

static void set_state(roundie_navigation_t state)
{
    status.state = state;
    status.changed_us = roundie_time_us();
}

void roundie_launcher_status(roundie_launcher_status_t *out)
{
    roundie_lock(&lock);
    *out = status;
    roundie_unlock(&lock);
}

/* Locked. origin is TOUCH=1, INJECTED=2 or 0 for the system's home launch. */
static bool admit(const char *id, uint32_t origin, int64_t started)
{
    bool valid = gui_ready && snapshot_valid && status.state == ROUNDIE_NAV_LIST &&
        !refresh_pending && status.request != UINT32_MAX;
    bool found = false;
    for (unsigned i = 0; i < status.count; i++) {
        if (strcmp(id, status.entries[i].id) == 0 && status.entries[i].health == ROUNDIE_PACKAGE_OK) {
            found = true;
        }
    }
    if (!valid || !found) {
        return false;
    }
    strcpy(status.selected, id);
    for (unsigned i = 0; i < status.count; i++) {
        if (strcmp(id, status.entries[i].id) == 0) {
            strcpy(status.selected_name, status.entries[i].name[0] != '\0' ?
                status.entries[i].name : id);
            break;
        }
    }
    status.request++;
    status.generation = 0;
    status.launch_pending = true;
    status.launch_result = 0;
    status.unload_result = 0;
    status.app_result = 0;
    status.result_request = 0;
    status.completed = 0;
    status.completed_us = 0;
    status.executed = false;
    status.result_success = false;
    status.result_visible = false;
    status.loader_result = 0;
    status.error[0] = '\0';
    status.result_apply_us = 0;
    status.completion_to_gui_us = 0;
    status.admission_us = 0;
    status.started_us = 0;
    status.exit_admission_us = 0;
    status.return_us = 0;
    status.physical += origin == 1;
    status.injected += origin == 2;
    status.home_active = home[0] != '\0' && strcmp(id, home) == 0;
    home_due = false;
    requested_at = started;
    set_state(ROUNDIE_NAV_REQUESTED);
    return true;
}

esp_err_t roundie_launcher_select(const char *id, uint32_t revision, uint32_t origin)
{
    if (id == NULL || !roundie_package_id_valid(id) || (origin != 1 && origin != 2)) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t started = roundie_time_us();
    roundie_lock(&lock);
    bool valid = revision == status.revision && admit(id, origin, started);
    if (!valid) {
        status.rejected++;
    }
    uint32_t elapsed = (uint32_t)(roundie_time_us() - started);
    if (elapsed > status.enqueue_max_us) {
        status.enqueue_max_us = elapsed;
    }
    roundie_unlock(&lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void roundie_launcher_set_home(const char *id, bool autostart)
{
    roundie_lock(&lock);
    if (id != NULL && roundie_package_id_valid(id)) {
        strcpy(home, id);
        home_due = autostart;
    } else {
        home[0] = '\0';
        home_due = false;
    }
    roundie_unlock(&lock);
}

esp_err_t roundie_launcher_home(void)
{
    roundie_lock(&lock);
    bool valid = home[0] != '\0' && admit(home, 0, roundie_time_us());
    if (!valid) {
        status.rejected++;
    }
    roundie_unlock(&lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t roundie_launcher_exit(uint32_t generation)
{
    roundie_lock(&lock);
    bool valid = gui_ready && generation != 0 && generation == status.generation &&
        !status.exit_pending && (status.state == ROUNDIE_NAV_ACTIVE || status.state == ROUNDIE_NAV_LOADING);
    if (valid) {
        status.exit_pending = true;
        exit_at = roundie_time_us();
        set_state(ROUNDIE_NAV_EXITING);
    } else {
        status.rejected++;
    }
    roundie_unlock(&lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t roundie_launcher_refresh(void)
{
    roundie_lock(&lock);
    bool valid = gui_ready && !refresh_pending &&
        (status.state == ROUNDIE_NAV_LIST || status.state == ROUNDIE_NAV_ERROR) && !observing;
    if (valid) {
        refresh_pending = true;
        set_state(ROUNDIE_NAV_REFRESHING);
    }
    roundie_unlock(&lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t roundie_launcher_back(uint32_t request)
{
    roundie_lock(&lock);
    bool valid = gui_ready && status.state == ROUNDIE_NAV_RESULT &&
        request != 0 && request == status.request && request == status.result_request && !observing;
    if (valid) {
        status.result_visible = false;
        set_state(snapshot_valid ? ROUNDIE_NAV_LIST : ROUNDIE_NAV_ERROR);
    } else {
        status.rejected++;
    }
    roundie_unlock(&lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t roundie_launcher_park_home(void)
{
    roundie_lock(&lock);
    bool valid = home_parked;
    if (!valid && status.home_active && !status.exit_pending &&
        (status.state == ROUNDIE_NAV_ACTIVE || status.state == ROUNDIE_NAV_LOADING) &&
        status.generation != 0) {
        status.exit_pending = true;
        exit_at = roundie_time_us();
        set_state(ROUNDIE_NAV_EXITING);
        home_parked = true;
        valid = true;
    }
    roundie_unlock(&lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void roundie_launcher_resume_home(void)
{
    roundie_lock(&lock);
    if (home_parked) {
        home_parked = false;
        home_due = true;
    }
    roundie_unlock(&lock);
}

void roundie_launcher_result_report(uint32_t request, bool visible, uint32_t apply_us,
    uint32_t completion_us)
{
    roundie_lock(&lock);
    if (request == status.request && request == status.result_request && status.state == ROUNDIE_NAV_RESULT) {
        status.result_visible = visible;
        status.result_apply_us = apply_us;
        status.completion_to_gui_us = completion_us;
    }
    roundie_unlock(&lock);
}

void roundie_launcher_gui_report(bool ready, bool foreground, uint32_t objects,
    uint32_t pool, uint32_t create_us, uint32_t latency_us)
{
    roundie_lock(&lock);
    gui_ready = ready;
    status.foreground = foreground;
    status.gui_objects = objects;
    status.pool_free = pool;
    if (create_us != 0) {
        status.gui_create_us = create_us;
    }
    if (latency_us > status.gui_latency_max_us) {
        status.gui_latency_max_us = latency_us;
    }
    roundie_unlock(&lock);
}

void roundie_launcher_painted(roundie_navigation_t state, uint32_t request,
    uint32_t generation, int64_t at_us, uint32_t ready_us)
{
    roundie_lock(&lock);
    if (state == status.state && request == status.request && generation == status.generation) {
        status.painted_state = state;
        status.painted_request = request;
        status.painted_generation = generation;
        status.gui_ready_us = ready_us;
        status.gui_applied_us = at_us;
    }
    roundie_unlock(&lock);
}

void roundie_launcher_gui_failed(void)
{
    roundie_lock(&lock);
    snapshot_valid = false;
    status.launch_result = ESP_ERR_NO_MEM;
    status.foreground = false;
    status.gui_objects = 0;
    set_state(ROUNDIE_NAV_ERROR);
    roundie_unlock(&lock);
}

static void publish(esp_err_t result)
{
    roundie_packages_status_t packages;
    roundie_packages_status(&packages);
    int64_t started = roundie_time_us();
    if (result == ESP_OK && packages.count > ROUNDIE_PACKAGES_MAX) {
        result = ESP_ERR_INVALID_SIZE;
    }
    if (result == ESP_OK) {
        for (unsigned i = 0; i < packages.count; i++) {
            result = roundie_packages_get(i, &record);
            if (result != ESP_OK) {
                break;
            }
            if (!roundie_package_id_valid(record.metadata.id)) {
                result = ESP_ERR_INVALID_ARG;
                break;
            }
            for (unsigned j = 0; j < i; j++) {
                if (strcmp(discovered[j].id, record.metadata.id) == 0) {
                    result = ESP_ERR_INVALID_ARG;
                }
            }
            if (result != ESP_OK) {
                break;
            }
            discovered[i] = (roundie_launcher_entry_t){.abi = record.metadata.abi, .health = record.health};
            memcpy(discovered[i].id, record.metadata.id, sizeof(record.metadata.id));
            memcpy(discovered[i].name, record.metadata.name, sizeof(record.metadata.name));
            memcpy(discovered[i].version, record.metadata.version, sizeof(record.metadata.version));
        }
    }
    roundie_lock(&lock);
    refresh_pending = false;
    status.refresh_us = packages.enumeration_us;
    if (result == ESP_OK && packages.count <= ROUNDIE_PACKAGES_MAX && status.revision != UINT32_MAX) {
        memcpy(status.entries, discovered, sizeof(discovered));
        status.count = packages.count;
        status.home_available = false;
        for (unsigned i = 0; i < status.count; i++) {
            if (home[0] != '\0' && strcmp(status.entries[i].id, home) == 0 &&
                status.entries[i].health == ROUNDIE_PACKAGE_OK) {
                status.home_available = true;
            }
        }
        status.revision++;
        package_revision = packages.revision;
        snapshot_valid = true;
        set_state(ROUNDIE_NAV_LIST);
    } else {
        snapshot_valid = false;
        status.launch_result = result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
        set_state(ROUNDIE_NAV_ERROR);
    }
    status.copy_us = (uint32_t)(roundie_time_us() - started);
    roundie_unlock(&lock);
}

void roundie_launcher_poll(bool allowed, bool transfer_busy)
{
    roundie_elf_status_t runtime;
    roundie_ui_status_t ui;
    roundie_elf_status(&runtime);
    roundie_ui_status(&ui);
    bool busy = roundie_elf_busy();
    int64_t now = roundie_time_us();
    roundie_lock(&lock);
    status.stack_min = roundie_thread_stack_unused();
    bool ready = gui_ready;
    bool refresh = refresh_pending || status.state == ROUNDIE_NAV_WAITING;
    bool launch = status.launch_pending;
    bool exiting = status.exit_pending;
    bool result_pending = status.state == ROUNDIE_NAV_RESULT;
    uint32_t generation = status.generation;
    char id[ROUNDIE_PACKAGE_ID];
    memcpy(id, status.selected, sizeof(id));
    roundie_unlock(&lock);

    if (!ready) {
        return;
    }
    /* Observe trusted explicit development starts as well as launcher starts. */
    if (busy && !observing && !launch) {
        roundie_lock(&lock);
        observing = true;
        observed_request = 0;
        completion_before = runtime.completed;
        status.generation = runtime.instance;
        status.selected[0] = '\0';
        status.result_visible = false;
        status.home_active = false;
        requested_at = now;
        set_state(ROUNDIE_NAV_LOADING);
        roundie_unlock(&lock);
        generation = runtime.instance;
    }
    if (observing) {
        if (runtime.instance != generation) {
            roundie_lock(&lock);
            status.unload_result = ESP_ERR_INVALID_STATE;
            set_state(ROUNDIE_NAV_ERROR);
            roundie_unlock(&lock);
            return;
        }
        if (exiting && !exit_submitted && runtime.abi_version >= 2 &&
            (runtime.state == ROUNDIE_ELF_RUNNING || runtime.state == ROUNDIE_ELF_SUSPENDED ||
             runtime.state == ROUNDIE_ELF_STOPPED)) {
            esp_err_t result = roundie_elf_control(generation, ROUNDIE_ELF_UNLOAD);
            if (result == ESP_OK) {
                exit_submitted = true;
                roundie_lock(&lock);
                status.exit_admission_us = (uint32_t)(roundie_time_us() - exit_at);
                roundie_unlock(&lock);
            } /* A preceding lifecycle control may still be completing. */
        }
        roundie_lock(&lock);
        if (runtime.state == ROUNDIE_ELF_RUNNING && status.state == ROUNDIE_NAV_LOADING) {
            status.started_us = (uint32_t)(now - requested_at);
            set_state(ROUNDIE_NAV_ACTIVE);
        }
        if (!busy && runtime.state == ROUNDIE_ELF_UNLOADED && runtime.resources_outstanding == 0 &&
            ui.generation == 0 && ui.outstanding == 0 && ui.pending == 0 &&
            (observed_request == 0 || runtime.completed > completion_before || runtime.error[0] != '\0')) {
            observing = false;
            exit_submitted = false;
            status.launch_pending = false;
            status.exit_pending = false;
            status.app_result = runtime.app_result;
            status.unload_result = runtime.error[0] == '\0' ? ESP_OK : ESP_FAIL;
            status.return_us = exiting ? (uint32_t)(now - exit_at) : (uint32_t)(now - requested_at);
            if (observed_request != 0 && observed_request == status.request &&
                (runtime.abi_version == 1 || status.unload_result != ESP_OK || runtime.loader_result != 0)) {
                status.result_request = observed_request;
                status.completed = runtime.completed;
                status.completed_us = runtime.completed_us;
                status.loader_result = runtime.loader_result;
                status.executed = runtime.executed;
                status.result_success = runtime.executed && runtime.loader_result == 0 &&
                    status.unload_result == ESP_OK;
                memcpy(status.error, runtime.error, sizeof(runtime.error));
                set_state(ROUNDIE_NAV_RESULT);
            } else {
                /* A normal end of any other application returns home; the
                 * home application's own end shows the list. */
                home_due = !status.home_active && status.unload_result == ESP_OK;
                set_state(snapshot_valid ? ROUNDIE_NAV_LIST : ROUNDIE_NAV_ERROR);
            }
            status.home_active = false;
        }
        roundie_unlock(&lock);
        return;
    }
    if (!allowed || transfer_busy || busy) {
        if (launch) {
            roundie_lock(&lock);
            status.launch_pending = false;
            status.launch_result = ESP_ERR_INVALID_STATE;
            status.result_request = status.request;
            set_state(ROUNDIE_NAV_RESULT);
            roundie_unlock(&lock);
        }
        return;
    }
    roundie_packages_status_t packages;
    roundie_packages_status(&packages);
    roundie_lock(&lock);
    if (home_due && !launch && status.state == ROUNDIE_NAV_LIST && !refresh_pending &&
        packages.revision == package_revision) {
        /* Once, when the list is current: a missing or unhealthy home stays on the list. */
        launch = status.home_available && admit(home, 0, now);
        home_due = false;
        memcpy(id, status.selected, sizeof(id));
    }
    roundie_unlock(&lock);
    if (launch) {
        uint32_t previous_instance = runtime.instance;
        uint32_t previous_completion = runtime.completed;
        esp_err_t result = packages.revision == package_revision ? ESP_OK : ESP_ERR_INVALID_STATE;
        roundie_lock(&lock);
        status.admission_us = (uint32_t)(now - requested_at);
        set_state(ROUNDIE_NAV_LOADING);
        roundie_unlock(&lock);
        if (result == ESP_OK) {
            result = roundie_packages_launch(id);
        }
        roundie_elf_status(&runtime);
        roundie_lock(&lock);
        status.launch_pending = false;
        status.launch_result = result;
        if (result == ESP_OK && runtime.instance > previous_instance) {
            status.generation = runtime.instance;
            completion_before = previous_completion;
            observed_request = status.request;
            observing = true;
            set_state(ROUNDIE_NAV_LOADING);
        } else {
            status.launch_result = result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
            status.result_request = status.request;
            set_state(ROUNDIE_NAV_RESULT);
        }
        roundie_unlock(&lock);
        return;
    }
    if (result_pending) {
        return; /* A registry publication must not dismiss a result. */
    }
    if (refresh) {
        roundie_lock(&lock);
        set_state(ROUNDIE_NAV_REFRESHING);
        roundie_unlock(&lock);
        uint32_t before = ui.flushes;
        esp_err_t result = roundie_packages_scan();
        roundie_ui_status(&ui);
        roundie_lock(&lock);
        status.discovery_flushes = ui.flushes - before;
        roundie_unlock(&lock);
        publish(result);
    } else if (packages.revision != package_revision) {
        publish(ESP_OK); /* Completed package operation already scanned. */
    }
}
