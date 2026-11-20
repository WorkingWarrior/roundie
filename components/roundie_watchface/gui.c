/* GUI half of WatchFaceHost (docs/watch-face-platform.md): the face screen,
 * the gallery and the host lifecycle. GUI task only; storage is the
 * worker's (store.c). A loaded face is freed only when neither the host, the
 * gallery (the face to restore) nor the pending slot refers to it. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "private.h"
#include "roundie_face_host.h"
#include "roundie_fonts.h"
#include "roundie_platform.h"

#define CHECK_PERIOD_US 1000000   /* validity, zone and battery checks */
#define BATTERY_PERIOD_US 5000000
#define GALLERY_BUTTON_COLOR 0x2a3340
#define GALLERY_ACCENT 0x2f6fd0

static const roundie_watchface_gui_platform_t *platform;
static roundie_thread_t gui_task = ROUNDIE_THREAD_NONE;
static bool abandoned;
static bool creation_failed;
static lv_obj_t *screen;
static lv_obj_t *surface; /* faces build under it; the gallery lies above it */
static roundie_face_host_t host;
static roundie_face_loaded_t *current;  /* host's current face; NULL for the fallback */
static roundie_face_loaded_t *original; /* gallery: the face to restore */
static bool original_system;
static roundie_face_loaded_t *pending; /* taken while Home was not shown */
static uint32_t pending_purpose;
static uint32_t pending_serial;
static bool pending_due;
static bool home_shown;
static bool parked; /* light sleep */
static bool resume_due;
static bool visible = true;
static int64_t next_check;
static int64_t battery_at = INT64_MIN;
static bool battery_valid;
static uint8_t battery_percent;
static bool battery_charging;
static roundie_watchface_status_t report;

/* Gallery. */
static lv_obj_t *gallery;
static lv_obj_t *gallery_name;
static lv_obj_t *gallery_count;
static lv_obj_t *gallery_note;
static roundie_watchface_status_t listing; /* static: too large for the GUI stack */
static unsigned gallery_index;
static unsigned gallery_total; /* the system face plus installed faces */

static void sample(rwf_data_v1_t *out, bool battery_now)
{
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    roundie_elf_clock_t local;
    int64_t utc = 0;
    if (platform->time(&local, &utc)) {
        out->flags |= RWF_DATA_TIME_VALID;
        out->utc_seconds = utc;
        out->local_days = local.days;
        out->ms_of_day = local.ms_of_day;
        out->time_revision = local.revision;
        out->utc_offset_s = (int32_t)((int64_t)local.days * 86400 + local.ms_of_day / 1000U - utc);
    }
    platform->zone(out->zone);
    out->zone[sizeof(out->zone) - 1] = '\0';
    int64_t now = roundie_time_us();
    if (battery_now || now - battery_at >= BATTERY_PERIOD_US) {
        battery_at = now;
        uint8_t percent = 0;
        bool charging = false;
        battery_valid = platform->battery(&percent, &charging);
        battery_percent = battery_valid ? percent : 0;
        battery_charging = battery_valid && charging;
    }
    if (battery_valid) {
        out->flags |= RWF_DATA_BATTERY_VALID;
        out->battery_percent = battery_percent;
        if (battery_charging) {
            out->flags |= RWF_DATA_CHARGING;
        }
    }
    if (visible) {
        out->flags |= RWF_DATA_VISIBLE;
    }
}

/* --- Host services: resources and settings of the face being called. ---- */

static bool service_data(void *context, rwf_data_v1_t *out)
{
    (void)context;
    sample(out, false);
    return true;
}

static bool service_resource(void *context, uint32_t index, rwf_resource_v1_t *out)
{
    (void)context;
    if (current == NULL || index >= current->package.resource_count) {
        return false;
    }
    const roundie_face_resource_t *resource = &current->package.resources[index];
    *out = (rwf_resource_v1_t){
        .size = sizeof(*out),
        .format = resource->format,
        .width = resource->width,
        .height = resource->height,
        .length = resource->length,
        .data = current->bytes + resource->offset,
    };
    return true;
}

static int32_t service_setting_get(void *context, uint32_t index)
{
    (void)context;
    if (current == NULL || index >= current->definition.setting_count) {
        return RWF_ERR_INVALID;
    }
    return current->settings[index];
}

