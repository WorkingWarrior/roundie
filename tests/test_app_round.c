/* Round UI: production manager, bridge, real LVGL 9.5 and the round example's
 * C callbacks on host. Loader/IDF and time are mocked. This does NOT execute
 * Xtensa instructions. */
#define ROUNDIE_UI_INTEGRATION 1
#define main elf_regression_main
#include "test_elf.c"
#undef main
#include "lvgl.h"
#include "../components/roundie_app_ui/ui_private.h"

static lv_obj_t *home;
static uint16_t pixels[360 * 20];
extern char __executable_start;
extern char etext;
extern char __data_start;
extern char _end;

void vTaskDelay(TickType_t ticks)
{
    mock_clock += (int64_t)ticks * 1000;
    TaskHandle_t saved = current_task;
    current_task = (void *)2;
    roundie_ui_gui_pump();
    current_task = saved;
    lv_tick_inc(ticks);
}

static const rep_descriptor_v3_t *test_entry(const rep_api_v3_t *table)
{
    return rep_app_entry_v3(table);
}

void mock_ui_relocate(esp_elf_t *elf)
{
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

/* GUI-task touch: LVGL changes the value, then reports it as a widget would. */
static void touch_value(lv_obj_t *object, int32_t value)
{
    TaskHandle_t saved = current_task;
    current_task = (void *)2;
    if (lv_obj_check_type(object, &lv_roller_class)) {
        lv_roller_set_selected(object, (uint32_t)value, LV_ANIM_OFF);
    } else {
        lv_arc_set_value(object, value);
    }
    lv_obj_send_event(object, LV_EVENT_VALUE_CHANGED, NULL);
    current_task = saved;
}

static void click(lv_obj_t *object)
{
    TaskHandle_t saved = current_task;
    current_task = (void *)2;
    lv_obj_send_event(object, LV_EVENT_CLICKED, NULL);
    current_task = saved;
}

static struct {
    unsigned count;
    uint32_t type;
    uint32_t widget;
    uint32_t payload;
    bool coalesce;
} sunk;
static bool sink_accepts = true;

static bool sink(uint32_t generation, uint32_t type, uint32_t widget, uint32_t payload,
                 bool coalesce, int64_t time)
{
    (void)generation;
    (void)time;
    if (!sink_accepts) {
        return false;
    }
    sunk.count++;
    sunk.type = type;
    sunk.widget = widget;
    sunk.payload = payload;
    sunk.coalesce = coalesce;
    return true;
}

static rep_ui_widget_v2_t round_widget(uint32_t type, uint32_t parent, const char *text)
{
    rep_ui_widget_v2_t widget = {0};
    widget.base.version = 2;
    widget.base.struct_size = sizeof(widget);
    widget.base.type = type;
    widget.base.parent = parent;
    widget.base.text_length = (uint32_t)strlen(text);
    memcpy(widget.base.text, text, widget.base.text_length + 1);
    return widget;
}

static int32_t set_value(uint32_t generation, uint32_t widget, uint32_t property, uint32_t value,
                         const char *text)
{
    rep_ui_update_t update = {1, sizeof(update), widget, property, value, ""};
    if (text != NULL) {
        update.value = (uint32_t)strlen(text);
        memcpy(update.text, text, update.value + 1);
    }
    return roundie_ui_update(generation, &update);
}

static void shadow(uint32_t generation, uint32_t widget, int32_t committed, int32_t requested,
                   uint32_t pending)
{
    int32_t actual_committed;
    int32_t actual_requested;
    uint32_t actual_pending;
    assert(roundie_ui_shadow(generation, widget, &actual_committed, &actual_requested, &actual_pending));
    assert(actual_committed == committed && actual_requested == requested && actual_pending == pending);
}

static void inside(lv_obj_t *object)
{
    lv_area_t area;
    lv_obj_update_layout(object);
    lv_obj_get_coords(object, &area);
    assert(roundie_ui_box_in_circle(area.x1, area.y1, area.x2 - area.x1 + 1, area.y2 - area.y1 + 1));
    assert(area.y1 >= 60); /* below the navigation safe area */
}

static void bridge_tests(void)
{
    uint32_t generation = 2000;
    assert(roundie_ui_open(generation, sink));
    rep_ui_widget_v2_t view_request = {0};
    view_request.base = (rep_ui_widget_t){1, sizeof(rep_ui_widget_t), REP_UI_VIEW, 0, 0, 0, 360, 360, 0, ""};
    uint32_t view = roundie_ui_create(generation, &view_request);
    assert(view != 0);

    /* Format and round-geometry refusals. */
    rep_ui_widget_v2_t list_request = round_widget(REP_UI_LIST, view, "Red\nGreen\nBlue");
    list_request.base.x = 60;
    list_request.base.y = 100;
    list_request.base.width = 180;
    list_request.base.height = 132;
    list_request.value = 1;
    rep_ui_widget_v2_t invalid = list_request;
    invalid.base.version = 1;
    invalid.base.struct_size = sizeof(rep_ui_widget_t);
    assert(roundie_ui_create(generation, &invalid) == 0); /* round types need version 2 */
    invalid = list_request;
    invalid.base.x = 0;
    invalid.base.y = 0;
    assert(roundie_ui_create(generation, &invalid) == 0); /* corner outside the circle */
    invalid = list_request;
    invalid.base.height = 40;
    assert(roundie_ui_create(generation, &invalid) == 0); /* below the touch minimum */
    invalid = list_request;
    invalid.value = 3;
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = round_widget(REP_UI_LIST, view, "a\n\nb");
    invalid.base.x = 60;
    invalid.base.y = 100;
    invalid.base.width = 180;
    invalid.base.height = 132;
    assert(roundie_ui_create(generation, &invalid) == 0); /* empty item */
    invalid = round_widget(REP_UI_LIST, view, "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\nm\nn\no\np\nq");
    invalid.base.x = 60;
    invalid.base.y = 100;
    invalid.base.width = 180;
    invalid.base.height = 132;
    assert(roundie_ui_create(generation, &invalid) == 0); /* 17 items */
    invalid = list_request;
    invalid.options = 2;
    assert(roundie_ui_create(generation, &invalid) == 0);

    rep_ui_widget_v2_t arc_request = round_widget(REP_UI_ARC_SLIDER, view, "");
    arc_request.minimum = -10;
    arc_request.maximum = 10;
    arc_request.value = 5;
    invalid = arc_request;
    invalid.base.width = 100;
    assert(roundie_ui_create(generation, &invalid) == 0); /* geometry is the system's */
    invalid = arc_request;
    invalid.minimum = 10;
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = arc_request;
    invalid.value = 11;
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = arc_request;
    invalid.maximum = REP_UI_SLIDER_RANGE_LIMIT + 1;
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = round_widget(REP_UI_ARC_SLIDER, view, "x");
    invalid.maximum = 1;
    assert(roundie_ui_create(generation, &invalid) == 0); /* no text */
    invalid = round_widget(REP_UI_DIALOG, view, "Only a message");
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = round_widget(REP_UI_DIALOG, view, "M\nA\nB\nC");
    assert(roundie_ui_create(generation, &invalid) == 0);

    uint32_t list = roundie_ui_create(generation, &list_request);
    uint32_t arc = roundie_ui_create(generation, &arc_request);
    rep_ui_widget_v2_t edge_request = round_widget(REP_UI_EDGE_BUTTON, view, "Reset");
    uint32_t edge = roundie_ui_create(generation, &edge_request);
    rep_ui_widget_v2_t dialog_request = round_widget(REP_UI_DIALOG, view, "Reset?\nYes\nNo");
    dialog_request.options = REP_UI_OPTION_HIDDEN;
    uint32_t dialog = roundie_ui_create(generation, &dialog_request);
    assert(list != 0 && arc != 0 && edge != 0 && dialog != 0);
    /* One system-placed widget of each kind per view. */
    assert(roundie_ui_create(generation, &arc_request) == 0);
    assert(roundie_ui_create(generation, &edge_request) == 0);
    assert(roundie_ui_create(generation, &dialog_request) == 0);
    shadow(generation, list, 1, 1, 1); /* accepted, not yet realized */
    assert(!roundie_ui_shadow(generation, edge, &(int32_t){0}, &(int32_t){0}, &(uint32_t){0}));
    assert(!roundie_ui_shadow(generation + 1, list, &(int32_t){0}, &(int32_t){0}, &(uint32_t){0}));

    assert(roundie_ui_transition(generation, ROUNDIE_UI_SHOW));
    shadow(generation, list, 1, 1, 0);
    shadow(generation, arc, 5, 5, 0);
    shadow(generation, dialog, 0, 0, 0);
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *roller = lv_obj_get_child(root, 0);
    lv_obj_t *arc_object = lv_obj_get_child(root, 1);
    lv_obj_t *edge_object = lv_obj_get_child(root, 2);
    lv_obj_t *dialog_object = lv_obj_get_child(root, 3);
    assert(lv_obj_check_type(roller, &lv_roller_class) && lv_roller_get_selected(roller) == 1);
    assert(lv_obj_check_type(arc_object, &lv_arc_class) && lv_arc_get_value(arc_object) == 5);
    assert(lv_obj_has_flag(dialog_object, LV_OBJ_FLAG_HIDDEN));
    inside(roller);
    inside(edge_object);
    lv_obj_t *primary = lv_obj_get_child(dialog_object, 1);
    lv_obj_t *secondary = lv_obj_get_child(dialog_object, 2);
    inside(primary);
    inside(secondary);
    assert(lv_obj_get_height(primary) >= 44 && lv_obj_get_height(edge_object) >= 44);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(dialog_object, 0)), "Reset?") == 0);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(primary, 0)), "Yes") == 0);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(secondary, 0)), "No") == 0);
    roundie_ui_status_t ui = ui_status();
    assert(ui.error == 0 && ui.lvgl_bytes <= 12288);
    printf("Round widgets retained %" PRIu32 " LVGL bytes in %" PRIu32 " objects\n",
           ui.lvgl_bytes, ui.realized_objects);

    /* Touch commits in GUI order; a superseded event is stale at delivery. */
    touch_value(roller, 2);
    assert(sunk.type == REP3_EVENT_VALUE && sunk.widget == list && sunk.payload == 2 && sunk.coalesce);
    shadow(generation, list, 2, 2, 0);
    assert(roundie_ui_event_valid(generation, REP3_EVENT_VALUE, list, 2));
    touch_value(roller, 0);
    assert(!roundie_ui_event_valid(generation, REP3_EVENT_VALUE, list, 2));

    /* An app request outstanding during touch: the request wins once applied. */
    assert(set_value(generation, arc, REP_UI_VALUE, 11, NULL) == -1);
    assert(set_value(generation, arc, REP_UI_VALUE, (uint32_t)-11, NULL) == -1);
    assert(set_value(generation, arc, REP_UI_VALUE, (uint32_t)-7, NULL) == 0);
    shadow(generation, arc, 5, -7, 1);
    touch_value(arc_object, 9);
    shadow(generation, arc, 9, -7, 1);
    pump();
    shadow(generation, arc, -7, -7, 0);
    assert(lv_arc_get_value(arc_object) == -7);
    assert(!roundie_ui_event_valid(generation, REP3_EVENT_VALUE, arc, 9));
    assert(set_value(generation, edge, REP_UI_VALUE, 0, NULL) == -1);
    assert(set_value(generation, arc, REP_UI_TEXT, 0, "no") == -1);
    assert(set_value(generation, dialog, REP_UI_TEXT, 0, "Only") == -1); /* field count fixed */

    /* New list items select the first; validation uses the accepted count. */
    assert(set_value(generation, list, REP_UI_TEXT, 0, "One\nTwo") == 0);
    shadow(generation, list, 0, 0, 1);
    assert(set_value(generation, list, REP_UI_VALUE, 2, NULL) == -1);
    assert(set_value(generation, list, REP_UI_VALUE, 1, NULL) == 0);
    pump();
    shadow(generation, list, 1, 1, 0);
    assert(lv_roller_get_option_count(roller) == 2 && lv_roller_get_selected(roller) == 1);

    /* Dialog: hidden choices are refused; shown, a choice hides it. */
    unsigned rejected = ui_status().input_rejected;
    assert(roundie_ui_inject_value(generation, dialog, 1));
    assert(!roundie_ui_inject_value(generation, dialog, 1)); /* one pending injection */
    pump();
    assert(ui_status().input_rejected == rejected + 2);
    assert(set_value(generation, dialog, REP_UI_VISIBLE, 1, NULL) == 0);
    pump();
    assert(!lv_obj_has_flag(dialog_object, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_index(dialog_object) == (int32_t)lv_obj_get_child_count(root) - 1);
    click(secondary);
    assert(sunk.widget == dialog && sunk.payload == 2 && !sunk.coalesce);
    assert(lv_obj_has_flag(dialog_object, LV_OBJ_FLAG_HIDDEN));
    shadow(generation, dialog, 2, 2, 0);
    assert(roundie_ui_event_valid(generation, REP3_EVENT_VALUE, dialog, 2));
    unsigned count = sunk.count;
    click(primary);
    assert(sunk.count == count); /* hidden: no further choice */
    assert(set_value(generation, dialog, REP_UI_VISIBLE, 1, NULL) == 0);
    pump();
    sink_accepts = false;
    click(primary);
    assert(!lv_obj_has_flag(dialog_object, LV_OBJ_FLAG_HIDDEN)); /* dropped: stays open */
    sink_accepts = true;
    assert(roundie_ui_inject_value(generation, dialog, 1));
    pump();
    assert(sunk.payload == 1 && lv_obj_has_flag(dialog_object, LV_OBJ_FLAG_HIDDEN));

    /* Edge button reports BUTTON; slider injection goes through LVGL. */
    click(edge_object);
    assert(sunk.type == REP3_EVENT_BUTTON && sunk.widget == edge && sunk.payload == REP3_ORIGIN_TOUCH);
    assert(roundie_ui_event_valid(generation, REP3_EVENT_BUTTON, edge, REP3_ORIGIN_TOUCH));
    unsigned injected = ui_status().injected;
    assert(roundie_ui_inject_value(generation, arc, 3));
    pump();
    assert(lv_arc_get_value(arc_object) == 3 && sunk.widget == arc && sunk.payload == 3);
    assert(ui_status().injected == injected + 1);
    assert(roundie_ui_inject_value(generation, arc, 50));
    pump();
    assert(lv_arc_get_value(arc_object) == 3); /* out of range: LVGL untouched */

    /* Disabled widgets neither report nor deliver. */
    assert(set_value(generation, list, REP_UI_ENABLED, 0, NULL) == 0);
    pump();
    assert(!roundie_ui_event_valid(generation, REP3_EVENT_VALUE, list, 1));
    count = sunk.count;
    touch_value(roller, 0);
    assert(sunk.count == count);

    assert(roundie_ui_destroy(generation, view) == 0);
    assert(roundie_ui_transition(generation, ROUNDIE_UI_DESTROY));
    assert(roundie_ui_retire(generation));
    ui = ui_status();
    assert(ui.widgets == 0 && ui.outstanding == 0 && ui.realized_objects == 0);
    assert(lv_screen_active() == home);
    puts("Round bridge + real LVGL: validation, geometry, shadow, stale values, dialog, injection, cleanup PASS");
}

