/* Production launcher/coordinator and pinned LVGL. Package/runtime are mocked;
 * existing package and UI/runtime suites cover their production internals. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "roundie_launcher.h"
#include "roundie_elf.h"
#include "roundie_app_ui.h"
#include "freertos/task.h"
#include "lvgl.h"

static TaskHandle_t task = (void *)1;
static int64_t clock_us = 1000;
static roundie_packages_status_t packages;
static roundie_package_record_t records[ROUNDIE_PACKAGES_MAX];
static roundie_elf_status_t runtime;
static roundie_ui_status_t ui;
static bool input_busy;
static esp_err_t scan_result;
static esp_err_t launch_result;
static unsigned scans;
static unsigned launches;
static unsigned controls;
static roundie_launcher_status_t nav;

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return task; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 4096; }
int64_t esp_timer_get_time(void) { return clock_us += 10; }
void roundie_packages_status(roundie_packages_status_t *out) { assert(task == (void *)2); *out = packages; }
esp_err_t roundie_packages_scan(void)
{
    assert(task == (void *)2);
    scans++;
    if (scan_result == ESP_OK) { packages.revision++; }
    return scan_result;
}
esp_err_t roundie_packages_get(unsigned index, roundie_package_record_t *out)
{
    assert(task == (void *)2 && index < ROUNDIE_PACKAGES_MAX);
    *out = records[index];
    return ESP_OK;
}
esp_err_t roundie_packages_launch(const char *id)
{
    assert(task == (void *)2 && strcmp(id, nav.selected) == 0);
    launches++;
    if (launch_result == ESP_OK) {
        runtime.instance++;
        runtime.abi_version = strcmp(id, "org.roundie.hello") == 0 ? 1 : 3;
        runtime.state = ROUNDIE_ELF_LOADING;
        input_busy = true;
    }
    return launch_result;
}
void roundie_elf_status(roundie_elf_status_t *out) { *out = runtime; }
void roundie_ui_status(roundie_ui_status_t *out) { *out = ui; }
bool roundie_elf_busy(void) { return runtime.state != ROUNDIE_ELF_UNLOADED || input_busy; }
esp_err_t roundie_elf_control(uint32_t generation, roundie_elf_control_t operation)
{
    assert(task == (void *)2 && generation == runtime.instance && operation == ROUNDIE_ELF_UNLOAD);
    controls++;
    runtime.state = ROUNDIE_ELF_STOPPING;
    return ESP_OK;
}

static void poll(bool allowed, bool transfer)
{
    task = (void *)2;
    roundie_launcher_poll(allowed, transfer);
    task = (void *)1;
    roundie_launcher_status(&nav);
    roundie_launcher_gui_pump(false);
    roundie_launcher_status(&nav);
}

static void entry(unsigned i, const char *id, unsigned abi, roundie_package_health_t health)
{
    records[i] = (roundie_package_record_t){.health = health};
    snprintf(records[i].metadata.id, ROUNDIE_PACKAGE_ID, "%s", id);
    snprintf(records[i].metadata.name, ROUNDIE_PACKAGE_NAME, "%s", id);
    records[i].metadata.version[0] = 1;
    records[i].metadata.abi = (uint16_t)abi;
}

static void finish(void)
{
    runtime.state = ROUNDIE_ELF_UNLOADED;
    runtime.resources_outstanding = 0;
    input_busy = false;
    memset(&ui, 0, sizeof(ui));
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_LIST && !nav.exit_pending && !nav.launch_pending);
}

static void select_id(const char *id)
{
    assert(roundie_launcher_select(id, nav.revision, 2) == ESP_OK);
    roundie_launcher_status(&nav);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_LOADING && nav.generation == runtime.instance);
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels;
    lv_display_flush_ready(display);
}

int main(int argc, char **argv)
{
    (void)argv;
    lv_init();
    lv_display_t *display = lv_display_create(360, 360);
    static uint8_t buffer[360 * 10 * 2];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    roundie_launcher_gui_bind();
    if (argc > 1) {
        lv_mem_monitor_t memory;
        lv_mem_monitor(&memory);
        void *pressure = lv_malloc(memory.free_size - 8000);
        assert(pressure != NULL);
        poll(true, false);
        assert(nav.gui_objects == 0 && !nav.foreground);
        assert(roundie_launcher_select("org.roundie.hello", nav.revision, 2) != ESP_OK);
        lv_free(pressure);
        assert(roundie_launcher_refresh() == ESP_OK);
        poll(true, false);
        assert(nav.foreground && nav.gui_objects == 33);
        lv_deinit();
        puts("launcher low-memory: safe refusal, boot independent");
        return 0;
    }
    poll(false, false);
    assert(scans == 0 && nav.state == ROUNDIE_NAV_WAITING);
    poll(true, true);
    assert(scans == 0);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_LIST && nav.count == 0 && nav.gui_objects == 33);
    assert(roundie_launcher_select("org.roundie.hello", nav.revision, 2) != ESP_OK);
    packages.count = 1;
    entry(0, "org.roundie.hello", 1, ROUNDIE_PACKAGE_OK);
    assert(roundie_launcher_refresh() == ESP_OK);
    assert(roundie_launcher_select("org.roundie.hello", nav.revision, 2) != ESP_OK);
    poll(true, false);
    assert(nav.count == 1);
    uint32_t old_revision = nav.revision;
    assert(roundie_launcher_select("org.roundie.hello", old_revision - 1, 2) != ESP_OK);
    /* Real LVGL system row callback, no package or native operation on GUI. */
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *list = lv_obj_get_child(root, 2);
    lv_obj_send_event(lv_obj_get_child(list, 0), LV_EVENT_CLICKED, NULL);
    roundie_launcher_status(&nav);
    assert(nav.launch_pending && nav.physical == 1 && launches == 0);
    assert(roundie_launcher_select("org.roundie.hello", old_revision, 2) != ESP_OK);
    poll(true, false);
    assert(launches == 1 && nav.state == ROUNDIE_NAV_LOADING);
    runtime.app_result = 42;
    runtime.state = ROUNDIE_ELF_UNLOADED;
    poll(true, false); /* Input buffer still owned: do not report completion. */
    assert(nav.state == ROUNDIE_NAV_LOADING);
    finish();
    assert(nav.app_result == 42);
    packages.count = 3;
    entry(1, "org.roundie.ui", 3, ROUNDIE_PACKAGE_OK);
    entry(2, "org.roundie.bad", 9, ROUNDIE_PACKAGE_INCOMPATIBLE);
    packages.revision++;
    unsigned old_scans = scans;
    poll(true, false);
    assert(nav.count == 3 && scans == old_scans && nav.revision > old_revision);
    assert(roundie_launcher_select("org.roundie.bad", nav.revision, 2) != ESP_OK);
    records[2].health = ROUNDIE_PACKAGE_CORRUPT;
    packages.revision++;
    poll(true, false);
    assert(roundie_launcher_select("org.roundie.bad", nav.revision, 2) != ESP_OK);
    /* Failed refresh retains previous metadata, disables stale selection. */
    scan_result = ESP_FAIL;
    assert(roundie_launcher_refresh() == ESP_OK);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_ERROR && nav.count == 3);
    assert(roundie_launcher_select("org.roundie.ui", nav.revision, 2) != ESP_OK);
    scan_result = ESP_OK;
    assert(roundie_launcher_refresh() == ESP_OK);
    poll(true, false);
    launch_result = ESP_ERR_NO_MEM;
    assert(roundie_launcher_select("org.roundie.ui", nav.revision, 2) == ESP_OK);
    roundie_launcher_status(&nav);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_LIST && nav.launch_result == ESP_ERR_NO_MEM);
    launch_result = ESP_OK;
    /* Removed/stale cache revision cannot reach package launch. */
    assert(roundie_launcher_select("org.roundie.ui", nav.revision, 2) == ESP_OK);
    roundie_launcher_status(&nav);
    unsigned before = launches;
    packages.revision++;
    poll(true, false);
    assert(launches == before && nav.state == ROUNDIE_NAV_LIST);
    poll(true, false);
    select_id("org.roundie.ui");
    runtime.state = ROUNDIE_ELF_RUNNING;
    runtime.resources_outstanding = 3;
    ui.generation = runtime.instance;
    ui.outstanding = 2;
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_ACTIVE);
    lv_obj_t *chrome = lv_obj_get_child(lv_layer_top(), 0);
    assert(!lv_obj_has_flag(chrome, LV_OBJ_FLAG_HIDDEN));
    runtime.state = ROUNDIE_ELF_SUSPENDED;
    poll(true, false);
    /* Exit is a system callback, never an application widget/input event. */
    lv_obj_send_event(chrome, LV_EVENT_CLICKED, NULL);
    roundie_launcher_status(&nav);
    assert(nav.exit_pending && controls == 0);
    assert(roundie_launcher_exit(runtime.instance) != ESP_OK);
    poll(true, false);
    assert(controls == 1 && nav.state == ROUNDIE_NAV_EXITING);
    runtime.state = ROUNDIE_ELF_UNLOADED;
    input_busy = false;
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_EXITING); /* Resource/UI teardown pending. */
    finish();
    assert(lv_obj_is_valid(root) && lv_obj_is_valid(chrome));
    assert(lv_obj_get_child_count(list) == ROUNDIE_PACKAGES_MAX);
    /* Headless external start is observed and exit remains available. */
    runtime.instance++;
    runtime.abi_version = 2;
    runtime.state = ROUNDIE_ELF_RUNNING;
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_ACTIVE && nav.foreground);
    assert(roundie_launcher_exit(runtime.instance - 1) != ESP_OK);
    assert(roundie_launcher_exit(runtime.instance) == ESP_OK);
    poll(true, false);
    finish();
    /* Full capacity and malformed/duplicate publication fail closed. */
    packages.count = ROUNDIE_PACKAGES_MAX;
    for (unsigned i = 3; i < ROUNDIE_PACKAGES_MAX; i++) {
        char id[64];
        snprintf(id, sizeof(id), "org.roundie.app%u", i);
        entry(i, id, 1, ROUNDIE_PACKAGE_OK);
    }
    packages.revision++;
    poll(true, false);
    assert(nav.count == ROUNDIE_PACKAGES_MAX);
    records[7] = records[0];
    packages.revision++;
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_ERROR && nav.count == ROUNDIE_PACKAGES_MAX);
    packages.count = ROUNDIE_PACKAGES_MAX + 1;
    assert(roundie_launcher_refresh() == ESP_OK);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_ERROR);
    /* No scan/launch after recovery closes admission. */
    old_scans = scans;
    poll(false, false);
    assert(scans == old_scans);
    runtime.instance++;
    runtime.state = ROUNDIE_ELF_RUNNING;
    poll(true, false);
    runtime.instance++; /* Unexpected runtime generation cannot regain admission. */
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_ERROR && nav.unload_result != ESP_OK);
    assert(roundie_launcher_refresh() != ESP_OK);
    roundie_launcher_gui_pump(true);
    roundie_launcher_status(&nav);
    assert(!nav.foreground && lv_obj_has_flag(chrome, LV_OBJ_FLAG_HIDDEN));
    lv_deinit();
    puts("launcher: production state, real LVGL, ABI presentation, ownership and cleanup gates passed");
    return 0;
}
