/* Round UI: production manager, bridge, real LVGL 9.5 and the round example's
 * C callbacks on host. Loader/IDF and time are mocked. This does NOT execute
 * Xtensa instructions. */
#define ROUNDIE_UI_INTEGRATION 1
#define main elf_regression_main
#include "test_elf.c"
#undef main
#include "lvgl.h"
#include "roundie_fonts.h"
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

/* The watch and stopwatch examples are compiled with renamed exports
 * (tests/app_ui/CMakeLists.txt). */
const rep_descriptor_v3_t *watch_app_entry_v3(const rep_api_v3_t *table);
const rep_descriptor_v3_t *stopwatch_app_entry_v3(const rep_api_v3_t *table);
static bool use_watch;
static bool use_stopwatch;

static const rep_descriptor_v3_t *test_entry(const rep_api_v3_t *table)
{
    if (use_stopwatch) {
        return stopwatch_app_entry_v3(table);
    }
    return use_watch ? watch_app_entry_v3(table) : rep_app_entry_v3(table);
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

static void send_input(lv_obj_t *object, lv_event_code_t code)
{
    TaskHandle_t saved = current_task;
    current_task = (void *)2;
    lv_obj_send_event(object, code, NULL);
    current_task = saved;
}

static void click(lv_obj_t *object)
{
    send_input(object, LV_EVENT_CLICKED);
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
    /* Touch routing through LVGL hit testing: the full-panel arc object lies
     * above the list but owns only its ring on the right edge. */
    lv_obj_update_layout(root);
    lv_point_t list_centre = {150, 166};
    lv_point_t list_right = {235, 105};
    lv_point_t ring = {335, 180};
    lv_point_t ring_inner = {317, 180};
    lv_point_t left_ring = {25, 180};
    assert(lv_indev_search_obj(root, &list_centre) == roller);
    assert(lv_indev_search_obj(root, &list_right) == roller);
    assert(lv_indev_search_obj(root, &ring) == arc_object);
    assert(lv_indev_search_obj(root, &ring_inner) == arc_object);
    assert(lv_indev_search_obj(root, &left_ring) != arc_object);
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

static lv_obj_t *child(lv_obj_t *parent, unsigned index)
{
    return lv_obj_get_child(parent, (int32_t)index);
}

static void clock_widget_tests(void)
{
    uint32_t generation = 3000;
    assert(roundie_ui_open(generation, sink));
    rep_ui_widget_v2_t view_request = {0};
    view_request.base = (rep_ui_widget_t){1, sizeof(rep_ui_widget_t), REP_UI_VIEW, 0, 0, 0, 360, 360, 0, ""};
    uint32_t view = roundie_ui_create(generation, &view_request);
    rep_ui_widget_v2_t hands_request = round_widget(REP_UI_CLOCK_HANDS, view, "");
    hands_request.value = 900; /* 00:15:00 */
    rep_ui_widget_v2_t invalid = hands_request;
    invalid.base.width = 100;
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = hands_request;
    invalid.value = REP_UI_CLOCK_HANDS_PERIOD;
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = round_widget(REP_UI_CLOCK_HANDS, view, "12");
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid = hands_request;
    invalid.options = REP_UI_OPTION_FONT_LARGE;
    assert(roundie_ui_create(generation, &invalid) == 0);
    uint32_t hands = roundie_ui_create(generation, &hands_request);
    assert(hands != 0 && roundie_ui_create(generation, &hands_request) == 0);

    rep_ui_widget_v2_t label_request = round_widget(REP_UI_LABEL, view, "12:34");
    label_request.base.x = 60;
    label_request.base.y = 200;
    label_request.base.width = 240;
    label_request.base.height = 60;
    label_request.options = REP_UI_OPTION_FONT_MEDIUM | REP_UI_OPTION_FONT_LARGE;
    assert(roundie_ui_create(generation, &label_request) == 0);
    label_request.options = REP_UI_OPTION_NO_SECONDS;
    assert(roundie_ui_create(generation, &label_request) == 0);
    label_request.options = REP_UI_OPTION_LONG_PRESS; /* dial only */
    assert(roundie_ui_create(generation, &label_request) == 0);
    label_request.options = REP_UI_OPTION_FONT_LARGE;
    uint32_t label = roundie_ui_create(generation, &label_request);
    assert(label != 0);
    assert(roundie_ui_transition(generation, ROUNDIE_UI_SHOW));
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *scale = child(root, 0);
    lv_obj_t *text = child(root, 1);
    assert(lv_obj_check_type(scale, &lv_scale_class) && lv_obj_get_child_count(scale) == 3);
    assert(lv_obj_get_style_text_font(text, LV_PART_MAIN) == &roundie_font_48);
    shadow(generation, hands, 900, 900, 0);

    /* Minute hand at 15 minutes points right; the hour hand barely moves. */
    lv_obj_t *minute = child(scale, 1);
    lv_obj_update_layout(root);
    const lv_point_precise_t *points = lv_line_get_points(minute);
    assert(lv_line_get_point_count(minute) == 2);
    assert(points[1].x - points[0].x > 120 && points[1].y - points[0].y < 5 &&
           points[1].y - points[0].y > -5);
    assert(set_value(generation, hands, REP_UI_VALUE, REP_UI_CLOCK_HANDS_PERIOD, NULL) == -1);
    assert(set_value(generation, hands, REP_UI_VALUE, 2700, NULL) == 0); /* 00:45:00 */
    assert(set_value(generation, hands, REP_UI_TEXT, 0, "x") == -1);
    pump();
    shadow(generation, hands, 2700, 2700, 0);
    points = lv_line_get_points(minute);
    assert(points[0].x - points[1].x > 120);
    unsigned rejected = ui_status().input_rejected;
    assert(roundie_ui_inject_value(generation, hands, 10));
    pump();
    assert(ui_status().input_rejected == rejected + 1);
    shadow(generation, hands, 2700, 2700, 0); /* display only: no input */
    unsigned accepted = ui_status().input_accepted;
    send_input(scale, LV_EVENT_LONG_PRESSED); /* without the option: no event */
    assert(ui_status().input_accepted == accepted && sunk.widget != hands);

    /* Colors, including the root view; a value above 24 bits is refused. */
    assert(set_value(generation, view, REP_UI_COLOR, 0x1000000, NULL) == -1);
    assert(set_value(generation, view, REP_UI_COLOR, 0x021a0c, NULL) == 0);
    assert(set_value(generation, view, REP_UI_VISIBLE, 0, NULL) == -1);
    assert(set_value(generation, label, REP_UI_COLOR, 0x1aff80, NULL) == 0);
    assert(set_value(generation, hands, REP_UI_COLOR, 0x1aff80, NULL) == 0);
    pump();
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(root, LV_PART_MAIN)) == 0xff021a0c);
    assert(lv_color_to_u32(lv_obj_get_style_text_color(text, LV_PART_MAIN)) == 0xff1aff80);
    assert(lv_color_to_u32(lv_obj_get_style_line_color(minute, LV_PART_MAIN)) == 0xff1aff80);

    /* The dial passes touches through. */
    lv_point_t centre = {180, 120};
    lv_obj_t *found = lv_indev_search_obj(root, &centre);
    assert(found != scale && found != minute);
    assert(roundie_ui_destroy(generation, view) == 0);
    assert(roundie_ui_transition(generation, ROUNDIE_UI_DESTROY));
    assert(roundie_ui_retire(generation));
    assert(ui_status().outstanding == 0 && ui_status().realized_objects == 0);
    puts("Clock hands, styled labels and colors + real LVGL: validation, needles, shadow, pass-through PASS");
}

