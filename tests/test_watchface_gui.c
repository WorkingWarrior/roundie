/* Watch face store and GUI together on pinned LVGL: the three demo faces,
 * switching, gallery, park/resume, a corrupt preferred face and a 100-switch
 * memory check. The worker and GUI task are the same thread here; the
 * mailbox between them is the production one.
 *
 * argv: work directory, then the classic_analog, minimal_digital and
 * information packages built by tools/roundie_face.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "roundie_watchface.h"

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            abort(); \
        } \
    } while (0)

static int64_t clock_us = 1000000;
static int allocated;
static lv_indev_t *pointer;
static lv_point_t touch_point;
static lv_indev_state_t touch_state;

/* Platform port (components/roundie_platform/idf): one thread, test time. */
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return (TaskHandle_t)1;
}

int64_t esp_timer_get_time(void)
{
    return clock_us;
}

static bool test_allowed(void)
{
    return true;
}

static bool test_capacity(size_t *free_bytes)
{
    *free_bytes = 4U * 1024U * 1024U;
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
    return clock_us;
}

static const roundie_watchface_store_platform_t store_platform = {
    test_allowed, test_capacity, test_allocate, test_free, test_clock,
};

/* 2026-11-20 10:08:30 local, a Friday. */
static bool gui_time(roundie_elf_clock_t *local, int64_t *utc_seconds)
{
    local->monotonic_us = clock_us;
    local->days = 20777;
    local->ms_of_day = (10U * 3600U + 8U * 60U + 30U) * 1000U;
    local->revision = 1;
    *utc_seconds = 20777LL * 86400 + 9 * 3600 + 8 * 60 + 30;
    return true;
}

static void gui_zone(char out[48])
{
    snprintf(out, 48, "%s", "CET-1CEST,M3.5.0,M10.5.0/3");
}

static bool gui_battery(uint8_t *percent, bool *charging)
{
    *percent = 80;
    *charging = false;
    return true;
}

static const roundie_watchface_gui_platform_t gui_platform = {gui_time, gui_zone, gui_battery};

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static void touch_read(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    data->point = touch_point;
    data->state = touch_state;
}

/* One GUI task iteration: input, worker poll, face pump, render. */
static void step(bool home)
{
    clock_us += 20000;
    lv_tick_inc(20);
    lv_indev_read(pointer);
    roundie_watchface_poll(clock_us);
    roundie_watchface_gui_pump(home);
    roundie_watchface_poll(clock_us); /* takes the GUI report */
    lv_refr_now(NULL);
}

static void touch(int32_t x, int32_t y, bool pressed)
{
    touch_point.x = x;
    touch_point.y = y;
    touch_state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    step(true);
}

static void tap(int32_t x, int32_t y)
{
    touch(x, y, true);
    touch(x, y, false);
    step(true);
}

static roundie_watchface_status_t status(void)
{
    roundie_watchface_status_t out;
    roundie_watchface_status(&out);
    return out;
}

static void select_face(const char *id)
{
    CHECK(roundie_watchface_select(id) == ESP_OK);
    for (int i = 0; i < 4; i++) {
        step(true);
    }
    roundie_watchface_status_t now = status();
    CHECK(strcmp(now.active, id) == 0);
    CHECK(strcmp(now.preferred, strcmp(id, ROUNDIE_WATCHFACE_SYSTEM) == 0 ? "" : id) == 0);
}

static size_t lvgl_used(void)
{
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    return memory.total_size - memory.free_size;
}

static void corrupt(const char *path, long offset)
{
    FILE *file = fopen(path, "r+b");
    CHECK(file != NULL);
    CHECK(fseek(file, offset, SEEK_SET) == 0);
    int byte = fgetc(file);
    CHECK(byte != EOF);
    CHECK(fseek(file, offset, SEEK_SET) == 0);
    CHECK(fputc(byte ^ 0x01, file) != EOF);
    CHECK(fclose(file) == 0);
}

