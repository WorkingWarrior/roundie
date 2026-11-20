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
#include "gui_diagnostics.h"

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
static char diagnostic[1400];

static bool diagnostic_error(esp_err_t error)
{
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "12345678 %d ", error);
    return strstr(diagnostic, prefix) != NULL;
}

int usb_serial_jtag_write_bytes(const void *bytes, size_t length, TickType_t ticks)
{
    (void)ticks;
    assert(length < sizeof(diagnostic));
    memcpy(diagnostic, bytes, length);
    diagnostic[length] = '\0';
    return (int)length;
}

static void performance_tests(void)
{
    assert(roundie_gui_batch() == 12);
    assert(roundie_gui_command("BATCH 4", "12345678", ESP_OK));
    assert(roundie_gui_batch() == 4);
    input_busy = true;
    assert(roundie_gui_command("BATCH 12", "12345678", ESP_OK));
    assert(roundie_gui_batch() == 4 && diagnostic_error(ESP_ERR_INVALID_STATE));
    input_busy = false;
    assert(roundie_gui_command("BATCH 12", "12345678", ESP_FAIL));
    assert(roundie_gui_batch() == 4); /* Invalid framing cannot mutate tuning. */
    assert(roundie_gui_command("BATCH 12", "12345678", ESP_OK));
    assert(roundie_gui_batch() == 12);
    roundie_gui_work_t work = {.flushes = 18, .pixels = 129600, .dma_us = 200000,
        .dma_max_us = 11000, .submit_us = 5000, .wait_us = 190000, .render_us = 250000};
    for (unsigned i = 0; i < 40; i++) {
        roundie_gui_record(INT64_C(5000000000) + i, 500, 300000, 0, 6, 0, 6, &work, 0, 18 * (i + 1));
    }
    assert(roundie_gui_command("PERF 0", "12345678", ESP_OK));
    assert(strstr(diagnostic, "\"oldest\":9,\"latest\":40") != NULL);
    assert(strstr(diagnostic, "\"trace\":[9,5000000008,") != NULL);
    assert(strstr(diagnostic, "[12,40,720,5184000,0,300000,500,0,11000") != NULL);
    assert(roundie_gui_command("PERF 4294967295", "12345678", ESP_OK));
    assert(strstr(diagnostic, "\"trace\":[0,0,") != NULL);
    assert(roundie_gui_command("PERF 4294967296", "12345678", ESP_OK));
    assert(diagnostic_error(ESP_ERR_INVALID_ARG));
    assert(!roundie_gui_command("OTHER", "12345678", ESP_OK));
}

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
    static roundie_launcher_status_t current; /* home is admitted in the same poll */
    roundie_launcher_status(&current);
    assert(task == (void *)2 && strcmp(id, current.selected) == 0);
    launches++;
    if (launch_result == ESP_OK) {
        runtime.instance++;
        runtime.abi_version = strcmp(id, "org.roundie.hello") == 0 ? 1 : 3;
        runtime.state = ROUNDIE_ELF_LOADING;
        runtime.executed = false;
        runtime.error[0] = '\0';
        runtime.loader_result = 0;
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
    runtime.completed++;
    runtime.completed_us = clock_us;
    poll(true, false);
    assert(nav.state == (runtime.abi_version == 1 || runtime.error[0] != '\0' ?
        ROUNDIE_NAV_RESULT : ROUNDIE_NAV_LIST) && !nav.exit_pending && !nav.launch_pending);
}

static void back(void)
{
    unsigned before = launches;
    assert(roundie_launcher_back(nav.result_request - 1) != ESP_OK);
    lv_obj_send_event(lv_obj_get_child(lv_screen_active(), 6), LV_EVENT_CLICKED, NULL);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_LIST && launches == before && !nav.result_visible);
}

static void select_id(const char *id)
{
    assert(roundie_launcher_select(id, nav.revision, 2) == ESP_OK);
    roundie_launcher_status(&nav);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_LOADING && nav.generation == runtime.instance);
}

/* Quick settings actions, as main.c provides them on the device. */
static unsigned quick_brightness_percent = 60;
static unsigned quick_screen_offs;
static unsigned quick_sleeps;

static bool quick_battery(uint8_t *percent, bool *charging)
{
    *percent = 78;
    *charging = true;
    return true;
}

static uint8_t quick_brightness(bool cycle)
{
    if (cycle) {
        quick_brightness_percent = quick_brightness_percent >= 100 ? 30 : quick_brightness_percent < 60 ? 60 : 100;
    }
    return (uint8_t)quick_brightness_percent;
}

