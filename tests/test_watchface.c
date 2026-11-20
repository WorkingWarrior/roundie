/* Watch face platform host tests (docs/watch-face-platform.md): template and
 * calendar text, change detection, the WatchFaceHost lifecycle with mock
 * faces, native admission, and the production store over a real directory
 * (transfer, validation, install, preference, settings, removal, restart).
 * LVGL is not linked: the declarative backend is tested in tests/app_ui. */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "private.h"
#include "roundie_face_host.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            abort(); \
        } \
    } while (0)

int64_t roundie_time_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

/* --- Text ------------------------------------------------------------------ */

static rwf_data_v1_t at(uint32_t days, uint32_t hours, uint32_t minutes, uint32_t seconds)
{
    rwf_data_v1_t data = {.size = sizeof(data), .flags = RWF_DATA_TIME_VALID | RWF_DATA_VISIBLE};
    data.local_days = days;
    data.ms_of_day = ((hours * 60U + minutes) * 60U + seconds) * 1000U;
    data.utc_seconds = (int64_t)days * 86400 + data.ms_of_day / 1000U;
    snprintf(data.zone, sizeof(data.zone), "UTC0");
    return data;
}

static void expect_text(const char *template, const rwf_data_v1_t *data, const char *expected)
{
    char out[ROUNDIE_FACE_TEXT_MAX];
    if (!roundie_face_template_valid(template)) {
        fprintf(stderr, "template %s rejected\n", template);
        abort();
    }
    roundie_face_expand(template, data, out, sizeof(out));
    if (strcmp(out, expected) != 0) {
        fprintf(stderr, "template %s: got '%s', expected '%s'\n", template, out, expected);
        abort();
    }
}

static void test_text(void)
{
    /* 2026-11-20 is day 20777, a Friday. */
    roundie_face_date_t date = roundie_face_civil(20777);
    CHECK(date.year == 2026 && date.month == 11 && date.day == 20 && date.weekday == 4);
    date = roundie_face_civil(0);
    CHECK(date.year == 1970 && date.month == 1 && date.day == 1 && date.weekday == 3);
    date = roundie_face_civil(11016); /* 2000-02-29, leap day */
    CHECK(date.year == 2000 && date.month == 2 && date.day == 29);
    rwf_data_v1_t data = at(20777, 9, 5, 7);
    expect_text("{HH}:{MM}:{SS}", &data, "09:05:07");
    expect_text("{H}.{MM} {hh} {h}", &data, "9.05 09 9");
    expect_text("{WEEKDAY}, {D} {MONTH} {YYYY}", &data, "piątek, 20 listopada 2026");
    expect_text("{WD} {DD}.{MO} {MON}", &data, "pt 20.11 lis");
    expect_text("{{literal {TZ}", &data, "{literal UTC0");
    data = at(20777, 0, 0, 0);
    expect_text("{hh}:{MM} {h}", &data, "12:00 12");
    /* Local midnight in a +01:00 zone is 23:00 UTC the previous day. */
    data.utc_offset_s = 3600;
    data.utc_seconds = (int64_t)20777 * 86400 - 3600;
    expect_text("{UTC} {OFF}", &data, "23:00 +01:00");
    data.utc_offset_s = -(5 * 3600 + 30 * 60);
    expect_text("{OFF}", &data, "-05:30");
    data.flags = 0;
    expect_text("{HH}:{MM} {UTC} {D} {BAT}% {CHG}", &data, "--:-- --:-- -- --% ");
    data.flags = RWF_DATA_BATTERY_VALID | RWF_DATA_CHARGING;
    data.battery_percent = 42;
    expect_text("{BAT}% {CHG}", &data, "42% ładowanie");
    CHECK(!roundie_face_template_valid("{XX}"));
    CHECK(!roundie_face_template_valid("{HH"));
    CHECK(!roundie_face_template_valid("a}b"));
    CHECK(roundie_face_template_uses("x {SS}", ROUNDIE_FACE_USES_SECOND));
    CHECK(!roundie_face_template_uses("{{SS}}", ROUNDIE_FACE_USES_SECOND));
    CHECK(roundie_face_template_uses("{CHG}", ROUNDIE_FACE_USES_BATTERY));
    /* UTF-8 is cut at a character boundary. */
    char small[6];
    data = at(20777, 0, 0, 0);
    roundie_face_expand("ąąą", &data, small, sizeof(small));
    CHECK(strcmp(small, "ąą") == 0);
    /* Boundaries and hands. */
    CHECK(roundie_face_next_boundary_ms(59999, 0) == 1);
    CHECK(roundie_face_next_boundary_ms(60000, 0) == 60000);
    CHECK(roundie_face_next_boundary_ms(1500, RWF_NEED_SECOND) == 500);
    CHECK(roundie_face_hand_angle(3U * 3600000U, ROUNDIE_FACE_HAND_HOUR) == 900);
    CHECK(roundie_face_hand_angle(15U * 3600000U + 30U * 60000U, ROUNDIE_FACE_HAND_HOUR) == 1050);
    CHECK(roundie_face_hand_angle(45U * 60000U, ROUNDIE_FACE_HAND_MINUTE) == 2700);
    CHECK(roundie_face_hand_angle(30U * 1000U + 999U, ROUNDIE_FACE_HAND_SECOND) == 1800);
}