int main(int argc, char **argv)
{
    CHECK(argc == 5);
    CHECK(chdir(argv[1]) == 0);
    CHECK(mkdir("storage", 0755) == 0);
    lv_init();
    lv_display_t *display = lv_display_create(360, 360);
    static uint8_t buffer[360 * 10 * 2];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, touch_read);

    /* Boot: no preference, the system face is Home. */
    roundie_watchface_init(&store_platform, false);
    roundie_watchface_gui_bind(&gui_platform);
    step(true);
    step(true);
    roundie_watchface_status_t now = status();
    CHECK(now.home && !now.parked && strcmp(now.active, ROUNDIE_WATCHFACE_SYSTEM) == 0);
    CHECK(now.objects > 0 && now.failures == 0);

    /* Install is separate from activation: nothing changes on screen. */
    const char *ids[] = {"org.roundie.face.analog", "org.roundie.face.minimal", "org.roundie.face.info"};
    for (int i = 0; i < 3; i++) {
        CHECK(roundie_watchface_install_file(argv[2 + i]) == ESP_OK);
    }
    step(true);
    now = status();
    CHECK(now.count == 3 && strcmp(now.active, ROUNDIE_WATCHFACE_SYSTEM) == 0);

    /* Each demo face activates, renders and is persisted after success. */
    for (int i = 0; i < 3; i++) {
        select_face(ids[i]);
        now = status();
        CHECK(now.objects > 1 && now.failures == 0 && now.refused == 0);
        printf("face %s: objects=%u pool_free=%u bytes=%u activate_us=%u\n", ids[i], (unsigned)now.objects,
               (unsigned)now.pool_free, (unsigned)now.face_bytes, (unsigned)now.activate_us);
    }

    /* Leaving Home hides (parks) the face; returning resumes it. */
    step(false);
    now = status();
    CHECK(!now.home && now.parked);
    step(true);
    now = status();
    CHECK(now.home && !now.parked && strcmp(now.active, ids[2]) == 0);

    /* Light sleep: park before the GUI parks, resume after the restore. */
    uint32_t resumes = now.resumes;
    roundie_watchface_gui_park();
    step(true);
    CHECK(status().parked);
    roundie_watchface_gui_resume();
    step(true);
    now = status();
    CHECK(!now.parked && now.resumes == resumes + 1 && strcmp(now.active, ids[2]) == 0);

    /* Gallery: a long press opens it over the live face; the right arrow
     * previews the next face (wrapping to the system face), Cancel restores
     * and persists nothing, Set persists the previewed face. */
    touch(180, 180, true);
    for (int i = 0; i < 40; i++) {
        touch(180, 180, true);
    }
    touch(180, 180, false);
    step(true);
    CHECK(status().gallery);
    /* Gallery order: the system face, then installed faces by id. */
    tap(310, 180); /* info -> minimal */
    step(true);
    now = status();
    CHECK(strcmp(now.active, ids[1]) == 0 && strcmp(now.preferred, ids[2]) == 0);
    tap(310, 180); /* minimal -> system (wraps) */
    step(true);
    now = status();
    CHECK(strcmp(now.active, ROUNDIE_WATCHFACE_SYSTEM) == 0 && strcmp(now.preferred, ids[2]) == 0);
    tap(122, 258); /* Cancel */
    step(true);
    step(true);
    now = status();
    CHECK(!now.gallery && strcmp(now.active, ids[2]) == 0 && strcmp(now.preferred, ids[2]) == 0);
    touch(180, 180, true);
    for (int i = 0; i < 40; i++) {
        touch(180, 180, true);
    }
    touch(180, 180, false);
    step(true);
    CHECK(status().gallery);
    tap(50, 180); /* info -> analog */
    step(true);
    CHECK(strcmp(status().active, ids[0]) == 0);
    tap(237, 258); /* Set */
    step(true);
    step(true);
    now = status();
    CHECK(!now.gallery && strcmp(now.active, ids[0]) == 0 && strcmp(now.preferred, ids[0]) == 0);

    /* 100 switches: LVGL memory and store buffers return to the same level. */
    size_t baseline = lvgl_used();
    int buffers = allocated;
    for (int i = 0; i < 100; i++) {
        select_face(ids[(i + 1) % 2]);
    }
    now = status();
    CHECK(strcmp(now.active, ids[0]) == 0 && now.failures == 0);
    CHECK(allocated == buffers);
    printf("100 switches: lvgl used %zu -> %zu, activate_max_us=%u\n", baseline, lvgl_used(),
           (unsigned)now.activate_max_us);
    CHECK(lvgl_used() <= baseline + 256U);

    /* A package corrupted on storage is never shown: selecting it fails at
     * load (the shown face stays), and a same-version reinstall repairs it.
     * The boot-time fallback is covered by test_watchface.c and the desktop
     * restart test, where the GUI starts fresh. */
    select_face(ids[1]);
    corrupt("storage/faces/org.roundie.face.analog.rwf", 300);
    CHECK(roundie_watchface_select(ids[0]) != ESP_OK);
    for (int i = 0; i < 4; i++) {
        step(true);
    }
    now = status();
    CHECK(strcmp(now.active, ids[1]) == 0 && strcmp(now.preferred, ids[1]) == 0);
    CHECK(strcmp(now.error, "package-digest") == 0);
    CHECK(roundie_watchface_install_file(argv[2]) == ESP_OK);
    select_face(ids[0]);

    /* Removing the active face returns to the system face. */
    CHECK(roundie_watchface_remove(ids[0]) == ESP_OK);
    for (int i = 0; i < 4; i++) {
        step(true);
    }
    now = status();
    CHECK(strcmp(now.active, ROUNDIE_WATCHFACE_SYSTEM) == 0 && now.count == 2);

    /* Abandon (Safe Mode handoff): the face is torn down, nothing is drawn. */
    roundie_watchface_gui_abandon();
    step(true);
    puts("watchface gui: three faces, gallery, park/resume, corrupt refusal and repair, 100 switches passed");
    return 0;
}
