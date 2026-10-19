#include "private.h"
#include <limits.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "roundie_elf.h"
#include "roundie_app_ui.h"

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static roundie_launcher_status_t status;
static bool gui_ready;
static bool refresh_pending;
static bool snapshot_valid;
static bool observing;
static bool exit_submitted;
static uint32_t package_revision;
static int64_t requested_at;
static int64_t exit_at;
/* Sole worker scratch, deliberately outside its 12 KiB stack. */
static roundie_launcher_entry_t discovered[ROUNDIE_PACKAGES_MAX];
static roundie_package_record_t record;

static void set_state(roundie_navigation_t state)
{
    status.state = state;
    status.changed_us = esp_timer_get_time();
}

void roundie_launcher_status(roundie_launcher_status_t *out)
{
    portENTER_CRITICAL(&lock);
    *out = status;
    portEXIT_CRITICAL(&lock);
}

esp_err_t roundie_launcher_select(const char *id, uint32_t revision, uint32_t origin)
{
    if (id == NULL || !roundie_package_id_valid(id) || (origin != 1 && origin != 2)) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t started = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    bool valid = gui_ready && snapshot_valid && status.state == ROUNDIE_NAV_LIST &&
        !refresh_pending && revision == status.revision && status.request != UINT32_MAX;
    bool found = false;
    for (unsigned i = 0; i < status.count; i++) {
        if (strcmp(id, status.entries[i].id) == 0 && status.entries[i].health == ROUNDIE_PACKAGE_OK) {
            found = true;
        }
    }
    valid = valid && found;
    if (valid) {
        strcpy(status.selected, id);
        status.request++;
        status.generation = 0;
        status.launch_pending = true;
        status.launch_result = 0;
        status.unload_result = 0;
        status.app_result = 0;
        status.admission_us = 0;
        status.started_us = 0;
        status.exit_admission_us = 0;
        status.return_us = 0;
        status.physical += origin == 1;
        status.injected += origin == 2;
        requested_at = started;
        set_state(ROUNDIE_NAV_REQUESTED);
    } else {
        status.rejected++;
    }
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
    if (elapsed > status.enqueue_max_us) {
        status.enqueue_max_us = elapsed;
    }
    portEXIT_CRITICAL(&lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t roundie_launcher_exit(uint32_t generation)
{
    portENTER_CRITICAL(&lock);
    bool valid = gui_ready && generation != 0 && generation == status.generation &&
        !status.exit_pending && (status.state == ROUNDIE_NAV_ACTIVE || status.state == ROUNDIE_NAV_LOADING);
    if (valid) {
        status.exit_pending = true;
        exit_at = esp_timer_get_time();
        set_state(ROUNDIE_NAV_EXITING);
    } else {
        status.rejected++;
    }
    portEXIT_CRITICAL(&lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t roundie_launcher_refresh(void)
{
    portENTER_CRITICAL(&lock);
    bool valid = gui_ready && !refresh_pending &&
        (status.state == ROUNDIE_NAV_LIST || status.state == ROUNDIE_NAV_ERROR) && !observing;
    if (valid) {
        refresh_pending = true;
        set_state(ROUNDIE_NAV_REFRESHING);
    }
    portEXIT_CRITICAL(&lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void roundie_launcher_gui_report(bool ready, bool foreground, uint32_t objects,
    uint32_t pool, uint32_t create_us, uint32_t latency_us)
{
    portENTER_CRITICAL(&lock);
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
    portEXIT_CRITICAL(&lock);
}

void roundie_launcher_gui_failed(void)
{
    portENTER_CRITICAL(&lock);
    snapshot_valid = false;
    status.launch_result = ESP_ERR_NO_MEM;
    status.foreground = false;
    status.gui_objects = 0;
    set_state(ROUNDIE_NAV_ERROR);
    portEXIT_CRITICAL(&lock);
}

static void publish(esp_err_t result)
{
    roundie_packages_status_t packages;
    roundie_packages_status(&packages);
    int64_t started = esp_timer_get_time();
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
    portENTER_CRITICAL(&lock);
    refresh_pending = false;
    status.refresh_us = packages.enumeration_us;
    if (result == ESP_OK && packages.count <= ROUNDIE_PACKAGES_MAX && status.revision != UINT32_MAX) {
        memcpy(status.entries, discovered, sizeof(discovered));
        status.count = packages.count;
        status.revision++;
        package_revision = packages.revision;
        snapshot_valid = true;
        set_state(ROUNDIE_NAV_LIST);
    } else {
        snapshot_valid = false;
        status.launch_result = result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
        set_state(ROUNDIE_NAV_ERROR);
    }
    status.copy_us = (uint32_t)(esp_timer_get_time() - started);
    portEXIT_CRITICAL(&lock);
}

void roundie_launcher_poll(bool allowed, bool transfer_busy)
{
    roundie_elf_status_t runtime;
    roundie_ui_status_t ui;
    roundie_elf_status(&runtime);
    roundie_ui_status(&ui);
    bool busy = roundie_elf_busy();
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    status.stack_min = uxTaskGetStackHighWaterMark(NULL);
    bool ready = gui_ready;
    bool refresh = refresh_pending || status.state == ROUNDIE_NAV_WAITING;
    bool launch = status.launch_pending;
    bool exiting = status.exit_pending;
    uint32_t generation = status.generation;
    char id[ROUNDIE_PACKAGE_ID];
    memcpy(id, status.selected, sizeof(id));
    portEXIT_CRITICAL(&lock);

    if (!ready) {
        return;
    }
    /* Observe trusted explicit development starts as well as launcher starts. */
    if (busy && !observing && !launch) {
        portENTER_CRITICAL(&lock);
        observing = true;
        status.generation = runtime.instance;
        status.selected[0] = '\0';
        requested_at = now;
        set_state(ROUNDIE_NAV_LOADING);
        portEXIT_CRITICAL(&lock);
        generation = runtime.instance;
    }
    if (observing) {
        if (runtime.instance != generation) {
            portENTER_CRITICAL(&lock);
            status.unload_result = ESP_ERR_INVALID_STATE;
            set_state(ROUNDIE_NAV_ERROR);
            portEXIT_CRITICAL(&lock);
            return;
        }
        if (exiting && !exit_submitted && runtime.abi_version >= 2 &&
            (runtime.state == ROUNDIE_ELF_RUNNING || runtime.state == ROUNDIE_ELF_SUSPENDED ||
             runtime.state == ROUNDIE_ELF_STOPPED)) {
            esp_err_t result = roundie_elf_control(generation, ROUNDIE_ELF_UNLOAD);
            if (result == ESP_OK) {
                exit_submitted = true;
                portENTER_CRITICAL(&lock);
                status.exit_admission_us = (uint32_t)(esp_timer_get_time() - exit_at);
                portEXIT_CRITICAL(&lock);
            } /* A preceding lifecycle control may still be completing. */
        }
        portENTER_CRITICAL(&lock);
        if (runtime.state == ROUNDIE_ELF_RUNNING && status.state == ROUNDIE_NAV_LOADING) {
            status.started_us = (uint32_t)(now - requested_at);
            set_state(ROUNDIE_NAV_ACTIVE);
        }
        if (!busy && runtime.state == ROUNDIE_ELF_UNLOADED && runtime.resources_outstanding == 0 &&
            ui.generation == 0 && ui.outstanding == 0 && ui.pending == 0) {
            observing = false;
            exit_submitted = false;
            status.launch_pending = false;
            status.exit_pending = false;
            status.app_result = runtime.app_result;
            status.unload_result = runtime.error[0] == '\0' ? ESP_OK : ESP_FAIL;
            status.return_us = exiting ? (uint32_t)(now - exit_at) : (uint32_t)(now - requested_at);
            set_state(snapshot_valid ? ROUNDIE_NAV_LIST : ROUNDIE_NAV_ERROR);
        }
        portEXIT_CRITICAL(&lock);
        return;
    }
    if (!allowed || transfer_busy || busy) {
        if (launch) {
            portENTER_CRITICAL(&lock);
            status.launch_pending = false;
            status.launch_result = ESP_ERR_INVALID_STATE;
            set_state(snapshot_valid ? ROUNDIE_NAV_LIST : ROUNDIE_NAV_ERROR);
            portEXIT_CRITICAL(&lock);
        }
        return;
    }
    roundie_packages_status_t packages;
    roundie_packages_status(&packages);
    if (launch) {
        esp_err_t result = packages.revision == package_revision ? ESP_OK : ESP_ERR_INVALID_STATE;
        portENTER_CRITICAL(&lock);
        status.admission_us = (uint32_t)(now - requested_at);
        set_state(ROUNDIE_NAV_LOADING);
        portEXIT_CRITICAL(&lock);
        if (result == ESP_OK) {
            result = roundie_packages_launch(id);
        }
        roundie_elf_status(&runtime);
        portENTER_CRITICAL(&lock);
        status.launch_pending = false;
        status.launch_result = result;
        if (result == ESP_OK) {
            status.generation = runtime.instance;
            observing = true;
            set_state(ROUNDIE_NAV_LOADING);
        } else {
            set_state(ROUNDIE_NAV_LIST);
        }
        portEXIT_CRITICAL(&lock);
        return;
    }
    if (refresh) {
        portENTER_CRITICAL(&lock);
        set_state(ROUNDIE_NAV_REFRESHING);
        portEXIT_CRITICAL(&lock);
        uint32_t before = ui.flushes;
        esp_err_t result = roundie_packages_scan();
        roundie_ui_status(&ui);
        portENTER_CRITICAL(&lock);
        status.discovery_flushes = ui.flushes - before;
        portEXIT_CRITICAL(&lock);
        publish(result);
    } else if (packages.revision != package_revision) {
        publish(ESP_OK); /* Completed package operation already scanned. */
    }
}