static unsigned changes(const rwf_data_v1_t *before, const rwf_data_v1_t *after, uint32_t needs, uint32_t *kinds)
{
    return roundie_face_changes(before, after, needs, kinds, 8);
}

static void test_changes(void)
{
    uint32_t kinds[8];
    rwf_data_v1_t a = at(100, 23, 59, 59);
    CHECK(changes(NULL, &a, 0, kinds) == 1 && kinds[0] == RWF_EVENT_ACTIVATED);
    rwf_data_v1_t b = a;
    CHECK(changes(&a, &b, RWF_NEED_SECOND | RWF_NEED_BATTERY, kinds) == 0);
    /* Midnight: one DATE and one MINUTE, never replayed ticks. */
    b = at(101, 0, 0, 0);
    CHECK(changes(&a, &b, 0, kinds) == 2 && kinds[0] == RWF_EVENT_DATE && kinds[1] == RWF_EVENT_MINUTE);
    CHECK(changes(&a, &b, RWF_NEED_SECOND, kinds) == 3 && kinds[2] == RWF_EVENT_SECOND);
    /* A long sleep (3 hours) is still one MINUTE event. */
    b = at(100, 2, 0, 0);
    a = at(100, 23, 0, 0);
    b = at(101, 2, 0, 0);
    CHECK(changes(&a, &b, 0, kinds) == 2);
    /* Seconds only for faces that need them. */
    a = at(100, 10, 0, 1);
    b = at(100, 10, 0, 2);
    CHECK(changes(&a, &b, 0, kinds) == 0);
    CHECK(changes(&a, &b, RWF_NEED_SECOND, kinds) == 1 && kinds[0] == RWF_EVENT_SECOND);
    /* Zone (DST) change: ZONE, and a MINUTE when the local hour moved. */
    b = a;
    b.time_revision++;
    b.utc_offset_s = 7200;
    b.ms_of_day += 3600000U;
    CHECK(changes(&a, &b, 0, kinds) == 2 && kinds[0] == RWF_EVENT_ZONE && kinds[1] == RWF_EVENT_MINUTE);
    /* Time becoming untrusted: ZONE (validity), no MINUTE from stale fields. */
    b = a;
    b.flags &= ~RWF_DATA_TIME_VALID;
    CHECK(changes(&a, &b, 0, kinds) == 1 && kinds[0] == RWF_EVENT_ZONE);
    /* Battery only when needed. */
    b = a;
    b.flags |= RWF_DATA_BATTERY_VALID;
    b.battery_percent = 50;
    CHECK(changes(&a, &b, 0, kinds) == 0);
    CHECK(changes(&a, &b, RWF_NEED_BATTERY, kinds) == 1 && kinds[0] == RWF_EVENT_BATTERY);
    b = a;
    b.flags &= ~RWF_DATA_VISIBLE;
    CHECK(changes(&a, &b, 0, kinds) == 1 && kinds[0] == RWF_EVENT_VISIBLE);
}

/* --- Host lifecycle -------------------------------------------------------- */

