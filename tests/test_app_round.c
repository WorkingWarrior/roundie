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
const rep_descriptor_v3_t *sound_app_entry_v3(const rep_api_v3_t *table);
static bool use_watch;
static bool use_stopwatch;
static bool use_sound;

static const rep_descriptor_v3_t *test_entry(const rep_api_v3_t *table)
{
    if (use_stopwatch) {
        return stopwatch_app_entry_v3(table);
    }
    if (use_sound) {
        return sound_app_entry_v3(table);
    }
    return use_watch ? watch_app_entry_v3(table) : rep_app_entry_v3(table);
}

void mock_ui_relocate(esp_elf_t *elf)
{
    elf->sec[ELF_SEC_TEXT] = (esp_elf_sec_t){(uintptr_t)&__executable_start,
        (uintptr_t)&etext - (uintptr_t)&__executable_start, 0};
    elf->sec[ELF_SEC_DATA] = (esp_elf_sec_t){(uintptr_t)&__data_start,
        (uintptr_t)&_end - (uintptr_t)&__data_start, 0};
    elf->sec[ELF_SEC_RODATA] = (esp_elf_sec_t){(uintptr_t)&etext,
        (uintptr_t)&__data_start - (uintptr_t)&etext, 0};
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
    bool valid_at_post;
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
    /* The executor may deliver before the bridge's sink call returns; the
     * event must already pass the delivery check (manager.c) here. */
    sunk.valid_at_post = roundie_ui_event_valid(generation, type, widget, payload);
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
    /* An input read can hit-test the slider before the next layout refresh.
     * A zero-size arc then put its centre at the root origin, and LVGL's
     * lv_atan2(0, 0) divided by zero: a device panic on 0.3.51 (M8). */
    lv_point_t root_origin = {0, 0};
    assert(lv_indev_search_obj(root, &root_origin) != arc_object);
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
    assert(sunk.widget == dialog && sunk.payload == 2 && !sunk.coalesce && sunk.valid_at_post);
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
    shadow(generation, dialog, 2, 2, 0); /* and the earlier choice stays committed */
    sink_accepts = true;
    assert(roundie_ui_inject_value(generation, dialog, 1));
    pump();
    assert(sunk.payload == 1 && sunk.valid_at_post && lv_obj_has_flag(dialog_object, LV_OBJ_FLAG_HIDDEN));

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

static void preview(const char *name);

static void round_runtime_tests(void)
{
    roundie_elf_set_allowed(true);
    assert(roundie_elf_prepare());
    assert(roundie_elf_execute(fixture, fixture_size));
    assert(status.state == ROUNDIE_ELF_RUNNING && status.abi_version == 3 && round_ui);
    uint32_t generation = status.instance;
    roundie_ui_status_t ui = ui_status();
    assert(ui.widgets == 7 && ui.applied == 7 && ui.rejected == 2 && ui.foreground);
    preview("round");
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
    invalid.options = REP_UI_OPTION_TAP | REP_UI_OPTION_LONG_PRESS;
    assert(roundie_ui_create(generation, &invalid) == 0);
    invalid.options = REP_UI_OPTION_SECOND_RING | REP_UI_OPTION_NO_SECONDS;
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

    /* Regression, device 0.3.48: after about 9.7 hours the watch's UI charge
     * crept past 16 KiB (about 28 B per minute) and an update failed. Updates
     * must not drift the charge; 14 hours of minutes with rendering between. */
    uint32_t charged = ui_status().lvgl_bytes;
    uint32_t pool_free = 0;
    for (unsigned minute = 0; minute < 14U * 60U; minute++) {
        test_clock.ms_of_day = (test_clock.ms_of_day + 60000U) % 86400000U;
        test_clock.revision++;
        step_after(1100000);
        TaskHandle_t saved = current_task;
        current_task = (void *)2;
        lv_refr_now(NULL);
        current_task = saved;
        if (minute == 59U) {
            pool_free = ui_status().pool_free; /* after the first hour's minutes */
        }
    }
    printf("Watch over 14 h of minutes: UI charge %" PRIu32 " -> %" PRIu32 " B, LVGL pool free after 1 h %" PRIu32 ", after 14 h %" PRIu32 " B\n",
           charged, ui_status().lvgl_bytes, pool_free, ui_status().pool_free);
    assert(ui_status().failed == 0 && ui_status().error == 0 && status.state == ROUNDIE_ELF_RUNNING);
    assert(ui_status().lvgl_bytes == charged);
    assert(ui_status().pool_free >= pool_free); /* no real growth over the next 13 hours */

    control_step(ROUNDIE_ELF_UNLOAD);
    assert(status.state == ROUNDIE_ELF_UNLOADED && ui_status().outstanding == 0);
    assert(lv_screen_active() == home && live_bytes == 0);
    /* Launched without storage: rep_storage_v1 was NULL, nothing to save. */
    uint8_t saved[REP_STORAGE_CAPACITY];
    size_t saved_length;
    assert(!roundie_elf_storage_take(saved, &saved_length));

    /* With storage: the stored theme (blue) is applied at start; a long press
     * stores the next one, kept in RAM until the worker takes it after unload. */
    assert(roundie_elf_storage_set("\x01\x01", 2));
    assert(roundie_elf_prepare());
    assert(roundie_elf_execute(fixture, fixture_size));
    assert(status.state == ROUNDIE_ELF_RUNNING);
    generation = status.instance;
    root = lv_screen_active();
    dial = child(root, 0);
    pump();
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(root, LV_PART_MAIN)) == 0xff0b1220);
    assert(rep_storage_v1(REP_STORAGE_V1) == NULL); /* outside a callback */
    assert(!roundie_elf_storage_set("", 0) && !roundie_elf_storage_take(saved, &saved_length));
    send_input(dial, LV_EVENT_LONG_PRESSED);
    drain();
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(root, LV_PART_MAIN)) == 0xff000000);
    /* Argument checks, as from inside a native callback. */
    static uint8_t app_buffer[REP_STORAGE_CAPACITY + 1];
    uint8_t stack_buffer[4];
    callback_active = true;
    const rep_storage_v1_t *store = rep_storage_v1(REP_STORAGE_V1);
    assert(store != NULL && store->capacity == REP_STORAGE_CAPACITY && rep_storage_v1(2) == NULL);
    assert(store->read(generation, app_buffer, 1) == -1);             /* below the stored length */
    assert(store->read(generation, stack_buffer, sizeof(stack_buffer)) == -1); /* not app memory */
    assert(store->read(generation + 1, app_buffer, sizeof(app_buffer)) == -1);
    assert(store->read(generation, app_buffer, sizeof(app_buffer)) == 2 &&
           app_buffer[0] == 1 && app_buffer[1] == 2);
    assert(store->write(generation, app_buffer, REP_STORAGE_CAPACITY + 1) == -1);
    assert(store->write(generation, stack_buffer, 1) == -1);
    callback_active = false;
    control_step(ROUNDIE_ELF_UNLOAD);
    assert(status.state == ROUNDIE_ELF_UNLOADED && !storage.available);
    assert(!roundie_elf_storage_set("\x01\x00", 2)); /* the written blob is not taken yet */
    assert(roundie_elf_storage_take(saved, &saved_length) && saved_length == 2 &&
           saved[0] == 1 && saved[1] == 2);
    assert(!roundie_elf_storage_take(saved, &saved_length));
    /* A failed submission withdraws the blob from later instances. */
    assert(roundie_elf_storage_set("\x01\x00", 2));
    roundie_elf_storage_clear();
    assert(!storage.available);
    roundie_elf_set_clock_source(NULL);
    use_watch = false;
    puts("Watch example + production runtime: clock events, extrapolation, zone change, coalescing, themes, stored theme PASS (host C callbacks, mocked loader)");
}