static void drain(void)
{
    for (unsigned i = 0; i < 16 && mailbox.count != 0; i++) {
        roundie_elf_step(esp_timer_get_time());
    }
    pump();
}

static void round_runtime_tests(void)
{
    roundie_elf_set_allowed(true);
    assert(roundie_elf_prepare());
    assert(roundie_elf_execute(fixture, fixture_size));
    assert(status.state == ROUNDIE_ELF_RUNNING && status.abi_version == 3 && round_ui);
    uint32_t generation = status.instance;
    roundie_ui_status_t ui = ui_status();
    assert(ui.widgets == 7 && ui.applied == 7 && ui.rejected == 2 && ui.foreground);
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *color = lv_obj_get_child(root, 0);
    lv_obj_t *level = lv_obj_get_child(root, 1);
    lv_obj_t *roller = lv_obj_get_child(root, 2);
    lv_obj_t *arc = lv_obj_get_child(root, 3);
    lv_obj_t *edge = lv_obj_get_child(root, 4);
    lv_obj_t *dialog = lv_obj_get_child(root, 5);
    assert(lv_obj_has_flag(dialog, LV_OBJ_FLAG_HIDDEN));

    /* A drag's successive values coalesce into one queued event. */
    unsigned coalesced = mailbox.coalesced;
    touch_value(roller, 2);
    touch_value(arc, 10);
    touch_value(arc, 20);
    touch_value(arc, 30);
    assert(mailbox.coalesced == coalesced + 2 && mailbox.count == 2);
    drain();
    assert(strcmp(lv_label_get_text(color), "Blue") == 0);
    assert(strcmp(lv_label_get_text(level), "Level: 30") == 0);

    /* A value superseded by an applied request is rejected at delivery. */
    unsigned rejected = mailbox.rejected;
    touch_value(arc, 55);
    rep_ui_update_t update = {1, sizeof(update), 0, REP_UI_VALUE, 45, ""};
    int32_t committed;
    int32_t requested;
    uint32_t pending;
    uint32_t arc_handle = 0;
    for (uint32_t handle = 1; handle <= 1000; handle++) { /* boot-unique handles */
        if (roundie_ui_shadow(generation, handle, &committed, &requested, &pending) && committed == 55) {
            arc_handle = handle;
        }
    }
    assert(arc_handle != 0);
    update.widget = arc_handle;
    assert(roundie_ui_update(generation, &update) == 0);
    pump();
    drain();
    assert(mailbox.rejected == rejected + 1);
    assert(lv_arc_get_value(arc) == 45);

    /* Edge button opens the dialog; Reset requests 40 and reads the shadow. */
    click(edge);
    drain();
    assert(!lv_obj_has_flag(dialog, LV_OBJ_FLAG_HIDDEN));
    click(lv_obj_get_child(dialog, 1));
    assert(lv_obj_has_flag(dialog, LV_OBJ_FLAG_HIDDEN));
    drain();
    assert(lv_arc_get_value(arc) == 40 && strcmp(lv_label_get_text(level), "Level: 40") == 0);
    assert(roundie_ui_shadow(generation, arc_handle, &committed, &requested, &pending));
    assert(committed == 40 && requested == 40 && pending == 0);

    /* Timer reconciles the label with the committed shadow value. */
    unsigned timers = status.timers_delivered;
    mock_clock += 1100000;
    roundie_elf_step(esp_timer_get_time()); /* posts the due timer */
    drain();
    assert(status.timers_delivered > timers && status.state == ROUNDIE_ELF_RUNNING);
    assert(strcmp(lv_label_get_text(level), "Level: 40") == 0);

    /* The capability gates version-2 requests and shadow reads. */
    /* Static: the runtime accepts only loaded data or SDK memory, not stacks. */
    static rep_ui_widget_v2_t extra;
    static rep_ui_result_v2_t result;
    extra = round_widget(REP_UI_LIST, ui_status().view, "A");
    extra.base.x = 120;
    extra.base.y = 150;
    extra.base.width = 120;
    extra.base.height = 60;
    result = (rep_ui_result_v2_t){{2, sizeof(result), 0, 0, 0, 0, 0, 0}, arc_handle, 0, 0, 0};
    callback_active = true; /* as from inside a native callback */
    round_ui = false;
    assert(app_ui_create(generation, &extra.base) == 0);
    assert(app_ui_result(generation, &result.base) == -1);
    round_ui = true;
    assert(app_ui_create(generation, &extra.base) != 0); /* eighth widget, gate open */
    assert(app_ui_result(generation, &result.base) == 0);
    assert(result.base.version == 2 && result.committed == 40 && result.pending == 0);
    result.widget = UINT32_MAX;
    assert(app_ui_result(generation, &result.base) == -1);
    result.base = (rep_ui_result_t){1, sizeof(rep_ui_result_t), 0, 0, 0, 0, 0, 0};
    assert(app_ui_result(generation, &result.base) == 0 && result.base.version == 1);
    callback_active = false;

    control_step(ROUNDIE_ELF_UNLOAD);
    roundie_elf_status_t final_status;
    roundie_elf_status(&final_status);
    assert(status.state == ROUNDIE_ELF_UNLOADED && final_status.resources_outstanding == 0);
    assert(ui_status().outstanding == 0 && ui_status().widgets == 0 && live_bytes == 0);
    assert(lv_screen_active() == home);
    puts("Round example + production runtime: coalescing, stale rejection, dialog, shadow, capability gate PASS (host C callbacks, mocked loader)");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    lv_init();
    lv_display_t *display = lv_display_create(360, 360);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    home = lv_screen_active();
    current_task = (void *)2;
    roundie_ui_gui_bind();
    current_task = (void *)1;
    bridge_tests();
    FILE *file = fopen(argv[1], "rb");
    assert(file != NULL);
    fixture_size = fread(fixture, 1, sizeof(fixture), file);
    assert(fclose(file) == 0);
    roundie_elf_bind_executor((void *)1);
    mode = 0;
    round_runtime_tests();
    lv_deinit();
    return 0;
}