typedef struct {
    const char *name;
    int state; /* 0 empty, 1 created, 2 active */
    int32_t fail_create;
    int32_t fail_activate;
    int32_t fail_update; /* on the nth update (1-based); 0 never */
    unsigned updates;
    uint32_t last_kind;
    rwf_data_v1_t last;
    unsigned creates, activations, deactivations, destroys;
    const rwf_host_v1_t *host;
    bool request_needs;
} mock_t;

static int active_mocks;
static int live_mocks;

static int32_t mock_create(void *instance, const rwf_host_v1_t *host)
{
    mock_t *mock = instance;
    CHECK(mock->state == 0);
    mock->creates++;
    if (mock->fail_create != 0) {
        return mock->fail_create;
    }
    mock->host = host;
    mock->state = 1;
    live_mocks++;
    if (mock->request_needs) {
        host->set_needs(host->context, RWF_NEED_SECOND);
    }
    return RWF_OK;
}

static int32_t mock_activate(void *instance, rwf_surface_t *surface)
{
    mock_t *mock = instance;
    CHECK(mock->state == 1 && surface != NULL);
    mock->activations++;
    if (mock->fail_activate != 0) {
        return mock->fail_activate;
    }
    mock->state = 2;
    active_mocks++;
    CHECK(active_mocks == 1); /* never two active faces */
    return RWF_OK;
}

static int32_t mock_update(void *instance, const rwf_event_v1_t *event, const rwf_data_v1_t *data)
{
    mock_t *mock = instance;
    CHECK(mock->state == 2);
    mock->updates++;
    mock->last_kind = event->kind;
    mock->last = *data;
    if (mock->fail_update != 0 && mock->updates == (unsigned)mock->fail_update) {
        return RWF_ERR_INVALID;
    }
    return RWF_OK;
}

static void mock_deactivate(void *instance)
{
    mock_t *mock = instance;
    CHECK(mock->state >= 1);
    mock->deactivations++;
    if (mock->state == 2) {
        active_mocks--;
    }
    mock->state = 1;
}

static void mock_destroy(void *instance)
{
    mock_t *mock = instance;
    CHECK(mock->state == 1);
    mock->destroys++;
    mock->state = 0;
    live_mocks--;
}

static rwf_face_v1_t mock_vtable(uint32_t needs)
{
    return (rwf_face_v1_t){.size = sizeof(rwf_face_v1_t), .abi_major = RWF_ABI_MAJOR, .abi_minor = RWF_ABI_MINOR,
                           .needs = needs, .create = mock_create, .activate = mock_activate,
                           .update = mock_update, .deactivate = mock_deactivate, .destroy = mock_destroy};
}

static bool host_data(void *context, rwf_data_v1_t *out)
{
    (void)context;
    *out = at(1, 0, 0, 0);
    return true;
}