/* With ROUNDIE_UI_PREVIEW=<dir>, writes the active screen as <dir>/<name>.ppm,
 * masked to the round panel: a host preview of what the device shows. */
static void preview(const char *name)
{
    const char *directory = getenv("ROUNDIE_UI_PREVIEW");
    if (directory == NULL) {
        return;
    }
    /* Outside the 64 KiB LVGL pool, which cannot hold a full frame. */
    LV_DRAW_BUF_DEFINE_STATIC(snapshot, 360, 360, LV_COLOR_FORMAT_RGB888);
    LV_DRAW_BUF_INIT_STATIC(snapshot);
    lv_draw_buf_t *frame = &snapshot;
    assert(lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB888, frame) == LV_RESULT_OK);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    fprintf(file, "P6\n360 360\n255\n");
    for (int32_t y = 0; y < 360; y++) {
        const uint8_t *row = frame->data + y * frame->header.stride;
        for (int32_t x = 0; x < 360; x++) {
            int32_t dx = 2 * x + 1 - 360;
            int32_t dy = 2 * y + 1 - 360;
            bool inside = dx * dx + dy * dy <= 360 * 360;
            uint8_t pixel[3] = {0, 0, 0};
            if (inside) {
                pixel[0] = row[3 * x + 2]; /* LVGL RGB888 is B, G, R */
                pixel[1] = row[3 * x + 1];
                pixel[2] = row[3 * x];
            }
            assert(fwrite(pixel, 1, 3, file) == 3);
        }
    }
    assert(fclose(file) == 0);
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
    assert(ui.widgets == 5 && ui.failed == 0 && ui.foreground);
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *dial = child(root, 0);
    lv_obj_t *time_text = child(root, 1);
    lv_obj_t *laps_text = child(root, 2);
    lv_obj_t *lap = child(root, 3);
    assert(lv_obj_get_child_count(root) == 4); /* no edge button */
    assert(strcmp(lv_label_get_text(time_text), "00:00.0") == 0);
    assert(strcmp(lv_label_get_text(laps_text), "Dotknij, aby uruchomić") == 0);
    assert(strcmp(button_text(lap), "Okrążenie") == 0);
    assert(lv_obj_has_state(lap, LV_STATE_DISABLED) && status.timer_handle == 0);
    /* Taps anywhere on the face reach the dial, except on the lap button. */
    lv_obj_update_layout(root);
    lv_point_t over_time = {180, 166};
    lv_point_t over_laps = {180, 250};
    lv_point_t on_button = {180, 96};
    assert(lv_indev_search_obj(root, &over_time) == dial);
    assert(lv_indev_search_obj(root, &over_laps) == dial);
    assert(lv_indev_search_obj(root, &on_button) == lap);
    preview("stopwatch-zero");

    /* A long press does nothing; a tap starts: the 100 ms timer runs. */
    send_input(dial, LV_EVENT_LONG_PRESSED);
    drain();
    assert(status.timer_handle == 0);
    click(dial);
    drain();
    assert(status.timer_handle != 0 && !lv_obj_has_state(lap, LV_STATE_DISABLED));
    assert(strcmp(lv_label_get_text(laps_text), "") == 0);
    step_after(1250000);
    assert(starts_with(lv_label_get_text(time_text), "00:01."));
    lv_obj_t *ring = lv_obj_get_child(dial, 0);
    assert(lv_obj_check_type(ring, &lv_arc_class) && lv_arc_get_value(ring) == 1);
    assert(!lv_obj_has_flag(ring, LV_OBJ_FLAG_CLICKABLE)); /* taps go to the dial */

    /* Laps, newest first; five shown. */
    click(lap);
    drain();
    assert(starts_with(lv_label_get_text(laps_text), "1.  00:01."));
    step_after(2100000);
    click(lap);
    drain();
    const char *laps = lv_label_get_text(laps_text);
    assert(starts_with(laps, "2.  00:02.") && strstr(laps, "\n1.  00:01.") != NULL);
    for (unsigned i = 0; i < 4; i++) {
        step_after(300000);
        click(lap);
        drain();
    }
    laps = lv_label_get_text(laps_text);
    assert(starts_with(laps, "6.  00:00.") && strstr(laps, "\n2.  00:02.") != NULL &&
           strstr(laps, "1.  ") == NULL);
    preview("stopwatch-running");

    /* A tap stops: the timer is cancelled, time holds and turns amber, the
     * button resets. */
    click(dial);
    drain();
    assert(strcmp(button_text(lap), "Zeruj") == 0 && status.timer_handle == 0);
    assert(lv_color_to_u32(lv_obj_get_style_text_color(time_text, LV_PART_MAIN)) == 0xffff9f0a);
    char held[16];
    snprintf(held, sizeof(held), "%s", lv_label_get_text(time_text));
    assert(starts_with(held, "00:04."));
    step_after(2000000);
    assert(strcmp(lv_label_get_text(time_text), held) == 0);
    preview("stopwatch-stopped");
    click(lap);
    drain();
    assert(strcmp(lv_label_get_text(time_text), "00:00.0") == 0);
    assert(strcmp(lv_label_get_text(laps_text), "Dotknij, aby uruchomić") == 0);
    assert(lv_obj_has_state(lap, LV_STATE_DISABLED) && strcmp(button_text(lap), "Okrążenie") == 0);
    assert(lv_color_to_u32(lv_obj_get_style_text_color(time_text, LV_PART_MAIN)) == 0xffffffff);

    /* Backpressure (E0 A6 worst_stack): stop and reset handled before the GUI
     * pumps once queue 13 updates into the 12-entry UI queue. The refused
     * update is not an error: the app keeps running and the next tick redraws
     * the whole state, then the retry timer stops. */
    click(dial);
    drain();
    step_after(1250000);
    click(lap);
    drain();
    uint32_t refused = ui_status().rejected_full;
    click(dial);
    click(lap);
    drain();
    assert(ui_status().rejected_full == refused + 1);
    assert(status.state == ROUNDIE_ELF_RUNNING && ui_status().error == 0);
    assert(status.timer_handle != 0); /* the redraw tick */
    step_after(100000); /* one 100 ms tick */
    assert(status.timer_handle == 0);
    assert(strcmp(lv_label_get_text(time_text), "00:00.0") == 0);
    assert(strcmp(lv_label_get_text(laps_text), "Dotknij, aby uruchomić") == 0);
    assert(lv_obj_has_state(lap, LV_STATE_DISABLED) && strcmp(button_text(lap), "Okrążenie") == 0);
    assert(lv_color_to_u32(lv_obj_get_style_text_color(time_text, LV_PART_MAIN)) == 0xffffffff);
    assert(lv_arc_get_value(ring) == 0);

    /* Past an hour: "H:MM:SS.d"; the dial's value is the elapsed seconds. An
     * RP1 injection on the dial is a tap: it stops the stopwatch. */
    click(dial);
    drain();
    step_after(INT64_C(3601000000));
    assert(starts_with(lv_label_get_text(time_text), "1:00:0"));
    uint32_t handle = 0;
    int32_t committed = 0;
    int32_t requested;
    uint32_t pending;
    for (uint32_t candidate = 1; candidate <= 3000 && handle == 0; candidate++) {
        if (roundie_ui_shadow(generation, candidate, &committed, &requested, &pending) && committed >= 3600) {
            handle = candidate;
        }
    }
    assert(handle != 0 && committed >= 3600 && committed <= 3610);
    assert(roundie_ui_inject_value(generation, handle, 0));
    pump();
    drain();
    assert(status.timer_handle == 0 && strcmp(button_text(lap), "Zeruj") == 0);
    assert(ui_status().failed == 0 && ui_status().error == 0);

    control_step(ROUNDIE_ELF_UNLOAD);
    assert(status.state == ROUNDIE_ELF_UNLOADED && ui_status().outstanding == 0);
    assert(lv_screen_active() == home && live_bytes == 0);
    use_stopwatch = false;
    puts("Stopwatch example + production runtime: tap start/stop, laps, reset, hour format, dial PASS (host C callbacks, mocked loader)");
}