static roundie_elf_clock_t test_clock;
static bool test_clock_valid;
static unsigned test_clock_reads;

static bool clock_source_mock(roundie_elf_clock_t *out)
{
    test_clock_reads++;
    if (!test_clock_valid) {
        return false;
    }
    *out = test_clock;
    out->monotonic_us = esp_timer_get_time();
    return true;
}

static void step_after(int64_t microseconds)
{
    mock_clock += microseconds;
    roundie_elf_step(esp_timer_get_time());
    drain();
}

static void watch_runtime_tests(void)
{
    use_watch = true;
    roundie_elf_set_clock_source(clock_source_mock);
    test_clock_valid = false;
    test_clock = (roundie_elf_clock_t){0, 20760, (12U * 3600U + 34U * 60U + 56U) * 1000U, 1};
    roundie_elf_set_allowed(true);
    assert(roundie_elf_prepare());
    assert(roundie_elf_execute(fixture, fixture_size));
    assert(status.state == ROUNDIE_ELF_RUNNING && clock_app && round_ui);
    uint32_t generation = status.instance;
    roundie_ui_status_t ui = ui_status();
    assert(ui.widgets == 4 && ui.failed == 0 && ui.foreground);
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *dial = child(root, 0);
    lv_obj_t *date = child(root, 1);
    lv_obj_t *time_text = child(root, 2);
    assert(lv_obj_get_child_count(root) == 3); /* no buttons on the face */
    assert(strcmp(lv_label_get_text(time_text), "--:--") == 0);
    assert(strcmp(lv_label_get_text(date), "Ustaw czas") == 0);

    /* No valid wall time: no clock event, the face waits. */
    step_after(1100000);
    assert(strcmp(lv_label_get_text(time_text), "--:--") == 0);
    unsigned reads = test_clock_reads;
    roundie_elf_step(esp_timer_get_time());
    assert(test_clock_reads == reads); /* sampled at most once per second */

    test_clock_valid = true;
    step_after(1100000);
    assert(strcmp(lv_label_get_text(time_text), "12:34") == 0);
    assert(strcmp(lv_label_get_text(date), "WT 3 LIS") == 0);
    uint32_t hands = 0;
    int32_t committed = 0;
    int32_t requested;
    uint32_t pending;
    for (uint32_t handle = 1; handle <= 2000 && hands == 0; handle++) {
        if (roundie_ui_shadow(generation, handle, &committed, &requested, &pending) &&
            committed >= 2096 && committed <= 2098) {
            hands = handle;
        }
    }
    assert(hands != 0); /* 12:34:56 is 2096 s into the 12-hour dial */

    /* The timer advances the hands between minute events. */
    step_after(2100000);
    assert(roundie_ui_shadow(generation, hands, &committed, &requested, &pending));
    assert(committed >= 2098 && committed <= 2101);

    /* A zone change (new revision) is posted at the next check, not a minute later. */
    test_clock.ms_of_day += 3600000U;
    test_clock.revision++;
    step_after(1100000);
    assert(strcmp(lv_label_get_text(time_text), "13:35") == 0 ||
           strcmp(lv_label_get_text(time_text), "13:34") == 0);

    /* Queued clock samples coalesce into the newest one. */
    unsigned count = mailbox.count;
    assert(post_with_origin(generation, REP3_EVENT_CLOCK, 20761, 0, false, esp_timer_get_time()) == ESP_OK);
    assert(post_with_origin(generation, REP3_EVENT_CLOCK, 20761, 1000, false, esp_timer_get_time()) == ESP_OK);
    assert(mailbox.count == count + 1);
    drain();
    assert(strcmp(lv_label_get_text(date), "ŚR 4 LIS") == 0);
    assert(strcmp(lv_label_get_text(time_text), "00:00") == 0);

    /* A long press anywhere on the dial, including over the labels, cycles the
     * theme through COLOR updates. A short tap does nothing. */
    lv_point_t over_time = {180, 230};
    lv_point_t over_date = {180, 118};
    lv_obj_update_layout(root);
    assert(lv_indev_search_obj(root, &over_time) == dial);
    assert(lv_indev_search_obj(root, &over_date) == dial);
    click(dial);
    drain();
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(root, LV_PART_MAIN)) == 0xff021a0c);
    send_input(dial, LV_EVENT_LONG_PRESSED);
    drain();
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(root, LV_PART_MAIN)) == 0xff0b1220);
    /* RP1 injection of the dial is a long press. */
    assert(roundie_ui_inject_value(generation, hands, 0));
    pump();
    drain();
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(root, LV_PART_MAIN)) == 0xff000000);
    assert(ui_status().failed == 0 && ui_status().error == 0);

    control_step(ROUNDIE_ELF_UNLOAD);
    assert(status.state == ROUNDIE_ELF_UNLOADED && ui_status().outstanding == 0);
    assert(lv_screen_active() == home && live_bytes == 0);
    roundie_elf_set_clock_source(NULL);
    use_watch = false;
    puts("Watch example + production runtime: clock events, extrapolation, zone change, coalescing, themes PASS (host C callbacks, mocked loader)");
}