static void test_host(void)
{
    static roundie_face_host_t host;
    mock_t fallback = {.name = "fallback"};
    rwf_face_v1_t fallback_face = mock_vtable(RWF_NEED_BATTERY);
    rwf_host_v1_t services = {.size = sizeof(services), .caps = RWF_HOST_CAP_SETTINGS, .data = host_data};
    roundie_face_host_init(&host, &services, &fallback_face, &fallback);
    rwf_surface_t *surface = (rwf_surface_t *)&host; /* any non-NULL handle */
    rwf_data_v1_t data = at(10, 12, 0, 0);

    /* Fallback first. */
    CHECK(roundie_face_host_fallback(&host, surface, &data) == RWF_OK);
    CHECK(fallback.state == 2 && fallback.last_kind == RWF_EVENT_ACTIVATED);

    /* Negotiation: wrong major, newer minor, missing capability, short vtable. */
    mock_t a = {.name = "a"};
    rwf_face_v1_t face = mock_vtable(0);
    face.abi_major = 2;
    CHECK(roundie_face_host_switch(&host, &face, &a, "org.a.face", surface, &data) == RWF_ERR_UNSUPPORTED);
    face = mock_vtable(0);
    face.abi_minor = RWF_ABI_MINOR + 1;
    CHECK(roundie_face_host_switch(&host, &face, &a, "org.a.face", surface, &data) == RWF_ERR_UNSUPPORTED);
    face = mock_vtable(0);
    face.requires = RWF_HOST_CAP_RESOURCES;
    CHECK(roundie_face_host_switch(&host, &face, &a, "org.a.face", surface, &data) == RWF_ERR_UNSUPPORTED);
    face = mock_vtable(0);
    face.size = 8;
    CHECK(roundie_face_host_switch(&host, &face, &a, "org.a.face", surface, &data) == RWF_ERR_INVALID);
    face = mock_vtable(0);
    face.update = NULL;
    CHECK(roundie_face_host_switch(&host, &face, &a, "org.a.face", surface, &data) == RWF_ERR_INVALID);
    CHECK(a.creates == 0 && host.refused == 5 && fallback.state == 2);

    /* Native admission: never runs a face; BootGuard must be ARMED. */
    face = mock_vtable(0);
    CHECK(roundie_face_host_admit_native(&host, &face, false) == RWF_ERR_STATE);
    CHECK(roundie_face_host_admit_native(&host, &face, true) == RWF_ERR_UNSUPPORTED);
    face.abi_major = 9;
    CHECK(roundie_face_host_admit_native(&host, &face, true) == RWF_ERR_UNSUPPORTED);
    CHECK(a.creates == 0);

    /* A good switch: the fallback is destroyed only after the new face works. */
    face = mock_vtable(0);
    a.request_needs = true;
    CHECK(roundie_face_host_switch(&host, &face, &a, "org.a.face", surface, &data) == RWF_OK);
    CHECK(a.state == 2 && fallback.state == 0 && fallback.destroys == 1);
    CHECK(roundie_face_host_needs(&host) == RWF_NEED_SECOND); /* set_needs during create */

    /* Create failure: the current face stays active. */
    mock_t b = {.name = "b", .fail_create = RWF_ERR_NO_MEMORY};
    rwf_face_v1_t face_b = mock_vtable(0);
    CHECK(roundie_face_host_switch(&host, &face_b, &b, "org.b.face", surface, &data) == RWF_ERR_NO_MEMORY);
    CHECK(a.state == 2 && b.state == 0 && strcmp(host.failed_id, "org.b.face") == 0);

    /* Activate failure: the old face is activated again. */
    b = (mock_t){.name = "b", .fail_activate = RWF_ERR_NO_MEMORY};
    CHECK(roundie_face_host_switch(&host, &face_b, &b, "org.b.face", surface, &data) == RWF_ERR_NO_MEMORY);
    CHECK(a.state == 2 && b.state == 0 && b.destroys == 1 && b.deactivations == 1);

    /* First-update failure: same. */
    b = (mock_t){.name = "b", .fail_update = 1};
    CHECK(roundie_face_host_switch(&host, &face_b, &b, "org.b.face", surface, &data) == RWF_ERR_INVALID);
    CHECK(a.state == 2 && b.state == 0);

    /* Samples deliver changes only. */
    unsigned before = a.updates;
    CHECK(roundie_face_host_sample(&host, &data) == RWF_OK && a.updates == before);
    rwf_data_v1_t later = at(10, 12, 0, 1);
    CHECK(roundie_face_host_sample(&host, &later) == RWF_OK && a.updates == before + 1);
    CHECK(a.last_kind == RWF_EVENT_SECOND);

    /* Park: no calls while parked; resume brings the current sample. */
    roundie_face_host_park(&host);
    CHECK(a.state == 1 && active_mocks == 0);
    before = a.updates;
    CHECK(roundie_face_host_sample(&host, &later) == RWF_ERR_STATE);
    rwf_event_v1_t tap = {.size = sizeof(tap), .kind = RWF_EVENT_TAP};
    CHECK(roundie_face_host_event(&host, &tap, &later) == RWF_ERR_STATE);
    CHECK(roundie_face_host_switch(&host, &face_b, &b, "org.b.face", surface, &data) == RWF_ERR_STATE);
    CHECK(a.updates == before && b.creates == 1);
    rwf_data_v1_t woke = at(10, 15, 30, 0);
    CHECK(roundie_face_host_resume(&host, surface, &woke) == RWF_OK);
    CHECK(a.state == 2 && a.last_kind == RWF_EVENT_RESUMED && a.last.ms_of_day == woke.ms_of_day);
    CHECK(a.updates == before + 1); /* one update, not three hours of ticks */
    CHECK(roundie_face_host_sample(&host, &woke) == RWF_OK && a.updates == before + 1);

    /* An update failure replaces the face with the fallback, never retried. */
    a.fail_update = (int32_t)a.updates + 1;
    rwf_data_v1_t next = at(10, 15, 30, 1);
    CHECK(roundie_face_host_sample(&host, &next) == RWF_ERR_INVALID);
    CHECK(a.state == 0 && fallback.state == 2 && host.current.fallback);
    CHECK(strcmp(host.failed_id, "org.a.face") == 0);
    unsigned a_updates = a.updates;
    CHECK(roundie_face_host_sample(&host, &next) == RWF_OK && a.updates == a_updates);

    /* Resume failure of a user face: fallback. */
    mock_t c = {.name = "c"};
    rwf_face_v1_t face_c = mock_vtable(0);
    CHECK(roundie_face_host_switch(&host, &face_c, &c, "org.c.face", surface, &data) == RWF_OK);
    roundie_face_host_hide(&host);
    c.fail_update = (int32_t)c.updates + 1;
    CHECK(roundie_face_host_resume(&host, surface, &data) == RWF_ERR_INVALID);
    CHECK(c.state == 0 && fallback.state == 2);

    /* 1000 switches between two faces: no leaks, at most one active. */
    mock_t d = {.name = "d"};
    mock_t e = {.name = "e"};
    rwf_face_v1_t face_d = mock_vtable(0); /* the host keeps the vtable pointer */
    rwf_face_v1_t face_e = mock_vtable(0);
    for (unsigned i = 0; i < 1000; i++) {
        mock_t *target = i % 2 == 0 ? &d : &e;
        CHECK(roundie_face_host_switch(&host, i % 2 == 0 ? &face_d : &face_e, target, i % 2 == 0 ? "org.d.face" : "org.e.face", surface,
                                       &data) == RWF_OK);
        CHECK(active_mocks == 1 && live_mocks == 1);
    }
    CHECK(d.creates == 500 && d.destroys == 500 && e.creates == 500 && e.destroys == 499);
    roundie_face_host_clear(&host);
    CHECK(active_mocks == 0 && live_mocks == 0 && !roundie_face_host_active(&host));
}