static int32_t service_setting_set(void *context, uint32_t index, uint32_t value)
{
    (void)context;
    if (current == NULL || index >= current->definition.setting_count ||
        value >= current->definition.settings[index].options) {
        return RWF_ERR_INVALID;
    }
    current->settings[index] = (uint8_t)value;
    roundie_face_mailbox_settings(current->id, current->settings, current->definition.setting_count);
    return RWF_OK;
}

/* --- Ownership ------------------------------------------------------------ */

static void release(roundie_face_loaded_t *face)
{
    if (face != NULL && face != current && face != original && face != pending) {
        roundie_face_loaded_free(face);
    }
}

/* After any host call: a face the host dropped (failure) is no longer current. */
static void settle(void)
{
    if (current != NULL && (host.current.state == ROUNDIE_FACE_EMPTY || host.current.instance != &current->instance)) {
        roundie_face_loaded_t *dropped = current;
        current = NULL;
        release(dropped);
    }
}

static uint32_t elapsed(int64_t since)
{
    int64_t now = roundie_time_us();
    return now > since ? (uint32_t)(now - since) : 0U;
}

static uint32_t count_objects(lv_obj_t *object)
{
    uint32_t count = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        count += count_objects(lv_obj_get_child(object, (int32_t)i));
    }
    return count;
}

static void measure(void)
{
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    report.pool_free = (uint32_t)memory.free_size;
    report.objects = screen != NULL ? count_objects(screen) : 0U;
    uint32_t bytes = 0;
    roundie_face_loaded_t *held[3] = {current, original, pending};
    for (unsigned i = 0; i < 3; i++) {
        bool repeated = false;
        for (unsigned j = 0; j < i; j++) {
            repeated = repeated || held[j] == held[i];
        }
        if (held[i] != NULL && !repeated) {
            bytes += (uint32_t)held[i]->size;
        }
    }
    report.face_bytes = bytes;
}

/* Switches the host to face (NULL: the fallback). True when it is shown. */
static bool show_face(roundie_face_loaded_t *face)
{
    rwf_data_v1_t data;
    sample(&data, true);
    int64_t started = roundie_time_us();
    int32_t result;
    roundie_face_loaded_t *previous = current;
    if (face == NULL) {
        result = roundie_face_host_fallback(&host, (rwf_surface_t *)surface, &data);
        current = NULL;
    } else {
        current = face; /* services refer to the face being created */
        result = roundie_face_host_switch(&host, &face->vtable, &face->instance, face->id,
                                          (rwf_surface_t *)surface, &data);
        if (result != RWF_OK) {
            bool previous_back = previous != NULL && host.current.state != ROUNDIE_FACE_EMPTY &&
                host.current.instance == &previous->instance;
            current = previous_back ? previous : NULL;
            release(face);
        }
    }
    release(previous);
    settle();
    report.activate_us = elapsed(started);
    if (report.activate_us > report.activate_max_us) {
        report.activate_max_us = report.activate_us;
    }
    next_check = 0;
    measure();
    return result == RWF_OK;
}

static const char *current_id(void)
{
    return current != NULL ? current->id : ROUNDIE_WATCHFACE_SYSTEM;
}

/* --- Gallery ---------------------------------------------------------------- */

static const char *gallery_id(unsigned index)
{
    return index == 0 ? ROUNDIE_WATCHFACE_SYSTEM : listing.entries[index - 1].id;
}

static void gallery_paint(const char *note)
{
    if (gallery_index == 0) {
        lv_label_set_text(gallery_name, "Tarcza systemowa");
    } else {
        const roundie_watchface_entry_t *entry = &listing.entries[gallery_index - 1];
        lv_label_set_text(gallery_name, entry->name[0] != '\0' ? entry->name : entry->id);
    }
    lv_label_set_text_fmt(gallery_count, "%u / %u", gallery_index + 1, gallery_total);
    lv_label_set_text(gallery_note, note);
}

