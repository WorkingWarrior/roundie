/* Real production manager, bridge, LVGL 9.5 and example C callbacks on host.
 * Loader/IDF and time are mocked. This does NOT execute Xtensa instructions. */
#define ROUNDIE_UI_INTEGRATION 1
#define main elf_regression_main
#include "test_elf.c"
#undef main
#include "lvgl.h"
#include "roundie_launcher.h"

static roundie_packages_status_t launcher_packages;
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 4096; }
void roundie_packages_status(roundie_packages_status_t *out) { *out = launcher_packages; }
esp_err_t roundie_packages_scan(void)
{
    launcher_packages.count = 1;
    launcher_packages.revision++;
    return ESP_OK;
}
esp_err_t roundie_packages_get(unsigned index, roundie_package_record_t *out)
{
    assert(index == 0);
    *out = (roundie_package_record_t){.health = ROUNDIE_PACKAGE_OK};
    strcpy(out->metadata.id, "org.roundie.ui");
    strcpy(out->metadata.name, "Roundie UI");
    out->metadata.version[0] = 1;
    out->metadata.abi = 3;
    return ESP_OK;
}
esp_err_t roundie_packages_launch(const char *id)
{
    assert(strcmp(id, "org.roundie.ui") == 0);
    return roundie_elf_prepare() ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool stall_gui;
static unsigned fault;
static const rep_api_v3_t *ui_api;
static rep_descriptor_v3_t ui_descriptor;
static unsigned ui_destroy_calls;
static uint32_t old_button;
static uint32_t event_count_at_input;
static lv_obj_t *home;
static void *pressure;
static uint16_t pixels[360 * 20];
extern char __executable_start;
extern char etext;
extern char __data_start;
extern char _end;

void vTaskDelay(TickType_t ticks)
{
    mock_clock += (int64_t)ticks * 1000;
    if (!stall_gui) {
        TaskHandle_t saved = current_task;
        current_task = (void *)2;
        roundie_ui_gui_pump();
        current_task = saved;
        lv_tick_inc(ticks);
    }
}

static int32_t test_init(const rep_context_v3_t *context)
{
    if (fault == 1) {
        /* Partial initialization: accepted commands must be canceled safely. */
        assert(rep_app_entry_v3(ui_api)->init(context) == 0);
        return -1;
    }
    int32_t result = rep_app_entry_v3(ui_api)->init(context);
    if (fault == 6) {
        lv_mem_monitor_t monitor;
        lv_mem_monitor(&monitor);
        pressure = lv_malloc(monitor.free_size - 6000);
        assert(pressure != NULL);
    }
    return result;
}

static int32_t test_event(const rep_context_v3_t *context, const rep_event_v3_t *event)
{
    if (fault == 4) {
        roundie_elf_set_allowed(false); /* recovery during active callback */
    }
    if (event->type == REP3_EVENT_BUTTON) {
        assert(event->reserved == REP3_ORIGIN_TOUCH || event->reserved == REP3_ORIGIN_INJECTED);
    }
    if (fault == 4) {
        return 0;
    }
    return rep_app_entry_v3(ui_api)->on_event(context, event);
}

static int32_t test_destroy(const rep_context_v3_t *context)
{
    ui_destroy_calls++;
    return rep_app_entry_v3(ui_api)->destroy(context);
}

static const rep_descriptor_v3_t *test_entry(const rep_api_v3_t *table)
{
    ui_api = table;
    assert(rep_abi_v3(3) == table && rep_abi_v3(2) == NULL && rep_abi_v2(2) == NULL);
    ui_descriptor = *rep_app_entry_v3(table);
    ui_descriptor.init = test_init;
    ui_descriptor.on_event = test_event;
    ui_descriptor.destroy = test_destroy;
    if (fault == 2) {
        ui_descriptor.required_capabilities |= UINT32_C(0x80000000);
    }
    return &ui_descriptor;
}

void mock_ui_relocate(esp_elf_t *elf)
{
    /* Mock relocation maps host C code/data; real ELF32 ranges remain covered
     * by preflight and target execution, not by this host address mapping. */
    elf->sec[ELF_SEC_TEXT] = (esp_elf_sec_t){(uintptr_t)&__executable_start,
        (uintptr_t)&etext - (uintptr_t)&__executable_start};
    elf->sec[ELF_SEC_DATA] = (esp_elf_sec_t){(uintptr_t)&__data_start,
        (uintptr_t)&_end - (uintptr_t)&__data_start};
    elf->sec[ELF_SEC_RODATA] = (esp_elf_sec_t){(uintptr_t)&etext,
        (uintptr_t)&__data_start - (uintptr_t)&etext};
    const rep_descriptor_v3_t *(*entry3)(const rep_api_v3_t *) = test_entry;
    memcpy(&elf->entry, &entry3, sizeof(entry3));
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    (void)area;
    (void)data;
    lv_display_flush_ready(display);
}

static void pump(void)
{
    vTaskDelay(10);
}

static roundie_ui_status_t ui_status(void)
{
    roundie_ui_status_t result;
    roundie_ui_status(&result);
    return result;
}

static void clean(uint32_t generation)
{
    assert(roundie_ui_transition(generation, ROUNDIE_UI_DESTROY));
    assert(roundie_ui_retire(generation));
    roundie_ui_status_t ui = ui_status();
    assert(ui.widgets == 0 && ui.pending == 0 && ui.outstanding == 0 && !ui.foreground);
    assert(ui.realized_objects == 0);
    assert(lv_screen_active() == home);
}

static bool sink(uint32_t generation, uint32_t widget, uint32_t origin, int64_t time)
{
    (void)generation;
    (void)widget;
    (void)origin;
    (void)time;
    return fault != 3;
}

static void bridge_tests(void)
{
    uint32_t generation = 1000;
    assert(roundie_ui_open(generation, sink));
    rep_ui_widget_t widget = {1, sizeof(widget), REP_UI_VIEW, 0, 0, 0, 360, 360, 0, ""};
    rep_ui_widget_t invalid = widget;
    invalid.type = 99;
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = widget;
    invalid.width = INT32_MAX;
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = widget;
    invalid.x = -1;
    assert(roundie_ui_create(generation, &invalid) == 0);
    uint32_t view = roundie_ui_create(generation, &widget);
    assert(view != 0 && roundie_ui_create(generation, &widget) == 0);
    widget = (rep_ui_widget_t){1, sizeof(widget), REP_UI_BUTTON, view, 100, 160, 160, 50, 3, "tap"};
    invalid = widget;
    invalid.parent++;
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = widget;
    invalid.text_length = REP_UI_TEXT_CAPACITY;
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = widget;
    invalid.width = 1;
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = widget;
    invalid.text[3] = 'x';
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = widget;
    invalid.text[0] = (char)0xc0;
    assert(roundie_ui_create(generation, &invalid) == 0);
    assert(roundie_ui_create(generation + 1, &widget) == 0);
    uint32_t button = roundie_ui_create(generation, &widget);
    widget.text[0] = 'X'; /* copied payload */
    current_task = (void *)1;
    roundie_ui_gui_pump();
    assert(ui_status().applied == 0); /* wrong task cannot render */
    assert(roundie_ui_transition(generation, ROUNDIE_UI_SHOW));
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *object = lv_obj_get_child(root, 0);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(object, 0)), "tap") == 0);
    assert(roundie_ui_input(generation, button, REP3_ORIGIN_INJECTED));
    fault = 3;
    assert(!roundie_ui_input(generation, button, REP3_ORIGIN_INJECTED));
    fault = 0;
    assert(ui_status().input_dropped == 1);
    for (unsigned i = 2; i < REP_UI_WIDGET_LIMIT; i++) {
        assert(roundie_ui_create(generation, &widget) != 0);
    }
    assert(roundie_ui_create(generation, &widget) == 0);
    pump();
    pump();
    rep_ui_update_t update = {1, sizeof(update), button, REP_UI_TEXT, 1, "A"};
    for (unsigned i = 0; i < ROUNDIE_UI_QUEUE_CAPACITY; i++) {
        update.text[0] = (char)('A' + i);
        assert(roundie_ui_update(generation, &update) == 0);
    }
    assert(roundie_ui_update(generation, &update) == -1);
    pump();
    assert(strcmp(lv_label_get_text(lv_obj_get_child(object, 0)), "D") == 0); /* FIFO, batch=4 */
    assert(roundie_ui_transition(generation, ROUNDIE_UI_HIDE)); /* full queue cannot block control */
    assert(!roundie_ui_input(generation, button, REP3_ORIGIN_TOUCH));
    assert(lv_screen_active() == home);
    assert(roundie_ui_transition(generation, ROUNDIE_UI_SHOW));
    assert(roundie_ui_destroy(generation, view) == 0);
    assert(roundie_ui_update(generation, &update) == -1);
    assert(roundie_ui_destroy(generation, view) == -1);
    clean(generation);
    assert(!roundie_ui_input(generation, button, REP3_ORIGIN_TOUCH));
    assert(roundie_ui_open(++generation, sink));
    assert(roundie_ui_update(generation, &update) == -1); /* old handle */
    clean(generation);
    puts("Bridge + real LVGL: validation, UTF-8, copied payload, bounds, FIFO, overflow, owner task, stale IDs, cleanup PASS");
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    lv_init();
    lv_display_t *display = lv_display_create(360, 360);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    home = lv_screen_active();
    current_task = (void *)2;
    roundie_ui_gui_bind();
    current_task = (void *)1;
    bridge_tests();
    assert(elf_regression_main(3, argv) == 0);
    FILE *file = fopen(argv[3], "rb");
    assert(file != NULL);
    fixture_size = fread(fixture, 1, sizeof(fixture), file);
    assert(fclose(file) == 0);
    mode = 0;
    for (fault = 0; fault <= 6; fault++) {
        ui_destroy_calls = 0;
        roundie_elf_set_allowed(true);
        assert(roundie_elf_prepare());
        assert(roundie_elf_execute(fixture, fixture_size));
        uint32_t generation = status.instance;
        if (fault == 1 || fault == 2 || fault == 6) {
            assert(status.state == ROUNDIE_ELF_UNLOADED && status.error[0] != '\0');
            assert(ui_destroy_calls == (fault == 2 ? 0U : 1U));
            if (fault == 6) {
                assert(ui_status().failed != 0);
                lv_free(pressure);
                pressure = NULL;
            }
        } else {
            assert(status.state == ROUNDIE_ELF_RUNNING && status.abi_version == 3);
            roundie_ui_status_t ui = ui_status();
            assert(ui.widgets == 6 && ui.applied == 6 && ui.foreground && ui.realized_objects == 7);
            assert(ui.button != old_button);
            assert(!roundie_ui_input(generation, old_button, REP3_ORIGIN_INJECTED));
            old_button = ui.button;
            event_count_at_input = status.events_delivered;
            current_task = (void *)2;
            lv_obj_t *button_object = lv_obj_get_child(lv_screen_active(), 3);
            lv_obj_send_event(button_object, LV_EVENT_CLICKED, NULL);
            assert(status.events_delivered == event_count_at_input); /* never recursive */
            current_task = (void *)1;
            roundie_elf_step(esp_timer_get_time());
            if (fault == 4) {
                roundie_elf_step(esp_timer_get_time());
                assert(status.state == ROUNDIE_ELF_UNLOADED);
            } else {
                assert(status.ui_delivered == 1);
                pump();
                assert(ui_status().delivered == 1 && ui_status().physical == 1);
                lv_obj_t *label = lv_obj_get_child(lv_screen_active(), 2);
                assert(strcmp(lv_label_get_text(label), "Counter: 1") == 0);
                for (unsigned i = 0; i < ROUNDIE_ELF_EVENT_CAPACITY; i++) {
                    assert(roundie_elf_post(generation, i) == ESP_OK);
                }
                assert(!roundie_ui_input(generation, ui.button, REP3_ORIGIN_INJECTED));
                control_step(ROUNDIE_ELF_SUSPEND);
                assert(!roundie_ui_input(generation, ui.button, REP3_ORIGIN_TOUCH));
                control_step(ROUNDIE_ELF_RESUME);
                if (fault == 5) {
                    stall_gui = true;
                    control_step(ROUNDIE_ELF_UNLOAD);
                    assert(status.state == ROUNDIE_ELF_STOPPING && loader_initialized && ui_owned);
                    assert(roundie_elf_busy() && ui_status().widgets == 6);
                    stall_gui = false;
                    pump();
                    roundie_elf_step(esp_timer_get_time());
                    assert(status.state == ROUNDIE_ELF_UNLOADED);
                } else {
                    control_step(ROUNDIE_ELF_UNLOAD);
                }
                assert(ui_destroy_calls == 1);
            }
        }
        roundie_elf_status_t result;
        roundie_elf_status(&result);
        assert(result.resources_outstanding == 0 && live_bytes == 0 && !loader_initialized);
        assert(ui_status().outstanding == 0 && ui_status().widgets == 0 && ui_status().pending == 0);
        assert(lv_screen_active() == home);
    }
    /* Launcher stays outside app resource ownership through a real production
     * runtime/UI destroy barrier. Package resolution/loader remain mocked. */
    fault = 0;
    current_task = (void *)2;
    roundie_launcher_gui_bind();
    current_task = (void *)1;
    roundie_elf_set_allowed(true);
    roundie_launcher_poll(true, false);
    current_task = (void *)2;
    roundie_launcher_gui_pump(false);
    home = lv_screen_active();
    static roundie_launcher_status_t navigation;
    roundie_launcher_status(&navigation);
    assert(navigation.count == 1 && navigation.foreground);
    assert(roundie_launcher_select("org.roundie.ui", navigation.revision, 1) == ESP_OK);
    current_task = (void *)1;
    roundie_launcher_poll(true, false);
    assert(status.state == ROUNDIE_ELF_LOADING);
    assert(roundie_elf_execute(fixture, fixture_size));
    roundie_launcher_poll(true, false);
    current_task = (void *)2;
    roundie_launcher_gui_pump(false);
    assert(lv_screen_active() != home && lv_obj_is_valid(home));
    lv_obj_t *exit = lv_obj_get_child(lv_layer_top(), 0);
    assert(!lv_obj_has_flag(exit, LV_OBJ_FLAG_HIDDEN));
    unsigned delivered = status.ui_delivered;
    lv_obj_send_event(exit, LV_EVENT_CLICKED, NULL);
    assert(status.ui_delivered == delivered && status.state == ROUNDIE_ELF_RUNNING);
    current_task = (void *)1;
    roundie_launcher_poll(true, false);
    assert(!roundie_ui_input(status.instance, ui_status().button, REP3_ORIGIN_TOUCH));
    roundie_elf_step(esp_timer_get_time());
    roundie_launcher_poll(true, false);
    current_task = (void *)2;
    roundie_launcher_gui_pump(false);
    roundie_launcher_status(&navigation);
    assert(navigation.state == ROUNDIE_NAV_LIST && navigation.foreground);
    assert(status.state == ROUNDIE_ELF_UNLOADED && ui_status().outstanding == 0);
    assert(lv_screen_active() == home && lv_obj_is_valid(exit));
    roundie_elf_status_t final_runtime;
    roundie_elf_status(&final_runtime);
    assert(final_runtime.resources_outstanding == 0);
    lv_deinit();
    puts("ABI3 production runtime + bridge + LVGL: lifecycle, input, no recursion, full FIFO, recovery, delayed barrier PASS (host C callbacks, mocked loader)");
    return 0;
}