/* --- Store ----------------------------------------------------------------- */

static bool allowed = true;
static size_t capacity = 64U * 1024U * 1024U;
static size_t allocated;

static bool test_allowed(void)
{
    return allowed;
}

static bool test_capacity(size_t *free_bytes)
{
    *free_bytes = capacity;
    return true;
}

static void *test_allocate(size_t size)
{
    allocated++;
    return malloc(size);
}

static void test_free(void *memory)
{
    if (memory != NULL) {
        allocated--;
    }
    free(memory);
}

static int64_t test_clock(void)
{
    return roundie_time_us();
}

static const roundie_watchface_store_platform_t store_platform = {
    test_allowed, test_capacity, test_allocate, test_free, test_clock,
};

/* The store binds the declarative backend; without LVGL a marker is enough. */
void roundie_face_declarative_bind(roundie_face_loaded_t *loaded)
{
    loaded->vtable = (rwf_face_v1_t){.size = sizeof(rwf_face_v1_t), .abi_major = RWF_ABI_MAJOR,
                                     .needs = loaded->definition.needs};
}

static roundie_watchface_status_t store_status(void)
{
    roundie_watchface_status_t status;
    roundie_watchface_status(&status);
    return status;
}

static void expect_error(esp_err_t result, const char *reason)
{
    roundie_watchface_status_t status = store_status();
    if (result == ESP_OK || strcmp(status.error, reason) != 0) {
        fprintf(stderr, "expected refusal %s, got %d '%s'\n", reason, (int)result, status.error);
        abort();
    }
}

/* Takes the offer; returns its id ("system" for the fallback) and frees it. */
static bool take(char id[ROUNDIE_FACE_ID], uint32_t *purpose, uint32_t *serial, uint8_t *settings)
{
    roundie_face_loaded_t *face = NULL;
    if (!roundie_face_mailbox_take(&face, purpose, serial)) {
        return false;
    }
    snprintf(id, ROUNDIE_FACE_ID, "%s", face != NULL ? face->id : ROUNDIE_WATCHFACE_SYSTEM);
    if (face != NULL && settings != NULL) {
        memcpy(settings, face->settings, ROUNDIE_FACE_SETTINGS_MAX);
    }
    roundie_face_loaded_free(face);
    return true;
}