static void quick_screen_off(void)
{
    quick_screen_offs++;
}

static bool quick_sleep(void)
{
    quick_sleeps++;
    return true;
}

static const roundie_launcher_quick_t quick_mock = {
    quick_battery, quick_brightness, quick_screen_off, quick_sleep,
};

/* A real LVGL pointer: system chrome sees the same events as a finger. */
static lv_indev_t *pointer;
static lv_point_t touch_point;
static lv_indev_state_t touch_state;

static void touch_read(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    data->point = touch_point;
    data->state = touch_state;
}

static void touch(int32_t x, int32_t y, bool pressed)
{
    touch_point.x = x;
    touch_point.y = y;
    touch_state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_tick_inc(20);
    lv_refr_now(NULL); /* layout, as the GUI task's timer handler does */
    lv_indev_read(pointer);
}

/* Press at (x0, y0), move in steps to (x1, y1), release there. */
static void swipe(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    touch(x0, y0, false);
    for (int32_t step = 0; step <= 8; step++) {
        touch(x0 + (x1 - x0) * step / 8, y0 + (y1 - y0) * step / 8, true);
    }
    touch(x1, y1, false);
    roundie_launcher_status(&nav);
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
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, touch_read);
    performance_tests();
    roundie_launcher_gui_bind();
    roundie_launcher_gui_set_quick_settings(&quick_mock);
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
        assert(nav.foreground && nav.gui_objects == 51);
        lv_deinit();
        puts("launcher low-memory: safe refusal, boot independent");
        return 0;
    }
    poll(false, false);
    assert(scans == 0 && nav.state == ROUNDIE_NAV_WAITING);
    poll(true, true);
    assert(scans == 0);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_LIST && nav.count == 0 && nav.gui_objects == 51);
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
    runtime.executed = true;
    runtime.state = ROUNDIE_ELF_UNLOADED;
    poll(true, false); /* Input buffer still owned: do not report completion. */
    assert(nav.state == ROUNDIE_NAV_LOADING);
    input_busy = false;
    poll(true, false); /* Unchanged completion serial is historical evidence. */
    assert(nav.state == ROUNDIE_NAV_LOADING && !nav.result_visible);
    finish();
    assert(nav.app_result == 42 && nav.result_success && nav.executed && nav.result_visible);
    assert(nav.result_request == nav.request && nav.completed == runtime.completed);
    assert(strcmp(nav.selected_name, "org.roundie.hello") == 0);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(root, 4)), "42") == 0);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(root, 1)), "Aplikacja zakończona") == 0);
    uint32_t result_request = nav.request;
    clock_us += 10000000;
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_RESULT && nav.result_visible && lv_screen_active() == root);
    assert(roundie_launcher_select("org.roundie.hello", nav.revision, 2) != ESP_OK);
    packages.revision++;
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_RESULT && nav.result_request == result_request);
    back();
    assert(roundie_launcher_back(result_request) != ESP_OK);
    /* Same application, new request: neither old result nor stale Back applies. */
    select_id("org.roundie.hello");
    assert(!nav.result_visible && nav.result_request == 0 && !nav.executed);
    assert(roundie_launcher_back(result_request) != ESP_OK);
    runtime.loader_result = -7;
    strcpy(runtime.error, "loader-relocation");
    finish();
    assert(!nav.result_success && !nav.executed && nav.loader_result == -7);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(root, 4)), "Brak wyniku aplikacji") == 0);
    back();
    select_id("org.roundie.hello");
    runtime.executed = true;
    runtime.app_result = 42;
    strcpy(runtime.error, "cleanup-failed");
    finish();
    assert(!nav.result_success && nav.executed && nav.app_result == 42 && nav.unload_result != ESP_OK);
    back();
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
    assert(nav.state == ROUNDIE_NAV_RESULT && nav.launch_result == ESP_ERR_NO_MEM && !nav.result_success);
    back();
    launch_result = ESP_OK;
    /* Removed/stale cache revision cannot reach package launch. */
    assert(roundie_launcher_select("org.roundie.ui", nav.revision, 2) == ESP_OK);
    roundie_launcher_status(&nav);
    unsigned before = launches;
    packages.revision++;
    poll(true, false);
    assert(launches == before && nav.state == ROUNDIE_NAV_RESULT);
    back();
    poll(true, false);
    select_id("org.roundie.ui");
    runtime.state = ROUNDIE_ELF_RUNNING;
    runtime.resources_outstanding = 3;
    ui.generation = runtime.instance;
    ui.outstanding = 2;
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_ACTIVE);
    lv_obj_t *chrome = lv_obj_get_child(lv_layer_top(), 0);
    lv_obj_t *bottom = lv_obj_get_child(lv_layer_top(), 1);
    lv_obj_t *pill = lv_obj_get_child(lv_layer_top(), 2);
    assert(lv_obj_has_flag(bottom, LV_OBJ_FLAG_HIDDEN)); /* no home configured */
    assert(!lv_obj_has_flag(chrome, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN));
    runtime.state = ROUNDIE_ELF_SUSPENDED;
    poll(true, false);
    /* Exit is a system swipe from the left edge, never an application event.
     * Too short, too steep, outside the strip or in the top safe area: kept. */
    swipe(10, 180, 60, 180);
    assert(!nav.exit_pending);
    swipe(10, 180, 100, 280);
    assert(!nav.exit_pending);
    swipe(70, 180, 200, 180);
    assert(!nav.exit_pending);
    swipe(10, 40, 150, 40);
    assert(!nav.exit_pending && lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN));
    /* While dragging the pill follows the finger and turns red when armed. */
    touch(8, 200, true);
    touch(40, 200, true);
    assert(!lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN));
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(pill, 0)) == lv_color_to_u32(lv_color_hex(0x3a4452)));
    touch(110, 205, true);
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(pill, 0)) == lv_color_to_u32(lv_color_hex(0xc0392b)));
    /* Back over the strip before release: not closed. */
    touch(30, 205, true);
    touch(30, 205, false);
    roundie_launcher_status(&nav);
    assert(!nav.exit_pending && lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN));
    swipe(6, 150, 140, 170);
    assert(nav.exit_pending && controls == 0);
    assert(roundie_launcher_exit(runtime.instance) != ESP_OK);
    poll(true, false);
    assert(controls == 1 && nav.state == ROUNDIE_NAV_EXITING);
    assert(lv_obj_has_flag(chrome, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(lv_obj_get_child(pill, 0)), "Zamykanie...") == 0);
    runtime.state = ROUNDIE_ELF_UNLOADED;
    input_busy = false;
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_EXITING); /* Resource/UI teardown pending. */
    finish();
    assert(lv_obj_is_valid(root) && lv_obj_is_valid(chrome));
    assert(lv_obj_get_child_count(list) == ROUNDIE_PACKAGES_MAX + 3);
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
    /* Home: launched once the list is current; no Close on home; a swipe up
     * from the bottom edge opens the list without relaunching home. */
    packages.count = 4;
    entry(3, "org.roundie.other", 3, ROUNDIE_PACKAGE_OK);
    roundie_launcher_set_home("org.roundie.ui", true);
    packages.revision++;
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_LIST && nav.home_available && !nav.home_active);
    unsigned home_launches = launches;
    uint32_t inputs = nav.physical + nav.injected;
    poll(true, false);
    assert(launches == home_launches + 1 && nav.state == ROUNDIE_NAV_LOADING && nav.home_active);
    assert(strcmp(nav.selected, "org.roundie.ui") == 0 && nav.physical + nav.injected == inputs);
    runtime.state = ROUNDIE_ELF_RUNNING;
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_ACTIVE);
    assert(lv_obj_has_flag(chrome, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(bottom, LV_OBJ_FLAG_HIDDEN));
    swipe(6, 150, 140, 170);
    assert(!nav.exit_pending); /* the left edge does nothing on home */
    /* Quick settings: a swipe down from the top edge, closed by a swipe up. */
    lv_obj_t *top = lv_obj_get_child(lv_layer_top(), 3);
    lv_obj_t *quick = lv_obj_get_child(lv_layer_top(), 4);
    assert(!lv_obj_has_flag(top, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(quick, LV_OBJ_FLAG_HIDDEN));
    swipe(180, 10, 180, 60);
    assert(lv_obj_has_flag(quick, LV_OBJ_FLAG_HIDDEN)); /* too short */
    touch(180, 10, true);
    touch(180, 120, true);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(pill, 0)), LV_SYMBOL_SETTINGS " Ustawienia") == 0);
    touch(180, 120, false);
    assert(!lv_obj_has_flag(quick, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(lv_obj_get_child(quick, 0)), "Bateria 78% • ładuje") == 0);
    lv_obj_t *brightness = lv_obj_get_child(quick, 2);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(brightness, 0)), "Jasność 60%") == 0);
    swipe(180, 110, 180, 120); /* a tap on Brightness: one step */
    assert(strcmp(lv_label_get_text(lv_obj_get_child(brightness, 0)), "Jasność 100%") == 0);
    assert(!lv_obj_has_flag(quick, LV_OBJ_FLAG_HIDDEN) && !nav.exit_pending);
    swipe(180, 330, 180, 220);
    assert(lv_obj_has_flag(quick, LV_OBJ_FLAG_HIDDEN));
    swipe(180, 10, 180, 130);
    swipe(180, 174, 180, 174); /* Screen off */
    assert(quick_screen_offs == 1 && lv_obj_has_flag(quick, LV_OBJ_FLAG_HIDDEN));
    swipe(180, 10, 180, 130);
    swipe(180, 238, 180, 238); /* Sleep */
    assert(quick_sleeps == 1 && lv_obj_has_flag(quick, LV_OBJ_FLAG_HIDDEN));
    swipe(180, 340, 260, 300);
    assert(!nav.exit_pending); /* too short and too slanted */
    touch(180, 340, true);
    touch(180, 250, true);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(pill, 0)), LV_SYMBOL_LIST " Aplikacje") == 0);
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(pill, 0)) == lv_color_to_u32(lv_color_hex(0x2f6fd0)));
    touch(180, 250, false);
    roundie_launcher_status(&nav);
    assert(nav.exit_pending);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_EXITING && lv_obj_has_flag(bottom, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(quick, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(top, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(lv_obj_get_child(pill, 0)), "Aplikacje...") == 0);
    runtime.state = ROUNDIE_ELF_UNLOADED;
    finish();
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_LIST && launches == home_launches + 1 && !nav.home_active);
    /* From the list, a swipe from the left edge goes home. */
    assert(!lv_obj_has_flag(chrome, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(bottom, LV_OBJ_FLAG_HIDDEN));
    touch(6, 150, false); /* the repaint waits for a release */
    touch(6, 150, true);
    touch(100, 150, true);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(pill, 0)), LV_SYMBOL_HOME " Zegarek") == 0);
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(pill, 0)) == lv_color_to_u32(lv_color_hex(0x2f6fd0)));
    touch(140, 150, false);
    roundie_launcher_status(&nav);
    assert(nav.launch_pending && nav.home_active);
    poll(true, false);
    assert(launches == home_launches + 2 && nav.state == ROUNDIE_NAV_LOADING);
    runtime.state = ROUNDIE_ELF_RUNNING;
    poll(true, false);
    assert(roundie_launcher_exit(runtime.instance) == ESP_OK);
    poll(true, false);
    runtime.state = ROUNDIE_ELF_UNLOADED;
    finish();
    /* The normal end of another application returns home. */
    select_id("org.roundie.other");
    assert(!nav.home_active);
    runtime.state = ROUNDIE_ELF_RUNNING;
    poll(true, false);
    assert(!lv_obj_has_flag(chrome, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(bottom, LV_OBJ_FLAG_HIDDEN));
    swipe(6, 150, 140, 170);
    poll(true, false);
    runtime.state = ROUNDIE_ELF_UNLOADED;
    finish();
    poll(true, false);
    assert(launches == home_launches + 4 && nav.state == ROUNDIE_NAV_LOADING && nav.home_active);
    runtime.state = ROUNDIE_ELF_RUNNING;
    poll(true, false);
    swipe(180, 10, 180, 130);
    assert(!lv_obj_has_flag(quick, LV_OBJ_FLAG_HIDDEN));
    assert(roundie_launcher_exit(runtime.instance) == ESP_OK);
    poll(true, false);
    assert(lv_obj_has_flag(quick, LV_OBJ_FLAG_HIDDEN)); /* closed with home */
    runtime.state = ROUNDIE_ELF_UNLOADED;
    finish();
    /* Light sleep: home is unloaded, stays unloaded, and is launched on resume. */
    assert(roundie_launcher_park_home() != ESP_OK); /* nothing running */
    assert(roundie_launcher_home() == ESP_OK);
    poll(true, false);
    runtime.state = ROUNDIE_ELF_RUNNING;
    poll(true, false);
    assert(roundie_launcher_park_home() == ESP_OK && roundie_launcher_park_home() == ESP_OK);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_EXITING);
    runtime.state = ROUNDIE_ELF_UNLOADED;
    finish();
    unsigned parked_launches = launches;
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_LIST && launches == parked_launches);
    roundie_launcher_resume_home();
    poll(true, false);
    assert(launches == parked_launches + 1 && nav.home_active && nav.state == ROUNDIE_NAV_LOADING);
    roundie_launcher_resume_home(); /* once only */
    runtime.state = ROUNDIE_ELF_RUNNING;
    poll(true, false);
    assert(roundie_launcher_exit(runtime.instance) == ESP_OK);
    poll(true, false);
    runtime.state = ROUNDIE_ELF_UNLOADED;
    finish();
    poll(true, false);
    assert(launches == parked_launches + 1);
    /* M8-B: any other application also ends through its lifecycle for
     * sleep; home starts after the wake, not after that unload. */
    select_id("org.roundie.other");
    runtime.state = ROUNDIE_ELF_RUNNING;
    poll(true, false);
    assert(!nav.home_active && nav.state == ROUNDIE_NAV_ACTIVE);
    assert(roundie_launcher_park_home() == ESP_OK);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_EXITING);
    runtime.state = ROUNDIE_ELF_UNLOADED;
    finish();
    unsigned other_parked = launches;
    poll(true, false);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_LIST && launches == other_parked);
    roundie_launcher_resume_home();
    poll(true, false); /* back home */
    assert(launches == other_parked + 1 && nav.home_active);
    runtime.state = ROUNDIE_ELF_RUNNING;
    poll(true, false);
    assert(nav.home_active && roundie_launcher_exit(runtime.instance) == ESP_OK);
    poll(true, false);
    runtime.state = ROUNDIE_ELF_UNLOADED;
    finish();
    /* A home that fails to start shows its result once; no retry loop. */
    unsigned failed_launches = launches;
    launch_result = ESP_ERR_NO_MEM;
    assert(roundie_launcher_home() == ESP_OK);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_RESULT);
    back();
    poll(true, false);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_LIST && launches == failed_launches + 1);
    launch_result = ESP_OK;
    /* An unlisted or unhealthy home is not launched; disabled, nothing is home. */
    roundie_launcher_set_home("org.roundie.bad", true);
    packages.revision++;
    poll(true, false);
    poll(true, false);
    assert(nav.state == ROUNDIE_NAV_LIST && !nav.home_available && launches == failed_launches + 1);
    assert(roundie_launcher_home() != ESP_OK && lv_obj_has_flag(chrome, LV_OBJ_FLAG_HIDDEN));
    roundie_launcher_set_home("", false);
    assert(roundie_launcher_home() != ESP_OK);
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
    /* Transforming list: rows narrow and fade towards the round edge, and
     * follow the scroll. The header and Refresh scroll with the rows. */
    list = lv_obj_get_child(lv_screen_active(), 2);
    lv_obj_t *header = lv_obj_get_child(list, ROUNDIE_PACKAGES_MAX);
    lv_obj_t *refresh = lv_obj_get_child(list, ROUNDIE_PACKAGES_MAX + 2);
    lv_obj_update_layout(list);
    assert(lv_obj_has_flag(lv_obj_get_child(lv_screen_active(), 0), LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(header), "Roundie / Aplikacje") == 0);
    assert(lv_obj_get_width(lv_obj_get_child(list, 0)) == 250 && lv_obj_get_style_opa(lv_obj_get_child(list, 1), 0) == LV_OPA_COVER);
    lv_obj_t *lower = lv_obj_get_child(list, 2); /* centre 122 px below the screen centre */
    assert(lv_obj_get_width(lower) == 150 && lv_obj_get_x(lower) == 105);
    assert(lv_obj_get_style_opa(lower, 0) > LV_OPA_TRANSP && lv_obj_get_style_opa(lower, 0) < LV_OPA_COVER);
    assert(lv_obj_get_style_opa(lv_obj_get_child(list, 3), 0) == LV_OPA_TRANSP);
    lv_obj_scroll_to_y(list, 164, LV_ANIM_OFF);
    lv_obj_update_layout(list);
    assert(lv_obj_get_width(lv_obj_get_child(list, 3)) == 250 && lv_obj_get_style_opa(lv_obj_get_child(list, 3), 0) == LV_OPA_COVER);
    assert(lv_obj_get_style_opa(header, 0) == LV_OPA_TRANSP);
    lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
    lv_obj_update_layout(list);
    lv_area_t button;
    lv_obj_get_coords(refresh, &button);
    assert(button.y2 <= 360 - 100 && lv_obj_get_style_opa(refresh, 0) == LV_OPA_COVER);
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
    puts("launcher: production state, real LVGL, ABI presentation, ownership, cleanup and home navigation gates passed");
    return 0;
}