static void gallery_close(bool restore)
{
    if (gallery == NULL) {
        return;
    }
    lv_obj_delete(gallery);
    gallery = NULL;
    report.gallery = false;
    roundie_face_loaded_t *kept = original;
    bool kept_system = original_system;
    original = NULL;
    original_system = false;
    if (restore && !host.parked) {
        if (kept_system) {
            if (current != NULL) {
                show_face(NULL);
            }
        } else if (kept != NULL && kept != current) {
            show_face(kept);
        }
    }
    release(kept);
}

static void gallery_preview(void)
{
    const char *id = gallery_id(gallery_index);
    if (strcmp(id, current_id()) == 0) {
        gallery_paint("");
        return;
    }
    if (gallery_index == 0) {
        show_face(NULL);
        gallery_paint("");
        return;
    }
    if (!listing.entries[gallery_index - 1].healthy) {
        gallery_paint("Uszkodzona");
        return;
    }
    roundie_face_mailbox_preview(id);
    gallery_paint("Wczytywanie...");
}

static void gallery_clicked(lv_event_t *event)
{
    uintptr_t action = (uintptr_t)lv_event_get_user_data(event);
    if (gallery == NULL || gallery_total == 0) {
        return;
    }
    if (action == 0 || action == 1) {
        gallery_index = action == 0 ? (gallery_index + gallery_total - 1U) % gallery_total
                                    : (gallery_index + 1U) % gallery_total;
        gallery_preview();
    } else if (action == 2) {
        gallery_close(true);
    } else {
        const char *id = gallery_id(gallery_index);
        if (strcmp(id, current_id()) != 0) {
            gallery_paint("Niedostępna");
            return; /* a face that did not activate is never kept */
        }
        roundie_face_mailbox_persist(id);
        gallery_close(false);
    }
}

static lv_obj_t *gallery_button(lv_obj_t *parent, const char *text, int32_t x, int32_t y, int32_t w, int32_t h,
                                uintptr_t action, uint32_t color)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, h);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_radius(button, h / 2, 0);
    lv_obj_add_event_cb(button, gallery_clicked, LV_EVENT_CLICKED, (void *)action);
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, &roundie_font_20, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *gallery_label(lv_obj_t *parent, const lv_font_t *font, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, 220);
    lv_obj_set_pos(label, 70, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_bg_color(label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_70, 0);
    lv_obj_set_style_radius(label, 8, 0);
    return label;
}

static void gallery_open(void)
{
    if (gallery != NULL || host.parked) {
        return;
    }
    roundie_watchface_status(&listing);
    gallery_total = 1U + listing.count;
    gallery_index = 0;
    for (unsigned i = 0; i < listing.count; i++) {
        if (current != NULL && strcmp(listing.entries[i].id, current->id) == 0) {
            gallery_index = i + 1U;
        }
    }
    /* Transparent overlay: the face under it is the live preview. */
    gallery = lv_obj_create(screen);
    if (gallery == NULL) {
        return;
    }
    lv_obj_remove_style_all(gallery);
    lv_obj_set_size(gallery, 360, 360);
    lv_obj_remove_flag(gallery, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(gallery, LV_OBJ_FLAG_CLICKABLE); /* the face gets no taps while browsing */
    gallery_name = gallery_label(gallery, &roundie_font_20, 70);
    gallery_count = gallery_label(gallery, &roundie_font_14, 100);
    gallery_note = gallery_label(gallery, &roundie_font_14, 206);
    gallery_button(gallery, LV_SYMBOL_LEFT, 20, 155, 60, 50, 0, GALLERY_BUTTON_COLOR);
    gallery_button(gallery, LV_SYMBOL_RIGHT, 280, 155, 60, 50, 1, GALLERY_BUTTON_COLOR);
    gallery_button(gallery, "Anuluj", 70, 236, 105, 44, 2, GALLERY_BUTTON_COLOR);
    gallery_button(gallery, "Ustaw", 185, 236, 105, 44, 3, GALLERY_ACCENT);
    original = current;
    original_system = current == NULL;
    report.gallery = true;
    gallery_paint("");
}

/* --- Screen -------------------------------------------------------------- */

static void screen_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (abandoned || !home_shown || host.parked || gallery != NULL) {
        return;
    }
    if (code == LV_EVENT_LONG_PRESSED) {
        lv_indev_t *input = lv_indev_active();
        if (input != NULL) {
            lv_indev_wait_release(input); /* no click after the long press */
        }
        gallery_open();
        return;
    }
    if (code == LV_EVENT_SHORT_CLICKED && (roundie_face_host_needs(&host) & RWF_NEED_TAP) != 0) {
        lv_indev_t *input = lv_indev_active();
        lv_point_t point = {0};
        if (input != NULL) {
            lv_indev_get_point(input, &point);
        }
        rwf_data_v1_t data;
        sample(&data, false);
        rwf_event_v1_t tap = {.size = sizeof(tap), .kind = RWF_EVENT_TAP, .x = (int16_t)point.x,
                              .y = (int16_t)point.y};
        roundie_face_host_event(&host, &tap, &data);
        settle();
        roundie_face_host_sample(&host, &data); /* a refresh the tap requested */
        settle();
    }
}