static const char *button_text(lv_obj_t *button)
{
    return lv_label_get_text(lv_obj_get_child(button, 0));
}

static bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static void stopwatch_runtime_tests(void)
{
    use_stopwatch = true;
    roundie_elf_set_allowed(true);
    assert(roundie_elf_prepare());
    assert(roundie_elf_execute(fixture, fixture_size));
    assert(status.state == ROUNDIE_ELF_RUNNING && round_ui);
    uint32_t generation = status.instance;
    roundie_ui_status_t ui = ui_status();
    assert(ui.widgets == 6 && ui.failed == 0 && ui.foreground);
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *time_text = child(root, 1);
    lv_obj_t *laps_text = child(root, 2);
    lv_obj_t *lap = child(root, 3);
    lv_obj_t *edge = child(root, 4);
    assert(strcmp(lv_label_get_text(time_text), "00:00.0") == 0);
    assert(strcmp(lv_label_get_text(laps_text), "") == 0);
    assert(strcmp(button_text(edge), "Start") == 0 && strcmp(button_text(lap), "Okrążenie") == 0);
    assert(lv_obj_has_state(lap, LV_STATE_DISABLED) && status.timer_handle == 0);

    /* Start: the 100 ms timer runs and the time advances. */
    click(edge);
    drain();
    assert(strcmp(button_text(edge), "Stop") == 0 && !lv_obj_has_state(lap, LV_STATE_DISABLED));
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(edge, LV_PART_MAIN)) == 0xffc0392b);
    assert(status.timer_handle != 0);
    step_after(1250000);
    assert(starts_with(lv_label_get_text(time_text), "00:01."));

    /* Laps, newest first. */
    click(lap);
    drain();
    assert(starts_with(lv_label_get_text(laps_text), "1.  00:01."));
    step_after(2100000);
    click(lap);
    drain();
    const char *laps = lv_label_get_text(laps_text);
    assert(starts_with(laps, "2.  00:02.") && strstr(laps, "\n1.  00:01.") != NULL);

    /* Stop: the timer is cancelled, time holds, the top button resets. */
    click(edge);
    drain();
    assert(strcmp(button_text(edge), "Start") == 0 && strcmp(button_text(lap), "Zeruj") == 0);
    assert(status.timer_handle == 0);
    char held[16];
    snprintf(held, sizeof(held), "%s", lv_label_get_text(time_text));
    assert(starts_with(held, "00:03."));
    step_after(2000000);
    assert(strcmp(lv_label_get_text(time_text), held) == 0);
    click(lap);
    drain();
    assert(strcmp(lv_label_get_text(time_text), "00:00.0") == 0);
    assert(strcmp(lv_label_get_text(laps_text), "") == 0);
    assert(lv_obj_has_state(lap, LV_STATE_DISABLED) && strcmp(button_text(lap), "Okrążenie") == 0);

    /* Past an hour: "H:MM:SS.d", and the dial shows the elapsed seconds. */
    click(edge);
    drain();
    step_after(INT64_C(3601000000));
    assert(starts_with(lv_label_get_text(time_text), "1:00:0"));
    uint32_t dial = 0;
    int32_t committed = 0;
    int32_t requested;
    uint32_t pending;
    for (uint32_t handle = 1; handle <= 3000 && dial == 0; handle++) {
        if (roundie_ui_shadow(generation, handle, &committed, &requested, &pending) && committed >= 3600) {
            dial = handle;
        }
    }
    assert(dial != 0 && committed >= 3600 && committed <= 3610);
    assert(ui_status().failed == 0 && ui_status().error == 0);

    control_step(ROUNDIE_ELF_UNLOAD);
    assert(status.state == ROUNDIE_ELF_UNLOADED && ui_status().outstanding == 0);
    assert(lv_screen_active() == home && live_bytes == 0);
    use_stopwatch = false;
    puts("Stopwatch example + production runtime: start, laps, stop, reset, hour format, dial PASS (host C callbacks, mocked loader)");
}