static void gui_reports_active(const char *id)
{
    roundie_watchface_status_t gui = {0};
    snprintf(gui.active, sizeof(gui.active), "%s", id);
    roundie_face_mailbox_gui_status(&gui);
}

static bool exists(const char *directory, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    struct stat info;
    return stat(path, &info) == 0;
}

static void test_store(int count, char **cases, const char *directory)
{
    /* argv: good packages first (analog, minimal, info, analog v2), then
     * "file=expected-error" pairs. */
    const char *analog = cases[0];
    const char *minimal = cases[1];
    const char *info = cases[2];
    const char *analog_v2 = cases[3];
    roundie_watchface_init(&store_platform, false);
    CHECK(store_status().count == 0 && store_status().preferred[0] == '\0');
    char id[ROUNDIE_FACE_ID];
    uint32_t purpose = 0;
    uint32_t serial = 0;
    CHECK(!take(id, &purpose, &serial, NULL));

    /* Installation never activates. */
    CHECK(roundie_watchface_install_file(analog) == ESP_OK);
    CHECK(roundie_watchface_install_file(minimal) == ESP_OK);
    CHECK(store_status().count == 2 && store_status().entries[0].healthy);
    CHECK(!take(id, &purpose, &serial, NULL));
    /* Same version again: refused, the installed file stays. */
    expect_error(roundie_watchface_install_file(analog), "replace-requires-newer-version");
    CHECK(roundie_watchface_install_file(analog_v2) == ESP_OK);
    CHECK(strcmp(store_status().entries[0].id, "org.roundie.face.analog") == 0);
    CHECK(store_status().entries[0].version[1] == 1);

    /* Bad packages: each refused with its reason; nothing is installed. */
    for (int i = 4; i < count; i++) {
        char file[512];
        snprintf(file, sizeof(file), "%s", cases[i]);
        char *expected = strchr(file, '=');
        CHECK(expected != NULL);
        *expected++ = '\0';
        expect_error(roundie_watchface_install_file(file), expected);
        CHECK(store_status().count == 2 && store_status().phase == ROUNDIE_WATCHFACE_IDLE);
        CHECK(!exists(directory, ".transfer"));
    }

    /* Transfer protocol: offsets, digest, timeout. */
    uint8_t digest[32] = {0};
    CHECK(roundie_watchface_begin(0, 1000, digest) != ESP_OK);
    CHECK(roundie_watchface_begin(7, ROUNDIE_FACE_PACKAGE_MAX + 1, digest) != ESP_OK);
    CHECK(roundie_watchface_begin(7, 1000, digest) == ESP_OK);
    uint8_t chunk[600] = {0};
    CHECK(roundie_watchface_write(7, 0, chunk, 600) != ESP_OK);
    CHECK(roundie_watchface_write(7, 10, chunk, 10) != ESP_OK);
    CHECK(roundie_watchface_write(8, 0, chunk, 10) != ESP_OK);
    CHECK(roundie_watchface_write(7, 0, chunk, 10) == ESP_OK);
    CHECK(roundie_watchface_finish(7) != ESP_OK); /* incomplete */
    CHECK(roundie_watchface_begin(9, 1000, digest) != ESP_OK); /* one transfer at a time */
    roundie_watchface_poll(roundie_time_us() + 11000000); /* expired */
    CHECK(store_status().phase == ROUNDIE_WATCHFACE_IDLE && !exists(directory, ".transfer"));
    CHECK(roundie_watchface_install(digest) != ESP_OK);
    /* An interrupted transfer (power loss) leaves .transfer: ignored by the
     * scan, retired by the next BEGIN. */
    char stale_path[512];
    snprintf(stale_path, sizeof(stale_path), "%s/.transfer", directory);
    FILE *stale = fopen(stale_path, "wb");
    CHECK(stale != NULL);
    fputs("partial", stale);
    fclose(stale);
    CHECK(roundie_watchface_install_file(info) == ESP_OK);
    CHECK(store_status().count == 3 && !exists(directory, ".transfer"));

    /* Selection: offered, persisted only after the GUI reports success. */
    CHECK(roundie_watchface_select("org.roundie.face.minimal") == ESP_OK);
    uint8_t settings[ROUNDIE_FACE_SETTINGS_MAX] = {0};
    CHECK(take(id, &purpose, &serial, settings));
    CHECK(strcmp(id, "org.roundie.face.minimal") == 0 && purpose == ROUNDIE_FACE_PURPOSE_SELECT);
    CHECK(settings[0] == 1); /* the definition's default */
    roundie_watchface_poll(roundie_time_us());
    CHECK(store_status().preferred[0] == '\0' && !exists(directory, "active"));
    roundie_face_mailbox_report(serial, purpose, false, id); /* activation failed */
    roundie_watchface_poll(roundie_time_us());
    CHECK(store_status().preferred[0] == '\0' && !exists(directory, "active"));
    CHECK(roundie_watchface_select("org.roundie.face.minimal") == ESP_OK);
    CHECK(take(id, &purpose, &serial, NULL));
    roundie_face_mailbox_report(serial, purpose, true, id);
    roundie_watchface_poll(roundie_time_us());
    CHECK(strcmp(store_status().preferred, "org.roundie.face.minimal") == 0 && exists(directory, "active"));
    gui_reports_active("org.roundie.face.minimal");

    /* Settings are stored per face, atomically, and read back on load. */
    uint8_t values[1] = {0};
    roundie_face_mailbox_settings("org.roundie.face.minimal", values, 1);
    roundie_watchface_poll(roundie_time_us());
    CHECK(exists(directory, "org.roundie.face.minimal.cfg"));

    /* Unknown, malformed and path-like ids. */
    expect_error(roundie_watchface_select("org.missing.face"), "face-open");
    expect_error(roundie_watchface_select("../active"), "face-id");
    expect_error(roundie_watchface_select("Org.Bad"), "face-id");
    expect_error(roundie_watchface_remove("../../etc"), "face-id");

    /* Restart: the preferred face is offered at boot with its settings. */
    CHECK(!take(id, &purpose, &serial, NULL));
    roundie_watchface_init(&store_platform, false);
    CHECK(take(id, &purpose, &serial, settings));
    CHECK(strcmp(id, "org.roundie.face.minimal") == 0 && purpose == ROUNDIE_FACE_PURPOSE_BOOT && settings[0] == 0);
    gui_reports_active("org.roundie.face.minimal");

    /* Fallback-only start (Safe Mode, failed boot): nothing offered or admitted. */
    roundie_watchface_init(&store_platform, true);
    CHECK(!take(id, &purpose, &serial, NULL) && store_status().fallback_only);
    expect_error(roundie_watchface_install_file(analog), "face-admission");
    expect_error(roundie_watchface_select("org.roundie.face.minimal"), "face-admission");
    /* Storage unavailable: the same. */
    allowed = false;
    roundie_watchface_init(&store_platform, false);
    CHECK(!take(id, &purpose, &serial, NULL));
    expect_error(roundie_watchface_select("org.roundie.face.minimal"), "face-admission");
    /* Admission arrives later (BootGuard STABLE): the worker poll starts the store. */
    allowed = true;
    roundie_watchface_poll(roundie_time_us());
    CHECK(take(id, &purpose, &serial, NULL));
    CHECK(strcmp(id, "org.roundie.face.minimal") == 0 && purpose == ROUNDIE_FACE_PURPOSE_BOOT);

    /* A corrupt active package: fallback stays, the preference is kept. */
    char path[512];
    snprintf(path, sizeof(path), "%s/org.roundie.face.minimal.rwf", directory);
    int file = open(path, O_RDWR);
    CHECK(file >= 0);
    uint8_t byte = 0;
    CHECK(pread(file, &byte, 1, 300) == 1);
    byte ^= 0x5a;
    CHECK(pwrite(file, &byte, 1, 300) == 1);
    close(file);
    roundie_watchface_init(&store_platform, false);
    CHECK(!take(id, &purpose, &serial, NULL));
    CHECK(strcmp(store_status().error, "package-digest") == 0);
    CHECK(strcmp(store_status().preferred, "org.roundie.face.minimal") == 0 && exists(directory, "active"));
    CHECK(store_status().load_failures == 1);
    /* Repair by reinstalling the same version over a corrupt file is allowed. */
    CHECK(roundie_watchface_install_file(minimal) == ESP_OK);

    /* A corrupt preference file selects nothing and is left in place. */
    snprintf(path, sizeof(path), "%s/active", directory);
    file = open(path, O_RDWR);
    CHECK(file >= 0 && pwrite(file, "X", 1, 10) == 1);
    close(file);
    roundie_watchface_init(&store_platform, false);
    CHECK(!take(id, &purpose, &serial, NULL) && store_status().preferred[0] == '\0' && exists(directory, "active"));

    /* Select the system face: clears the preference after the GUI reports. */
    CHECK(roundie_watchface_select("org.roundie.face.info") == ESP_OK);
    CHECK(take(id, &purpose, &serial, NULL));
    roundie_face_mailbox_report(serial, purpose, true, id);
    roundie_watchface_poll(roundie_time_us());
    CHECK(strcmp(store_status().preferred, "org.roundie.face.info") == 0);
    gui_reports_active("org.roundie.face.info");

    /* Removing the active face: preference gone, fallback offered. */
    CHECK(roundie_watchface_remove("org.roundie.face.info") == ESP_OK);
    CHECK(take(id, &purpose, &serial, NULL));
    CHECK(strcmp(id, ROUNDIE_WATCHFACE_SYSTEM) == 0 && purpose == ROUNDIE_FACE_PURPOSE_REMOVED);
    CHECK(store_status().preferred[0] == '\0' && !exists(directory, "active") && store_status().count == 2);
    gui_reports_active(ROUNDIE_WATCHFACE_SYSTEM);
    CHECK(roundie_watchface_remove("org.roundie.face.info") != ESP_OK);

    /* System selection persists as "no preference". */
    CHECK(roundie_watchface_select("org.roundie.face.analog") == ESP_OK);
    CHECK(take(id, &purpose, &serial, NULL));
    roundie_face_mailbox_report(serial, purpose, true, id);
    roundie_watchface_poll(roundie_time_us());
    CHECK(exists(directory, "active"));
    CHECK(roundie_watchface_select(ROUNDIE_WATCHFACE_SYSTEM) == ESP_OK);
    CHECK(take(id, &purpose, &serial, NULL) && strcmp(id, ROUNDIE_WATCHFACE_SYSTEM) == 0);
    roundie_face_mailbox_report(serial, purpose, true, id);
    roundie_watchface_poll(roundie_time_us());
    CHECK(!exists(directory, "active") && store_status().preferred[0] == '\0');

    /* Gallery requests: preview offers are never persisted; "Ustaw" is. */
    roundie_face_mailbox_preview("org.roundie.face.minimal");
    roundie_watchface_poll(roundie_time_us());
    CHECK(take(id, &purpose, &serial, NULL) && purpose == ROUNDIE_FACE_PURPOSE_PREVIEW);
    roundie_face_mailbox_report(serial, purpose, true, id);
    roundie_watchface_poll(roundie_time_us());
    CHECK(!exists(directory, "active"));
    roundie_face_mailbox_persist("org.roundie.face.minimal");
    roundie_watchface_poll(roundie_time_us());
    CHECK(strcmp(store_status().preferred, "org.roundie.face.minimal") == 0);
    /* An untaken offer is replaced, not leaked. */
    CHECK(roundie_watchface_select("org.roundie.face.analog") == ESP_OK);
    CHECK(roundie_watchface_select("org.roundie.face.minimal") == ESP_OK);
    CHECK(take(id, &purpose, &serial, NULL) && strcmp(id, "org.roundie.face.minimal") == 0);

    /* Count limit (8) and storage reserve. */
    capacity = 1024;
    expect_error(roundie_watchface_install_file(info), "face-storage-reserve");
    capacity = 64U * 1024U * 1024U;
    CHECK(allocated == 0);
}

int main(int argc, char **argv)
{
    test_text();
    test_changes();
    test_host();
    if (argc >= 6) {
        CHECK(chdir(argv[1]) == 0);
        test_store(argc - 2, argv + 2, FACES_DIRECTORY);
    }
    puts("watchface host tests passed");
    return 0;
}