static bool create_screen(void)
{
    screen = lv_obj_create(NULL);
    if (screen == NULL) {
        return false;
    }
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    surface = lv_obj_create(screen);
    if (surface == NULL) {
        lv_obj_delete(screen);
        screen = NULL;
        return false;
    }
    lv_obj_remove_style_all(surface);
    lv_obj_remove_flag(surface, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(surface, 360, 360);
    lv_obj_add_event_cb(screen, screen_event, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(screen, screen_event, LV_EVENT_SHORT_CLICKED, NULL);
    return true;
}

void roundie_watchface_gui_bind(const roundie_watchface_gui_platform_t *gui_platform)
{
    platform = gui_platform;
    gui_task = roundie_thread_self();
    rwf_host_v1_t services = {
        .size = sizeof(rwf_host_v1_t),
        .abi_major = RWF_ABI_MAJOR,
        .abi_minor = RWF_ABI_MINOR,
        .caps = ROUNDIE_FACE_HOST_CAPS,
        .context = NULL,
        .data = service_data,
        .resource = service_resource,
        .setting_get = service_setting_get,
        .setting_set = service_setting_set,
    };
    roundie_face_host_init(&host, &services, &roundie_face_fallback, roundie_face_fallback_instance);
    snprintf(report.active, sizeof(report.active), "%s", ROUNDIE_WATCHFACE_SYSTEM);
    report.visible = true;
}

static void sync_report(void)
{
    snprintf(report.active, sizeof(report.active), "%s", current_id());
    memcpy(report.failed, host.failed_id, sizeof(report.failed));
    report.host_error = host.last_error;
    report.home = home_shown;
    report.parked = host.parked;
    report.visible = visible;
    report.creates = host.creates;
    report.destroys = host.destroys;
    report.activations = host.activations;
    report.deactivations = host.deactivations;
    report.updates = host.updates;
    report.switches = host.switches;
    report.failures = host.failures;
    report.refused = host.refused;
    roundie_face_mailbox_gui_status(&report);
}

/* An offer from the store: shown now when Home is up, else kept until it is. */
static void apply_offer(roundie_face_loaded_t *face, uint32_t purpose, uint32_t serial)
{
    if (purpose == ROUNDIE_FACE_PURPOSE_PREVIEW && gallery == NULL) {
        roundie_face_mailbox_report(serial, purpose, false, face != NULL ? face->id : ROUNDIE_WATCHFACE_SYSTEM);
        release(face);
        return; /* the gallery was closed meanwhile */
    }
    if (purpose != ROUNDIE_FACE_PURPOSE_PREVIEW) {
        gallery_close(false);
    }
    if (!home_shown || host.parked) {
        if (pending_due) {
            /* A newer offer supersedes the one still waiting. */
            roundie_face_mailbox_report(pending_serial, pending_purpose, false,
                                        pending != NULL ? pending->id : ROUNDIE_WATCHFACE_SYSTEM);
            roundie_face_loaded_t *replaced = pending;
            pending = NULL;
            pending_due = false;
            release(replaced);
        }
        pending = face;
        pending_purpose = purpose;
        pending_serial = serial;
        pending_due = true;
        return;
    }
    char id[ROUNDIE_FACE_ID];
    snprintf(id, sizeof(id), "%s", face != NULL ? face->id : ROUNDIE_WATCHFACE_SYSTEM);
    bool shown = show_face(face);
    roundie_face_mailbox_report(serial, purpose, shown, id);
    if (gallery != NULL) {
        gallery_paint(shown ? "" : "Niedostępna");
    }
}

static void deliver_due(void)
{
    int64_t now = roundie_time_us();
    if (!visible || now < next_check || !roundie_face_host_active(&host)) {
        return;
    }
    rwf_data_v1_t data;
    sample(&data, false);
    int64_t started = roundie_time_us();
    roundie_face_host_sample(&host, &data);
    settle();
    uint32_t took = elapsed(started);
    if (took > report.update_max_us) {
        report.update_max_us = took;
    }
    int64_t wait = CHECK_PERIOD_US;
    if ((data.flags & RWF_DATA_TIME_VALID) != 0) {
        int64_t boundary = (int64_t)roundie_face_next_boundary_ms(data.ms_of_day, roundie_face_host_needs(&host)) *
            1000;
        if (boundary < wait) {
            wait = boundary;
        }
    }
    next_check = now + wait;
}

void roundie_watchface_gui_pump(bool home)
{
    if (platform == NULL || abandoned || roundie_thread_self() != gui_task) {
        return;
    }
    if (screen == NULL) {
        if (creation_failed || !create_screen()) {
            creation_failed = true;
            return; /* the launcher list stays usable */
        }
    }
    roundie_face_loaded_t *face = NULL;
    uint32_t purpose = 0;
    uint32_t serial = 0;
    if (roundie_face_mailbox_take(&face, &purpose, &serial)) {
        apply_offer(face, purpose, serial);
    }
    if (resume_due) {
        resume_due = false;
        parked = false;
        if (home_shown) {
            rwf_data_v1_t data;
            sample(&data, true);
            int64_t started = roundie_time_us();
            roundie_face_host_resume(&host, (rwf_surface_t *)surface, &data);
            settle();
            report.resume_us = elapsed(started);
            report.resumes++;
            next_check = 0;
            measure();
        }
    }
    if (home && !home_shown) {
        home_shown = true;
        lv_screen_load(screen);
        if (!parked) {
            rwf_data_v1_t data;
            sample(&data, true);
            roundie_face_host_resume(&host, (rwf_surface_t *)surface, &data);
            settle();
            next_check = 0;
            measure();
        }
    } else if (!home && home_shown) {
        home_shown = false;
        gallery_close(true);
        roundie_face_host_hide(&host);
        measure();
    }
    if (home_shown && !host.parked && pending_due) {
        roundie_face_loaded_t *face_due = pending;
        pending_due = false;
        pending = NULL;
        apply_offer(face_due, pending_purpose, pending_serial);
    }
    if (home_shown && lv_screen_active() != screen) {
        lv_screen_load(screen);
    }
    if (home_shown && !host.parked) {
        deliver_due();
    }
    sync_report();
}

void roundie_watchface_gui_park(void)
{
    if (platform == NULL || abandoned || roundie_thread_self() != gui_task) {
        return;
    }
    gallery_close(true);
    roundie_face_host_park(&host);
    parked = true;
    sync_report();
}

void roundie_watchface_gui_resume(void)
{
    if (platform == NULL || abandoned || roundie_thread_self() != gui_task) {
        return;
    }
    if (parked) {
        resume_due = true;
    }
}

void roundie_watchface_gui_visible(bool on)
{
    if (platform == NULL || abandoned || roundie_thread_self() != gui_task) {
        return;
    }
    visible = on;
    next_check = 0; /* the VISIBLE change, and fresh time when the panel comes on */
    if (!on && home_shown && roundie_face_host_active(&host)) {
        rwf_data_v1_t data;
        sample(&data, false);
        roundie_face_host_sample(&host, &data);
        settle();
    }
}

void roundie_watchface_gui_abandon(void)
{
    /* Safe Mode may delete the screen tree: drop every reference without
     * touching LVGL; the faces' memory stays allocated until restart. */
    abandoned = true;
    screen = NULL;
    surface = NULL;
    gallery = NULL;
    report.home = false;
    report.gallery = false;
    roundie_face_mailbox_gui_status(&report);
}