/* Every generated font has the Polish letters, ASCII and an LVGL symbol. */
static void font_tests(void)
{
    static const uint32_t letters[] = {
        0xD3, 0xF3, 0x104, 0x105, 0x106, 0x107, 0x118, 0x119, 0x141,
        0x142, 0x143, 0x144, 0x15A, 0x15B, 0x179, 0x17A, 0x17B, 0x17C,
    };
    const lv_font_t *fonts[] = {&roundie_font_14, &roundie_font_28, &roundie_font_48};
    assert(LV_FONT_DEFAULT == &roundie_font_14);
    for (size_t font = 0; font < sizeof(fonts) / sizeof(fonts[0]); font++) {
        lv_font_glyph_dsc_t glyph;
        for (size_t letter = 0; letter < sizeof(letters) / sizeof(letters[0]); letter++) {
            assert(lv_font_get_glyph_dsc(fonts[font], &glyph, letters[letter], 0));
            assert(glyph.box_w > 0);
        }
        assert(lv_font_get_glyph_dsc(fonts[font], &glyph, 'A', 0));
        assert(lv_font_get_glyph_dsc(fonts[font], &glyph, 0xF00C, 0)); /* LV_SYMBOL_OK */
        assert(!lv_font_get_glyph_dsc(fonts[font], &glyph, 0x0416, 0)); /* Cyrillic: absent */
    }
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
    font_tests();
    bridge_tests();
    clock_widget_tests();
    FILE *file = fopen(argv[1], "rb");
    assert(file != NULL);
    fixture_size = fread(fixture, 1, sizeof(fixture), file);
    assert(fclose(file) == 0);
    roundie_elf_bind_executor((void *)1);
    mode = 0;
    round_runtime_tests();
    watch_runtime_tests();
    stopwatch_runtime_tests();
    lv_deinit();
    return 0;
}