/* Sound example on rep_audio_v1: the timer feeds the stream until the tune is
 * written, then it closes with drain; the button stops it; stop and unload
 * release the stream; the table checks owner, handle and pointers. */
static void sound_runtime_tests(void)
{
    /* 16 notes of 300 ms and 2 of 600 ms, each followed by 20 ms of silence, 16 kHz mono. */
    const uint32_t tune_bytes = (16U * 300U * 16U + 2U * 600U * 16U + 18U * 320U) * 2U;
    use_sound = true;
    mock_audio.ready = true;
    roundie_elf_set_allowed(true);
    assert(roundie_elf_prepare());
    assert(roundie_elf_execute(fixture, fixture_size));
    assert(status.state == ROUNDIE_ELF_RUNNING && round_ui);
    uint32_t generation = status.instance;
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *title = child(root, 0);
    lv_obj_t *play = child(root, 1);
    assert(strcmp(lv_label_get_text(title), "Dźwięk") == 0 && strcmp(button_text(play), "Graj") == 0);
    assert(!lv_obj_has_state(play, LV_STATE_DISABLED) && mock_audio.handle == 0);

    /* Play to the end: the press fills the ring, each tick refills what was played. */
    uint32_t releases = mock_audio.releases;
    click(play);
    drain();
    assert(mock_audio.handle != 0 && mock_audio.owner == generation && mock_audio.rate == 16000 &&
           mock_audio.channels == 1 && mock_audio.buffered == ROUNDIE_AUDIO_RING_BYTES);
    assert(strcmp(button_text(play), "Stop") == 0 && status.timer_handle != 0);
    for (unsigned tick = 0; tick < 40 && mock_audio.handle != 0; tick++) {
        mock_audio.buffered = 0;
        step_after(50000);
    }
    assert(mock_audio.handle == 0 && mock_audio.drained == 1 && mock_audio.total == tune_bytes);
    assert(mock_audio.peak > 15000 && mock_audio.peak <= 16000);
    assert(status.timer_handle == 0 && strcmp(button_text(play), "Graj") == 0);

    /* The button stops at once (no drain). */
    click(play);
    drain();
    assert(mock_audio.handle != 0);
    click(play);
    drain();
    assert(mock_audio.handle == 0 && mock_audio.drained == 1 && mock_audio.closes == 2);
    assert(status.timer_handle == 0 && strcmp(button_text(play), "Graj") == 0);

    /* Table checks, as from inside a native callback, with a stream open. */
    click(play);
    drain();
    uint32_t stream = mock_audio.handle;
    static int16_t app_samples[4];
    int16_t stack_samples[4] = {0};
    callback_active = true;
    const rep_audio_v1_t *sound = rep_audio_v1(REP_AUDIO_V1);
    assert(sound != NULL && rep_audio_v1(2) == NULL && sound->buffer_bytes == ROUNDIE_AUDIO_RING_BYTES);
    assert(sound->open(generation, 16000, 1, 100) == 0);              /* one stream per app */
    assert(sound->writable(generation + 1, stream) == -1);             /* not the owner */
    assert(sound->writable(generation, stream + 1) == -1);             /* stale handle */
    assert(sound->write(generation, stream, stack_samples, sizeof(stack_samples)) == -1); /* stack */
    assert(sound->write(generation, stream, NULL, 2) == -1);
    assert(sound->write(generation, stream, app_samples, 0) == 0);
    assert(sound->close(generation + 1, stream, 0) == -1);
    callback_active = false;
    assert(rep_audio_v1(REP_AUDIO_V1) == NULL); /* outside a callback */

    /* Unload while playing: the system releases the stream after stop. */
    control_step(ROUNDIE_ELF_UNLOAD);
    assert(status.state == ROUNDIE_ELF_UNLOADED && ui_status().outstanding == 0);
    assert(mock_audio.handle == 0 && mock_audio.releases == releases + 1);
    assert(lv_screen_active() == home && live_bytes == 0);

    /* No working audio: the table is NULL and the button disabled. */
    mock_audio.ready = false;
    assert(roundie_elf_prepare());
    assert(roundie_elf_execute(fixture, fixture_size));
    root = lv_screen_active();
    title = child(root, 0);
    play = child(root, 1);
    assert(strcmp(lv_label_get_text(title), "Brak dźwięku") == 0 && lv_obj_has_state(play, LV_STATE_DISABLED));
    control_step(ROUNDIE_ELF_UNLOAD);
    assert(status.state == ROUNDIE_ELF_UNLOADED && live_bytes == 0);
    mock_audio.ready = true;
    use_sound = false;
    puts("Sound example + rep_audio_v1: feed to drained end, stop, ownership and pointer checks, release at unload, no audio PASS (host C callbacks, mocked audio)");
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
    sound_runtime_tests();
    lv_deinit();
    return 0;
}
