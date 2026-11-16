/* Roundie on the Linux desktop (docs/roundie-core.md).
 *
 * Runs the Roundie Core launcher and application UI bridge with LVGL on an
 * emulated panel. One GUI thread (this main thread) owns LVGL, SDL and
 * Vulkan, as the GUI task does on the board; one system worker thread polls
 * the launcher and the package manager, as the firmware's system worker. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <SDL3/SDL.h>
#include "psa/crypto.h"
#include "lvgl.h"
#include "display_scale.h"
#include "roundie_app_ui.h"
#include "roundie_audio.h"
#include "roundie_elf.h"
#include "roundie_launcher.h"
#include "roundie_packages.h"
#include "roundie_platform.h"
#include "board.h"
#include "desktop_input.h"
#include "power_manager.h"
#include "power_shared.h"
#include "roundie_power.h"
#include "vk_present.h"
#include "esp_log.h"
#include "lab.h"
#include "panel.h"
#include "vhw.h"
#include "app_runtime.h"
#include "clock.h"
#include "local_time.h"
#include "roundie_clock_port.h"
#include "roundie_wall_clock.h"
#include "time_protocol.h"

#define PACKAGES_PARENT "storage"
#define GUI_DELAY_MAX_MS 33U
#define SCRIPT_LINES 512U
#define SCRIPT_LINE 256U
#define INSTALLS_MAX 8U
#define LAB_STATE_FILE "lab-state"
/* main/main.c's touch retry after a failed read. */
#define TOUCH_RETRY_US 5000000

/* Profiles of the supported boards; the values mirror each board's
 * include/roundie_board_config.h and the 1.85B's default
 * CONFIG_ROUNDIE_LCD_STRIP_LINES. */
typedef struct {
    const char *name;
    uint32_t width;
    uint32_t height;
    uint32_t align;
    uint32_t strip_lines;
    bool round;
    bool rtc;   /* ROUNDIE_BOARD_CAP_RTC: PCF85063 */
    bool gauge; /* BQ27220 fuel gauge; otherwise the AXP2101 PMIC */
} profile_t;

static const profile_t profiles[] = {
    {"1.85b", 360, 360, 1, 20, true, true, true},
    {"1.75c", 466, 466, 2, 24, true, false, false},
};

typedef struct {
    const profile_t *profile;
    bool scaled;
    roundie_scale_geometry_t geometry;
    uint16_t *columns;
    roundie_scale_carry_t carry;
    uint16_t *carry_pixels;
    uint16_t *scale_output;
    uint16_t *panel;     /* emulated panel memory, RGB565 */
    uint32_t *presented; /* B8G8R8A8 with the round glass applied */
    uint8_t *glass;      /* 1 where the round glass shows the panel */
    bool dirty;
    uint32_t flushes;
    uint32_t flush_errors;
    lv_display_t *display;
    roundie_desktop_input_t input;
    uint16_t last_x;
    uint16_t last_y;
    uint32_t touch_reads;
    roundie_board_t *board;
    bool lit; /* the panel's light as last composed */
    uint32_t backlight; /* the backlight percentage as last composed */
    int64_t touch_retry_after_us;
    bool touch_error_logged;
    uint32_t touch_noise;
    lab_panel_t *lab_panel; /* the lab's control panel; its window only with --lab-panel */
} gui_t;

static gui_t gui;
/* Backlight for the power manager; the firmware's default brightness. */
static uint8_t brightness_percent = 60;
static atomic_bool quit_requested;
static char *caller_directory; /* script paths are relative to it */
static char *installs[INSTALLS_MAX];
static uint32_t install_count;
static atomic_bool install_failed;
/* GUI thread: Roundie Core runs; false while the virtual board is off or its bring-up stopped. */
static bool core_running;
/* The command line, for a restart of the process (restart_process). */
static int saved_argc;
static char **saved_argv;

static _Noreturn void fail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fputs("roundie-desktop: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    exit(2);
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(roundie_time_us() / 1000);
}

/* ---- Display: the firmware's scaling path, writing panel memory -------- */

static void copy_rows(int32_t x1, int32_t y1, uint32_t width, uint32_t rows, const uint16_t *source)
{
    for (uint32_t row = 0; row < rows; row++) {
        uint16_t *target = gui.panel + (size_t)(y1 + (int32_t)row) * gui.profile->width + x1;
        memcpy(target, source + (size_t)row * width, width * sizeof(uint16_t));
    }
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    gui.flushes++;
    if (!gui.scaled) {
        uint32_t width = (uint32_t)lv_area_get_width(area);
        uint32_t rows = (uint32_t)lv_area_get_height(area);
        copy_rows(area->x1, area->y1, width, rows, (const uint16_t *)pixels);
    } else {
        roundie_scale_plan_t plan;
        if (!roundie_scale_plan(&gui.geometry, &gui.carry, area->x1, area->y1, area->x2, area->y2, &plan)) {
            gui.carry.active = false;
            gui.flush_errors++;
            fprintf(stderr, "roundie-desktop: scaled flush (%d,%d)-(%d,%d) breaks panel alignment\n",
                    (int)area->x1, (int)area->y1, (int)area->x2, (int)area->y2);
            lv_display_flush_ready(display);
            return;
        }
        bool send = plan.send_y2 >= plan.send_y1;
        roundie_scale_strip(&gui.geometry, &plan, gui.columns, (const uint16_t *)pixels,
                            send ? gui.scale_output : NULL, gui.carry_pixels, false);
        roundie_scale_update_carry(&plan, &gui.carry);
        if (send) {
            uint32_t width = (uint32_t)(plan.x2 - plan.x1 + 1);
            uint32_t rows = (uint32_t)(plan.send_y2 - plan.send_y1 + 1);
            copy_rows(plan.x1, plan.send_y1, width, rows, gui.scale_output);
        }
    }
    if (lv_display_flush_is_last(display)) {
        gui.dirty = true;
    }
    lv_display_flush_ready(display);
}

static void display_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_INVALIDATE_AREA && gui.scaled) {
        /* Rounder, as on the board: every area's panel rectangle is aligned. */
        lv_area_t *rounded = lv_event_get_invalidated_area(event);
        roundie_scale_round(&gui.geometry, &rounded->x1, &rounded->y1, &rounded->x2, &rounded->y2);
    }
}

static void *allocate(size_t bytes)
{
    void *memory = calloc(1, bytes);
    if (memory == NULL) {
        fail("out of memory (%zu bytes)", bytes);
    }
    return memory;
}

/* Panel memory and the round glass, before the board powers on. */
static void create_panel(void)
{
    const profile_t *profile = gui.profile;
    size_t panel_pixels = (size_t)profile->width * profile->height;
    gui.panel = allocate(panel_pixels * sizeof(uint16_t));
    gui.presented = allocate(panel_pixels * sizeof(uint32_t));
    gui.glass = allocate(panel_pixels);
    float radius = (float)profile->width / 2.0f;
    for (uint32_t y = 0; y < profile->height; y++) {
        for (uint32_t x = 0; x < profile->width; x++) {
            float dx = (float)x + 0.5f - radius;
            float dy = (float)y + 0.5f - (float)profile->height / 2.0f;
            gui.glass[(size_t)y * profile->width + x] = !profile->round || dx * dx + dy * dy <= radius * radius;
        }
    }
}

static void create_display(void)
{
    const profile_t *profile = gui.profile;
    gui.scaled = profile->width != ROUNDIE_UI_WIDTH || profile->height != ROUNDIE_UI_HEIGHT;
    gui.geometry = (roundie_scale_geometry_t){
        .logical_width = ROUNDIE_UI_WIDTH,
        .logical_height = ROUNDIE_UI_HEIGHT,
        .physical_width = profile->width,
        .physical_height = profile->height,
        .align = profile->align,
    };
    /* Strip height as the firmware chooses it for the board's DMA chunks. */
    size_t buffer_bytes = 0;
    if (gui.scaled) {
        size_t chunk_pixels = (size_t)profile->width * profile->strip_lines;
        uint32_t rows = ROUNDIE_UI_HEIGHT;
        while (rows > 0 && roundie_scale_max_output(&gui.geometry, ROUNDIE_UI_WIDTH * rows) > chunk_pixels) {
            rows--;
        }
        if (rows < 3) {
            fail("strip of %u lines too small to scale the canvas", (unsigned)profile->strip_lines);
        }
        buffer_bytes = (size_t)ROUNDIE_UI_WIDTH * rows * 2U;
        gui.columns = allocate(profile->width * sizeof(uint16_t));
        gui.carry_pixels = allocate(profile->width * sizeof(uint16_t));
        gui.scale_output = allocate(chunk_pixels * sizeof(uint16_t));
        roundie_scale_columns(&gui.geometry, gui.columns);
    } else {
        buffer_bytes = (size_t)profile->width * profile->strip_lines * 2U;
    }
    gui.display = lv_display_create(ROUNDIE_UI_WIDTH, ROUNDIE_UI_HEIGHT);
    if (gui.display == NULL) {
        fail("LVGL display allocation failed");
    }
    lv_display_set_color_format(gui.display, LV_COLOR_FORMAT_RGB565);
    lv_display_add_event_cb(gui.display, display_event, LV_EVENT_ALL, NULL);
    lv_display_set_flush_cb(gui.display, flush);
    void *buffer = allocate(buffer_bytes);
    lv_display_set_buffers(gui.display, buffer, NULL, (uint32_t)buffer_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
}

/* dim: the window shows the backlight level; screenshots keep full intensity,
 * so they compare equal at any brightness. */
static void compose(bool dim)
{
    size_t pixels = (size_t)gui.profile->width * gui.profile->height;
    gui.lit = core_running && roundie_desktop_board_lit(gui.board);
    gui.backlight = roundie_desktop_board_backlight(gui.board);
    uint32_t level = dim ? gui.backlight : 100U;
    if (!gui.lit) {
        /* Panel or backlight off: the glass is black, the panel keeps its memory. */
        for (size_t i = 0; i < pixels; i++) {
            gui.presented[i] = 0xFF000000U;
        }
        return;
    }
    for (size_t i = 0; i < pixels; i++) {
        uint16_t c = gui.panel[i];
        uint32_t r = (c >> 11) & 0x1FU;
        uint32_t g = (c >> 5) & 0x3FU;
        uint32_t b = c & 0x1FU;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        r = r * level / 100U;
        g = g * level / 100U;
        b = b * level / 100U;
        uint32_t value = 0xFF000000U | (r << 16) | (g << 8) | b;
        gui.presented[i] = gui.glass[i] ? value : 0xFF000000U;
    }
}

static bool save_png(const char *path)
{
    compose(false);
    SDL_Surface *surface = SDL_CreateSurfaceFrom((int)gui.profile->width, (int)gui.profile->height,
                                                 SDL_PIXELFORMAT_ARGB8888, gui.presented,
                                                 (int)(gui.profile->width * 4U));
    if (surface == NULL) {
        fprintf(stderr, "roundie-desktop: screenshot: %s\n", SDL_GetError());
        return false;
    }
    bool saved = SDL_SavePNG(surface, path);
    if (!saved) {
        fprintf(stderr, "roundie-desktop: screenshot %s: %s\n", path, SDL_GetError());
    }
    SDL_DestroySurface(surface);
    return saved;
}

/* ---- Touch: emulated panel coordinates to the canvas -------------------- */

/* roundie_board_touch_read() of the virtual touch controller: the emulated
 * panel, or the lab's fault (vhw.h). Spurious points may lie off the panel,
 * as a controller's corrupted report may. */
static esp_err_t touch_read(bool *pressed, uint16_t *x, uint16_t *y)
{
    vhw_state_t state;
    vhw_get(&state, roundie_time_us());
    *pressed = false;
    if (state.touch == VHW_TOUCH_ABSENT) {
        return ESP_ERR_NOT_FOUND;
    }
    if (state.touch == VHW_TOUCH_TIMEOUT) {
        return ESP_ERR_TIMEOUT;
    }
    if (state.touch == VHW_TOUCH_NOISE) {
        /* xorshift32: the same points on every run. */
        uint32_t value = gui.touch_noise == 0 ? 0x2545F491U : gui.touch_noise;
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        gui.touch_noise = value;
        *pressed = (value & 3U) != 0;
        *x = (uint16_t)((value >> 8) & 0x3FFU);
        *y = (uint16_t)((value >> 20) & 0x3FFU);
        return ESP_OK;
    }
    roundie_desktop_touch_read(&gui.input, pressed, x, y);
    return ESP_OK;
}

static void read_touch(lv_indev_t *device, lv_indev_data_t *data)
{
    (void)device;
    gui.touch_reads++;
    bool pressed = false;
    uint16_t x = gui.last_x;
    uint16_t y = gui.last_y;
    int64_t now = roundie_time_us();
    /* main/main.c's policy: after a failed read, released for 5 s. */
    if (now < gui.touch_retry_after_us) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = gui.last_x;
        data->point.y = gui.last_y;
        return;
    }
    esp_err_t error = touch_read(&pressed, &x, &y);
    if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) {
        if (!gui.touch_error_logged) {
            ESP_LOGE("roundie", "Touch read failed: %s", esp_err_to_name(error));
            gui.touch_error_logged = true;
        }
        gui.touch_retry_after_us = now + TOUCH_RETRY_US;
    } else if (error == ESP_OK) {
        gui.touch_error_logged = false;
    }
    if (pressed && gui.scaled) {
        /* The board reports panel coordinates; LVGL works on the canvas. */
        x = (uint16_t)roundie_scale_touch(x, ROUNDIE_UI_WIDTH, gui.profile->width);
        y = (uint16_t)roundie_scale_touch(y, ROUNDIE_UI_HEIGHT, gui.profile->height);
    }
    /* As on the board: touches that turn the screen on, or arrive while it
     * is off, never reach applications or the launcher. */
    if (!roundie_power_gui_touch(roundie_time_us(), pressed)) {
        pressed = false;
    }
    if (pressed) {
        gui.last_x = x;
        gui.last_y = y;
    }
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = gui.last_x;
    data->point.y = gui.last_y;
}

/* ---- System worker ------------------------------------------------------- */

/* Installs a development package through the package manager's transfer
 * API, as the firmware's USB protocol does. Worker thread. */
static bool install_package(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "roundie-desktop: install %s: %s\n", path, strerror(errno));
        return false;
    }
    uint8_t *bytes = NULL;
    long size = -1;
    if (fseek(file, 0, SEEK_END) == 0) {
        size = ftell(file);
    }
    bool loaded = size > 0 && fseek(file, 0, SEEK_SET) == 0;
    if (loaded) {
        bytes = malloc((size_t)size);
        loaded = bytes != NULL && fread(bytes, 1, (size_t)size, file) == (size_t)size;
    }
    fclose(file);
    uint8_t digest[32];
    size_t digest_length = 0;
    esp_err_t result = ESP_FAIL;
    if (loaded && psa_hash_compute(PSA_ALG_SHA_256, bytes, (size_t)size, digest, sizeof(digest),
                                   &digest_length) == PSA_SUCCESS) {
        const uint32_t session = 1;
        result = roundie_packages_begin(session, (size_t)size, digest);
        /* The transfer API takes chunks of at most 512 bytes. */
        for (size_t offset = 0; result == ESP_OK && offset < (size_t)size; offset += 512U) {
            size_t chunk = (size_t)size - offset < 512U ? (size_t)size - offset : 512U;
            result = roundie_packages_write(session, offset, bytes + offset, chunk);
        }
        if (result == ESP_OK) {
            result = roundie_packages_finish(session);
        }
        if (result == ESP_OK) {
            result = roundie_packages_install(digest);
        }
    }
    free(bytes);
    if (result != ESP_OK) {
        /* The reason, before abort records its own state. */
        roundie_packages_status_t status;
        roundie_packages_status(&status);
        fprintf(stderr, "roundie-desktop: install %s: %s (%s)\n", path, esp_err_to_name(result), status.error);
        roundie_packages_abort(1);
        return false;
    }
    return true;
}

/* ---- Time: the firmware's wall clock on emulated hardware (M3) -------------- */

/* The firmware's sensor period: RTC sample and wall-clock poll. */
#define CLOCK_POLL_US 5000000

typedef enum {
    TIME_NONE,
    TIME_RT1,
    TIME_RESET,
} time_kind_t;

/* One request from the script to the system worker, which owns the wall
 * clock as on the board. A SET with an RTC takes 1.2 s there. */
typedef struct {
    roundie_lock_t lock;
    time_kind_t kind; /* pending request; TIME_NONE when free */
    uint32_t done;    /* completed requests */
    char text[SCRIPT_LINE];
    roundie_desktop_reset_t reset;
} time_mailbox_t;

static time_mailbox_t time_mailbox = {.lock = ROUNDIE_LOCK_INITIALIZER};

/* The firmware's startup: trust the RTC, or what the reset kept. The zone
 * rule is not reloaded on an emulated reset; it equals the stored rule, since
 * roundie_local_time_set_zone() stores before it applies. */
static void time_start(void)
{
    roundie_rtc_sample_t rtc;
    roundie_board_rtc_read(gui.board, &rtc);
    roundie_wall_clock_start(&rtc);
    roundie_wall_clock_publish();
}

/* RT1 as main.c's time_command(), printing the reply instead of sending it.
 * DIAG and RESTART need the board's serial transport and are refused. */
static void time_rt1(const char *arguments)
{
    roundie_time_frame_t *frame = calloc(1, sizeof(*frame));
    if (frame == NULL) {
        fail("out of memory");
    }
    char text[SCRIPT_LINE + 16];
    snprintf(text, sizeof(text), "!RT1 0000d0e5 %s\n", arguments);
    for (const char *byte = text; *byte != '\0'; byte++) {
        if (roundie_time_feed(frame, *byte)) {
            break;
        }
    }
    roundie_time_command_t command;
    const char *result = roundie_time_parse(frame, &command);
    free(frame);
    if (strcmp(result, "OK") == 0 && (command.diagnostic || command.restart)) {
        printf("!RT1 %s UNSUPPORTED\n", command.id);
        fflush(stdout);
        return;
    }
    if (strcmp(result, "OK") == 0 && command.zone) {
        if (command.zone_rule[0] != '\0' && roundie_local_time_set_zone(command.zone_rule) != ESP_OK) {
            result = "ZONE";
        }
        char active[ROUNDIE_TIME_ZONE_CAPACITY];
        roundie_local_time_zone(active);
        printf("!RT1 %s %s zone=%s\n", command.id, result, active);
        fflush(stdout);
        return;
    }
    roundie_rtc_sample_t rtc;
    esp_err_t error = ESP_OK;
    result = roundie_wall_clock_request(gui.board, result, command.set, command.seconds, &rtc, &error);
    uint32_t sync_count = 0;
    bool wall_valid = roundie_wall_clock_valid(&sync_count);
    int64_t rtc_seconds = -1;
    if (rtc.state == ROUNDIE_SENSOR_VALID) {
        roundie_rtc_unix_seconds(&rtc.time, &rtc_seconds);
    }
    int os = (rtc.state == ROUNDIE_SENSOR_VALID || rtc.state == ROUNDIE_SENSOR_INVALID) ? rtc.oscillator_stopped : -1;
    int64_t wall = wall_valid ? roundie_clock_port_wall_us() / 1000000 : -1;
    printf("!RT1 %s %s state=%d os=%d rtc=%lld wall=%lld err=%d\n", command.id, result, (int)rtc.state, os,
           (long long)rtc_seconds, (long long)wall, (int)error);
    fflush(stdout);
}

/* ---- Battery: the firmware's sensor sample on the virtual gauge or PMIC ---- */

/* The latest sample, for quick settings on the GUI thread. */
static struct {
    roundie_lock_t lock;
    roundie_battery_sample_t battery;
} sensors = {.lock = ROUNDIE_LOCK_INITIALIZER};

static const char *sensor_state_name(roundie_sensor_state_t state)
{
    switch (state) {
    case ROUNDIE_SENSOR_VALID: return "valid";
    case ROUNDIE_SENSOR_NOT_FOUND: return "not found";
    case ROUNDIE_SENSOR_COMM_ERROR: return "I2C error";
    case ROUNDIE_SENSOR_INVALID: return "invalid data";
    case ROUNDIE_SENSOR_UNAVAILABLE: return "unavailable";
    case ROUNDIE_SENSOR_BATTERY_ABSENT: return "battery absent";
    case ROUNDIE_SENSOR_UNCALIBRATED: return "gauge unverified";
    }
    return "unknown";
}

/* main/main.c's sensor worker: one battery sample, logged on a state change or
 * a change of 50 mV or 5 %. System worker. */
static void sample_battery(void)
{
    static bool first = true;
    static roundie_sensor_state_t last_state;
    static uint16_t last_voltage;
    static uint16_t last_soc;
    roundie_battery_sample_t battery;
    roundie_board_battery_read(gui.board, &battery);
    if (first || battery.state != last_state) {
        ESP_LOGI("roundie", "Gauge %s, present=%u, initialized=%u, error=%s", sensor_state_name(battery.state),
                 battery.battery_present, battery.initialized, esp_err_to_name(battery.error));
        last_state = battery.state;
    }
    if (battery.voltage_valid &&
        (first || abs((int)battery.voltage_mv - last_voltage) >= 50 ||
         (battery.soc_valid && abs((int)battery.soc_percent - last_soc) >= 5))) {
        if (battery.soc_valid) {
            ESP_LOGI("roundie", "Battery %u mV, gauge SoC %u%% (accuracy unverified), flags=0x%04x, op=0x%04x",
                     battery.voltage_mv, battery.soc_percent, battery.battery_flags, battery.operation_flags);
        } else {
            ESP_LOGI("roundie", "Battery %u mV, gauge SoC unavailable, flags=0x%04x, op=0x%04x",
                     battery.voltage_mv, battery.battery_flags, battery.operation_flags);
        }
        last_voltage = battery.voltage_mv;
        last_soc = battery.soc_percent;
    }
    first = false;
    roundie_lock(&sensors.lock);
    sensors.battery = battery;
    roundie_unlock(&sensors.lock);
}

/* Quick settings on the home application, as main/main.c's (docs/quick-settings.md). GUI thread. */
static bool quick_battery(uint8_t *percent, bool *charging)
{
    roundie_lock(&sensors.lock);
    roundie_battery_sample_t battery = sensors.battery;
    roundie_unlock(&sensors.lock);
    if (battery.state != ROUNDIE_SENSOR_VALID || !battery.battery_present || !battery.soc_valid ||
        battery.soc_percent > 100) {
        return false;
    }
    *percent = (uint8_t)battery.soc_percent;
    *charging = battery.current_valid && battery.current_ma > 0;
    return true;
}

/* 30, 60, 100, 30 ... percent, as main/main.c's step_backlight(). */
static uint8_t quick_brightness(bool cycle)
{
    if (cycle) {
        uint8_t next_percent = 30;
        if (brightness_percent < 60) {
            next_percent = 60;
        } else if (brightness_percent < 100) {
            next_percent = 100;
        }
        esp_err_t error = roundie_board_backlight_set(gui.board, next_percent);
        if (error != ESP_OK) {
            ESP_LOGE("roundie", "Backlight change: %s", esp_err_to_name(error));
        } else {
            brightness_percent = next_percent;
        }
    }
    return brightness_percent;
}

static const roundie_launcher_quick_t quick_settings = {
    quick_battery, quick_brightness, roundie_power_gui_screen_off, roundie_power_gui_sleep,
};

static void time_worker_poll(int64_t now, int64_t *polled_us)
{
    roundie_lock(&time_mailbox.lock);
    time_kind_t kind = time_mailbox.kind;
    char text[SCRIPT_LINE];
    memcpy(text, time_mailbox.text, sizeof(text));
    roundie_desktop_reset_t reset = time_mailbox.reset;
    roundie_unlock(&time_mailbox.lock);
    if (kind == TIME_RT1) {
        time_rt1(text);
    } else if (kind == TIME_RESET) {
        roundie_desktop_clock_reset(reset);
        fprintf(stderr, "roundie-desktop: emulated %s reset of the time subsystem\n",
                roundie_desktop_reset_name(reset));
        time_start();
        *polled_us = now;
    }
    if (kind != TIME_NONE) {
        roundie_lock(&time_mailbox.lock);
        time_mailbox.kind = TIME_NONE;
        time_mailbox.done++;
        roundie_unlock(&time_mailbox.lock);
    }
    if (now - *polled_us >= CLOCK_POLL_US) {
        *polled_us = now;
        roundie_rtc_sample_t rtc;
        roundie_board_rtc_read(gui.board, &rtc);
        roundie_wall_clock_poll(&rtc);
        sample_battery();
    }
}

static void *system_worker(void *argument)
{
    (void)argument;
    roundie_desktop_time_follow();
    for (uint32_t i = 0; i < install_count; i++) {
        if (!install_package(installs[i])) {
            atomic_store(&install_failed, true);
        }
    }
    /* As the board's trial after Safe Mode: no home start into a package that may have failed. */
    bool previous_failed = roundie_desktop_apps_startup();
    roundie_launcher_set_home(ROUNDIE_LAUNCHER_HOME_PACKAGE, !previous_failed);
    int64_t clock_polled_us = roundie_time_us();
    sample_battery();
    while (!atomic_load(&quit_requested)) {
        int64_t now = roundie_time_us();
        roundie_desktop_apps_poll(now);
        roundie_packages_poll(now);
        roundie_launcher_poll(true, roundie_packages_busy());
        roundie_power_worker_poll(now);
        time_worker_poll(now, &clock_polled_us);
        roundie_sleep_ms(10);
    }
    roundie_desktop_apps_shutdown();
    return NULL;
}

/* ---- Scripted runs --------------------------------------------------------- */

static const char *const navigation_names[] = {
    "WAITING", "REFRESHING", "LIST", "REQUESTED", "LOADING", "ACTIVE", "EXITING", "ERROR", "RESULT",
};

static const char *const app_state_names[] = {
    "UNLOADED", "LOADING", "READY", "RUNNING", "STOPPING", "UNLOADING", "INITIALIZED", "STARTING", "SUSPENDING",
    "SUSPENDED", "RESUMING", "STOPPED",
};

static const char *app_state_name(roundie_elf_state_t state)
{
    if ((size_t)state >= sizeof(app_state_names) / sizeof(app_state_names[0])) {
        return "?";
    }
    return app_state_names[state];
}

static const char *navigation_name(roundie_navigation_t state)
{
    if ((size_t)state < sizeof(navigation_names) / sizeof(navigation_names[0])) {
        return navigation_names[state];
    }
    return "?";
}

typedef struct {
    char lines[SCRIPT_LINES][SCRIPT_LINE];
    uint32_t count;
    uint32_t next;
    int64_t resume_us;    /* do not run the next line before this */
    int64_t deadline_us;  /* wait-nav deadline, 0 when not waiting */
    uint32_t marked_revision;
    bool matched;           /* wait-nav matched; waiting for a touch read */
    uint32_t matched_reads;
    bool failed;
    bool time_waiting; /* rt1 or reset sent to the system worker */
    uint32_t time_done;
} script_t;

static script_t script;

#define VIRTUAL_STEP_US 5000
/* Real time after which a callback that has not returned counts as hung. */
#define CALLBACK_HUNG_REAL_NS (1000LL * 1000 * 1000)

static int64_t real_time_ns(void)
{
    struct timespec real;
    clock_gettime(CLOCK_MONOTONIC, &real);
    return (int64_t)real.tv_sec * 1000000000LL + real.tv_nsec;
}

/* --virtual-time: while the script waits, time moves in steps of at most
 * VIRTUAL_STEP_US, each taken only after the executor and the system worker
 * have handled the previous one (the worker takes turns with this loop), so
 * applications, the launcher and the clock see the same times on every run.
 * A callback still running after CALLBACK_HUNG_REAL_NS of real time is taken
 * as hung: time then moves on without the executor, as it does on the board,
 * so the stall report and the script's deadlines come at the same virtual
 * times on every run. */
static void virtual_time_step(void)
{
    static int64_t callback_since_us; /* the busy callback, by its start */
    static int64_t callback_seen_ns;  /* real time it was first seen busy */
    int64_t now = roundie_time_us();
    if (!roundie_elf_executor_settled(now)) {
        roundie_elf_executor_kick();
        int64_t since = roundie_elf_executor_busy_since();
        if (since == 0) {
            return;
        }
        if (since != callback_since_us) {
            callback_since_us = since;
            callback_seen_ns = real_time_ns();
            return;
        }
        if (real_time_ns() - callback_seen_ns < CALLBACK_HUNG_REAL_NS) {
            return;
        }
    }
    int64_t step = VIRTUAL_STEP_US;
    if (script.resume_us > now && script.resume_us - now < step) {
        step = script.resume_us - now;
    }
    roundie_desktop_time_advance(step);
    roundie_elf_executor_kick();
}

static bool script_load(const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "roundie-desktop: script %s: %s\n", path, strerror(errno));
        return false;
    }
    char line[SCRIPT_LINE];
    while (fgets(line, sizeof(line), file) != NULL) {
        char *start = line;
        while (*start == ' ' || *start == '\t') {
            start++;
        }
        start[strcspn(start, "\r\n#")] = '\0';
        if (*start == '\0') {
            continue;
        }
        if (script.count == SCRIPT_LINES) {
            fprintf(stderr, "roundie-desktop: script longer than %u lines\n", SCRIPT_LINES);
            fclose(file);
            return false;
        }
        snprintf(script.lines[script.count], SCRIPT_LINE, "%s", start);
        script.count++;
    }
    fclose(file);
    return true;
}

static void script_fail(const char *line, const char *reason)
{
    fprintf(stderr, "roundie-desktop: script line %u \"%s\": %s\n", script.next + 1U, line, reason);
    script.failed = true;
    atomic_store(&quit_requested, true);
}

/* The GUI has painted the launcher's state and applied its latest change,
 * such as a refreshed list in the same state. */
static bool gui_caught_up(const roundie_launcher_status_t *status)
{
    return status->painted_state == status->state && status->painted_request == status->request &&
           status->painted_generation == status->generation && status->gui_applied_us >= status->changed_us;
}

/* Completes a waiting line once match holds, or fails it after timeout_ms.
 * A new screen ignores a press until LVGL has read a release
 * (lv_indev_wait_release), and a hit test needs the new objects laid out:
 * on a match, refresh now, then let LVGL read the panel once. */
static bool script_settled(bool match, int timeout_ms, int64_t now, const char *line, roundie_navigation_t state)
{
    if (!match && !script.matched) {
        if (script.deadline_us == 0) {
            script.deadline_us = now + (int64_t)timeout_ms * 1000;
        }
        if (now >= script.deadline_us) {
            fprintf(stderr, "roundie-desktop: launcher is %s\n", navigation_name(state));
            script_fail(line, "timed out");
        }
        return false;
    }
    if (!script.matched) {
        lv_refr_now(gui.display);
        script.matched = true;
        script.matched_reads = gui.touch_reads;
        return false;
    }
    if (gui.touch_reads == script.matched_reads) {
        return false;
    }
    script.matched = false;
    script.deadline_us = 0;
    return true;
}

/* rt1 ARGS or reset KIND on the system worker; true once it has completed. */
static bool script_time_request(const char *command, const char *line, int64_t now)
{
    if (!script.time_waiting) {
        roundie_desktop_reset_t reset = ROUNDIE_DESKTOP_RESET_SW;
        bool is_reset = strcmp(command, "reset") == 0;
        char kind[32] = "";
        if (is_reset && (sscanf(line, "%*s %31s", kind) != 1 || !roundie_desktop_reset_parse(kind, &reset))) {
            script_fail(line, "unknown reset");
            return false;
        }
        roundie_lock(&time_mailbox.lock);
        bool free_slot = time_mailbox.kind == TIME_NONE;
        if (free_slot) {
            time_mailbox.kind = is_reset ? TIME_RESET : TIME_RT1;
            const char *arguments = line + strlen(command);
            arguments += strspn(arguments, " \t");
            snprintf(time_mailbox.text, sizeof(time_mailbox.text), "%s", arguments);
            time_mailbox.reset = reset;
            script.time_done = time_mailbox.done;
        }
        roundie_unlock(&time_mailbox.lock);
        if (free_slot) {
            script.time_waiting = true;
            script.deadline_us = now + 10000000;
        }
        return false;
    }
    roundie_lock(&time_mailbox.lock);
    bool done = time_mailbox.done != script.time_done;
    roundie_unlock(&time_mailbox.lock);
    if (!done) {
        if (now >= script.deadline_us) {
            script_fail(line, "timed out");
        }
        return false;
    }
    script.time_waiting = false;
    script.deadline_us = 0;
    return true;
}

/* Wall-clock status and local time from the applications' clock source. */
static void script_clock(void)
{
    roundie_wall_clock_status_t status;
    roundie_wall_clock_status(&status);
    roundie_elf_clock_t local = {0};
    roundie_elf_clock_source_t source = roundie_desktop_app_clock_source();
    bool local_valid = source != NULL && source(&local);
    char zone[ROUNDIE_TIME_ZONE_CAPACITY];
    roundie_local_time_zone(zone);
    printf("clock valid=%d sync=%u wall=%lld local=%d days=%u ms=%u revision=%u mono=%lld zone=%s\n",
           (int)status.valid, (unsigned)status.sync_count,
           status.valid ? (long long)(roundie_clock_port_wall_us() / 1000000) : -1LL, (int)local_valid,
           (unsigned)local.days, (unsigned)local.ms_of_day, (unsigned)local.revision, (long long)roundie_time_us(),
           zone);
    fflush(stdout);
}

/* A command of the virtual hardware lab (lab.h). The script continues on a
 * later loop iteration, after the desktop has carried out what the machine does
 * next (lab_poll), so no line runs in a process that is about to restart. */
static void script_lab(const char *line, int64_t now)
{
    uint32_t hold_ms = 0;
    char command[64] = "";
    char word[64] = "";
    int x = 0;
    int y = 0;
    sscanf(line, "%63s", word);
    if (strcmp(word, "panel-screenshot") == 0 && sscanf(line, "%*s %63s", word) == 1) {
        char path[4096];
        if (word[0] == '/' || caller_directory == NULL) {
            snprintf(path, sizeof(path), "%s", word);
        } else {
            snprintf(path, sizeof(path), "%s/%s", caller_directory, word);
        }
        if (gui.lab_panel == NULL || !lab_panel_save(gui.lab_panel, now, path)) {
            script_fail(line, "panel screenshot failed");
            return;
        }
        script.next++;
        return;
    }
    if (strcmp(word, "panel-click") == 0) {
        /* The panel's control at a frame pixel, then its lab command. */
        if (sscanf(line, "%*s %d %d", &x, &y) != 2 || gui.lab_panel == NULL ||
            !lab_panel_command_at(gui.lab_panel, x, y, now, command, sizeof(command))) {
            script_fail(line, "no panel control there");
            return;
        }
        printf("panel-click %s\n", command);
        fflush(stdout);
        line = command;
    }
    lab_result_t result = lab_command(line, &gui.input, now, &hold_ms);
    if (result == LAB_UNKNOWN) {
        script_fail(line, core_running ? "unknown or malformed command" : "unknown command, or the board is off");
        return;
    }
    if (result == LAB_MALFORMED) {
        script_fail(line, "malformed lab command");
        return;
    }
    script.next++;
    /* A key stays pressed for its hold time before the next line. */
    script.resume_us = now + (int64_t)hold_ms * 1000;
}

/* Runs script lines whose time has come. */
static void script_step(void)
{
    while (script.next < script.count && !script.failed) {
        int64_t now = roundie_time_us();
        if (now < script.resume_us) {
            return;
        }
        const char *line = script.lines[script.next];
        char command[32] = "";
        char word[64] = "";
        int a = 0;
        int b = 0;
        int c = 0;
        int d = 0;
        int e = 0;
        if (sscanf(line, "%31s", command) != 1) {
            script.next++;
            continue;
        }
        /* Off or halted: only the lab and the script's own commands. */
        if (!core_running && strcmp(command, "wait") != 0 && strcmp(command, "release") != 0 &&
            strcmp(command, "screenshot") != 0 && strcmp(command, "quit") != 0) {
            script_lab(line, now);
            return;
        }
        if (strcmp(command, "wait") == 0 && sscanf(line, "%*s %d", &a) == 1 && a >= 0) {
            script.resume_us = now + (int64_t)a * 1000;
        } else if (strcmp(command, "down") == 0 && sscanf(line, "%*s %d %d", &a, &b) == 2) {
            roundie_desktop_touch_set(&gui.input, true, a, b);
        } else if (strcmp(command, "move") == 0 && sscanf(line, "%*s %d %d", &a, &b) == 2) {
            roundie_desktop_touch_set(&gui.input, true, a, b);
        } else if (strcmp(command, "up") == 0) {
            roundie_desktop_touch_set(&gui.input, false, 0, 0);
        } else if (strcmp(command, "tap") == 0 && sscanf(line, "%*s %d %d", &a, &b) == 2) {
            /* A press held for 100 ms, then released by the next line. */
            roundie_desktop_touch_set(&gui.input, true, a, b);
            snprintf(script.lines[script.next], SCRIPT_LINE, "up");
            script.resume_us = now + 100000;
            return;
        } else if (strcmp(command, "swipe") == 0 &&
                   sscanf(line, "%*s %d %d %d %d %d", &a, &b, &c, &d, &e) == 5 && e > 0) {
            roundie_desktop_touch_swipe(&gui.input, a, b, c, d, (uint32_t)e, now);
            script.resume_us = now + (int64_t)e * 1000 + 50000;
        } else if (strcmp(command, "release") == 0 && sscanf(line, "%*s %63s", word) == 1) {
            roundie_desktop_key_name(&gui.input, word, false);
        } else if (strcmp(command, "screenshot") == 0 && sscanf(line, "%*s %63s", word) == 1) {
            char path[4096];
            if (word[0] == '/' || caller_directory == NULL) {
                snprintf(path, sizeof(path), "%s", word);
            } else {
                snprintf(path, sizeof(path), "%s/%s", caller_directory, word);
            }
            if (core_running) {
                lv_refr_now(gui.display);
            }
            if (!save_png(path)) {
                script_fail(line, "screenshot failed");
                return;
            }
        } else if (strcmp(command, "expect-nav") == 0 && sscanf(line, "%*s %63s", word) == 1) {
            roundie_launcher_status_t *status = allocate(sizeof(*status));
            roundie_launcher_status(status);
            bool match = strcmp(navigation_name(status->state), word) == 0;
            if (!match) {
                fprintf(stderr, "roundie-desktop: launcher is %s\n", navigation_name(status->state));
            }
            free(status);
            if (!match) {
                script_fail(line, "launcher state differs");
                return;
            }
        } else if (strcmp(command, "wait-nav") == 0 && sscanf(line, "%*s %63s %d", word, &a) == 2) {
            roundie_launcher_status_t *status = allocate(sizeof(*status));
            roundie_launcher_status(status);
            bool match = strcmp(navigation_name(status->state), word) == 0 && gui_caught_up(status);
            roundie_navigation_t state = status->state;
            free(status);
            if (!script_settled(match, a, now, line, state)) {
                return;
            }
        } else if (strcmp(command, "status") == 0) {
            roundie_launcher_status_t *status = allocate(sizeof(*status));
            roundie_launcher_status(status);
            printf("launcher state=%s painted=%s revision=%u request=%u rejected=%u completed=%u"
                   " launch=%d app=%d entries=%u\n",
                   navigation_name(status->state), navigation_name((roundie_navigation_t)status->painted_state),
                   (unsigned)status->revision, (unsigned)status->request, (unsigned)status->rejected,
                   (unsigned)status->completed, (int)status->launch_result, (int)status->app_result,
                   (unsigned)status->count);
            fflush(stdout);
            free(status);
        } else if (strcmp(command, "app") == 0) {
            roundie_elf_status_t *status = allocate(sizeof(*status));
            roundie_elf_status(status);
            printf("app state=%s instance=%u abi=%u completed=%u loader=%d result=%d callbacks=%u"
                   " outstanding=%u busy=%d error=\"%s\"\n",
                   app_state_name(status->state), (unsigned)status->instance, (unsigned)status->abi_version,
                   (unsigned)status->completed, status->loader_result, (int)status->app_result,
                   (unsigned)status->callbacks, (unsigned)status->resources_outstanding, (int)roundie_elf_busy(),
                   status->error);
            fflush(stdout);
            free(status);
        } else if (strcmp(command, "app-stats") == 0) {
            roundie_elf_status_t *status = allocate(sizeof(*status));
            roundie_elf_status(status);
            printf("app-stats instance=%u state=%s events=%u timers=%u users=%u ui=%u last=%u queue=%u"
                   " dropped=%u coalesced=%u rejected=%u acquired=%u released=%u reclaimed=%u alloc=%u"
                   " control=%d\n",
                   (unsigned)status->instance, app_state_name(status->state), (unsigned)status->events_delivered,
                   (unsigned)status->timers_delivered, (unsigned)status->users_delivered,
                   (unsigned)status->ui_delivered, (unsigned)status->last_value, (unsigned)status->queue_depth,
                   (unsigned)status->events_dropped, (unsigned)status->events_coalesced,
                   (unsigned)status->events_rejected, (unsigned)status->resources_acquired,
                   (unsigned)status->resources_released, (unsigned)status->resources_reclaimed,
                   (unsigned)status->alloc_bytes, (int)status->control_result);
            fflush(stdout);
            free(status);
        } else if (strcmp(command, "app-event") == 0 && sscanf(line, "%*s %d", &a) == 1 && a >= 0) {
            /* The firmware's RE1 EVENT: a user event to the running instance. */
            roundie_elf_status_t *status = allocate(sizeof(*status));
            roundie_elf_status(status);
            esp_err_t result = roundie_elf_post(status->instance, (uint32_t)a);
            free(status);
            printf("app-event result=%s\n", esp_err_to_name(result));
            fflush(stdout);
        } else if (strcmp(command, "app-control") == 0 && sscanf(line, "%*s %63s", word) == 1) {
            /* RE1 SUSPEND, RESUME, STOP and UNLOAD. */
            static const char *const controls[] = {"suspend", "resume", "stop", "unload"};
            roundie_elf_control_t control = 0;
            for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
                if (strcmp(word, controls[i]) == 0) {
                    control = (roundie_elf_control_t)(i + 1);
                }
            }
            if (control == 0) {
                script_fail(line, "unknown control");
                return;
            }
            roundie_elf_status_t *status = allocate(sizeof(*status));
            roundie_elf_status(status);
            esp_err_t result = roundie_elf_control(status->instance, control);
            free(status);
            printf("app-control %s result=%s\n", word, esp_err_to_name(result));
            fflush(stdout);
        } else if ((strcmp(command, "ui-press") == 0 && sscanf(line, "%*s %d", &a) == 1 && a > 0) ||
                   (strcmp(command, "ui-value") == 0 && sscanf(line, "%*s %d %d", &a, &b) == 2 && a > 0)) {
            /* RE1 UI PRESS and UI VALUE: injected input to a widget of the running instance. */
            roundie_elf_status_t *status = allocate(sizeof(*status));
            roundie_elf_status(status);
            bool accepted = strcmp(command, "ui-press") == 0 ?
                roundie_ui_input(status->instance, (uint32_t)a, REP3_ORIGIN_INJECTED) :
                roundie_ui_inject_value(status->instance, (uint32_t)a, b);
            free(status);
            printf("%s accepted=%d\n", command, (int)accepted);
            fflush(stdout);
        } else if (strcmp(command, "audio") == 0) {
            roundie_audio_status_t audio;
            roundie_audio_status(&audio);
            printf("audio initialized=%d playing=%d plays=%u errors=%u volume=%u stream=%d phase=%u ring=%u"
                   " underruns=%u pauses=%u busy=%d\n",
                   (int)audio.initialized, (int)audio.playing, (unsigned)audio.plays, (unsigned)audio.errors,
                   (unsigned)audio.last_volume, (int)audio.stream_open, (unsigned)audio.stream_phase,
                   (unsigned)audio.ring_used, (unsigned)audio.underruns, (unsigned)audio.pauses,
                   (int)roundie_audio_busy());
            fflush(stdout);
        } else if (strcmp(command, "resources") == 0) {
            /* Leak checks of repeated application cycles: image mappings and descriptors. */
            unsigned mappings = 0;
            unsigned descriptors = 0;
            FILE *maps = fopen("/proc/self/maps", "r");
            char text[512];
            while (maps != NULL && fgets(text, sizeof(text), maps) != NULL) {
                if (strstr(text, "memfd:rep-app") != NULL) {
                    mappings++;
                }
            }
            if (maps != NULL) {
                fclose(maps);
            }
            DIR *fds = opendir("/proc/self/fd");
            while (fds != NULL && readdir(fds) != NULL) {
                descriptors++;
            }
            if (fds != NULL) {
                closedir(fds);
            }
            printf("resources image_mappings=%u descriptors=%u\n", mappings, descriptors);
            fflush(stdout);
        } else if (strcmp(command, "wait-app") == 0 && sscanf(line, "%*s %63s %d", word, &a) == 2) {
            roundie_elf_status_t *status = allocate(sizeof(*status));
            roundie_elf_status(status);
            bool match = strcmp(app_state_name(status->state), word) == 0;
            roundie_elf_state_t state = status->state;
            free(status);
            if (!match) {
                if (script.deadline_us == 0) {
                    script.deadline_us = now + (int64_t)a * 1000;
                }
                if (now >= script.deadline_us) {
                    fprintf(stderr, "roundie-desktop: application state is %s\n", app_state_name(state));
                    script_fail(line, "timed out");
                }
                return;
            }
            script.deadline_us = 0;
        } else if (strcmp(command, "power") == 0) {
            roundie_lock(&roundie_power_shared.lock);
            roundie_power_state_t state = roundie_power_shared.state;
            uint32_t sleeps = roundie_power_shared.sleeps;
            uint32_t screen_offs = roundie_power_shared.screen_offs;
            char message[sizeof(roundie_power_shared.message)];
            memcpy(message, roundie_power_shared.message, sizeof(message));
            roundie_unlock(&roundie_power_shared.lock);
            printf("power state=%s lit=%d sleeps=%u screen_offs=%u message=\"%s\"\n", roundie_power_state_name(state),
                   (int)roundie_desktop_board_lit(gui.board), (unsigned)sleeps, (unsigned)screen_offs, message);
            fflush(stdout);
        } else if (strcmp(command, "wait-power") == 0 && sscanf(line, "%*s %63s %d", word, &a) == 2) {
            /* No LVGL refresh here: the GUI may be parked for light sleep. */
            roundie_lock(&roundie_power_shared.lock);
            roundie_power_state_t state = roundie_power_shared.state;
            roundie_unlock(&roundie_power_shared.lock);
            if (strcmp(roundie_power_state_name(state), word) != 0) {
                if (script.deadline_us == 0) {
                    script.deadline_us = now + (int64_t)a * 1000;
                }
                if (now >= script.deadline_us) {
                    fprintf(stderr, "roundie-desktop: power state is %s\n", roundie_power_state_name(state));
                    script_fail(line, "timed out");
                }
                return;
            }
            script.deadline_us = 0;
        } else if (strcmp(command, "mark") == 0) {
            roundie_launcher_status_t *status = allocate(sizeof(*status));
            roundie_launcher_status(status);
            script.marked_revision = status->revision;
            free(status);
        } else if (strcmp(command, "wait-refreshed") == 0 && sscanf(line, "%*s %d", &a) == 1) {
            roundie_launcher_status_t *status = allocate(sizeof(*status));
            roundie_launcher_status(status);
            bool match = status->revision > script.marked_revision && gui_caught_up(status);
            roundie_navigation_t state = status->state;
            free(status);
            if (!script_settled(match, a, now, line, state)) {
                return;
            }
        } else if ((strcmp(command, "rt1") == 0 && sscanf(line, "%*s %63s", word) == 1) ||
                   (strcmp(command, "reset") == 0 && sscanf(line, "%*s %63s", word) == 1)) {
            if (!script_time_request(command, line, now)) {
                return;
            }
        } else if (strcmp(command, "clock") == 0) {
            script_clock();
        } else if (strcmp(command, "wait-clock") == 0 && sscanf(line, "%*s %63s %d", word, &a) == 2 &&
                   (strcmp(word, "valid") == 0 || strcmp(word, "invalid") == 0)) {
            roundie_wall_clock_status_t clock;
            roundie_wall_clock_status(&clock);
            if (clock.valid != (strcmp(word, "valid") == 0)) {
                if (script.deadline_us == 0) {
                    script.deadline_us = now + (int64_t)a * 1000;
                }
                if (now >= script.deadline_us) {
                    script_fail(line, "timed out");
                }
                return;
            }
            script.deadline_us = 0;
        } else if (strcmp(command, "time-advance") == 0 && sscanf(line, "%*s %d", &a) == 1 && a >= 0) {
            roundie_desktop_clock_advance((int64_t)a * 1000);
        } else if (strcmp(command, "wall-jump") == 0 && sscanf(line, "%*s %d", &a) == 1) {
            roundie_desktop_clock_jump_wall((int64_t)a * 1000000);
        } else if (strcmp(command, "time-drift") == 0 && sscanf(line, "%*s %63s %d", word, &a) == 2 &&
                   (strcmp(word, "rtc") == 0 || strcmp(word, "chip") == 0)) {
            roundie_desktop_clock_drift(strcmp(word, "chip") == 0, a);
        } else if (strcmp(command, "quit") == 0) {
            atomic_store(&quit_requested, true);
        } else {
            script_lab(line, now);
            return;
        }
        script.next++;
    }
}

/* ---- Main -------------------------------------------------------------------- */

static void usage(void)
{
    fputs("usage: roundie-desktop [--board 1.85b|1.75c] [--data DIR] [--zoom N]\n"
          "                       [--headless] [--script FILE] [--install FILE.rpk]...\n"
          "                       [--native-apps]  (run linux-x64.rep1 apps: trusted code, no sandbox)\n"
          "                       [--virtual-time]  (time moves only while a script waits)\n"
          "                       [--lab-panel]  (virtual hardware control panel in a second window)\n"
          "                       [--resume FILE]  (internal: restart after a virtual hardware reset)\n"
          "keys: B = BOOT, P = PWR, Esc = quit; the mouse is the touch panel\n", stderr);
}

/* The package manager resolves storage/packages relative to the process
 * directory, so the process runs in the data directory from here on. */
static void prepare_data(const char *directory)
{
    char path[4096];
    if (mkdir(directory, 0755) != 0 && errno != EEXIST) {
        fail("data directory %s: %s", directory, strerror(errno));
    }
    if (chdir(directory) != 0) {
        fail("data directory %s: %s", directory, strerror(errno));
    }
    snprintf(path, sizeof(path), PACKAGES_PARENT);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fail("%s/%s: %s", directory, path, strerror(errno));
    }
    snprintf(path, sizeof(path), PACKAGES_PARENT "/packages");
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fail("%s/%s: %s", directory, path, strerror(errno));
    }
}

static void log_window(SDL_Window *window)
{
    int width = 0;
    int height = 0;
    int pixel_width = 0;
    int pixel_height = 0;
    SDL_GetWindowSize(window, &width, &height);
    SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);
    fprintf(stderr, "roundie-desktop: window %dx%d, %dx%d pixels, density %.2f, display scale %.2f\n", width,
            height, pixel_width, pixel_height, (double)SDL_GetWindowPixelDensity(window),
            (double)SDL_GetWindowDisplayScale(window));
}

/* Handles one SDL event; returns whether the window needs the last frame
 * presented again. */
static bool handle_event(const SDL_Event *event, vk_present_t *present, SDL_Window *window)
{
    if (gui.lab_panel != NULL && lab_panel_event(gui.lab_panel, event, roundie_time_us())) {
        return false; /* the lab panel's window */
    }
    float density = SDL_GetWindowPixelDensity(window);
    int32_t x = 0;
    int32_t y = 0;
    switch (event->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        atomic_store(&quit_requested, true);
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        log_window(window);
        vk_present_resized(present);
        return true;
    case SDL_EVENT_WINDOW_EXPOSED:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event->button.button == SDL_BUTTON_LEFT &&
            vk_present_map(present, event->button.x * density, event->button.y * density, &x, &y)) {
            roundie_desktop_touch_set(&gui.input, true, x, y);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (roundie_desktop_touch_active(&gui.input)) {
            vk_present_map(present, event->motion.x * density, event->motion.y * density, &x, &y);
            roundie_desktop_touch_set(&gui.input, true, x, y);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event->button.button == SDL_BUTTON_LEFT) {
            roundie_desktop_touch_set(&gui.input, false, 0, 0);
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (event->key.repeat) {
            break;
        }
        if (event->key.key == SDLK_ESCAPE) {
            atomic_store(&quit_requested, true);
        } else if (event->key.key == SDLK_B) {
            roundie_desktop_key(&gui.input, ROUNDIE_DESKTOP_KEY_BOOT, event->type == SDL_EVENT_KEY_DOWN);
        } else if (event->key.key == SDLK_P) {
            roundie_desktop_key(&gui.input, ROUNDIE_DESKTOP_KEY_PWR, event->type == SDL_EVENT_KEY_DOWN);
        }
        break;
    default:
        break;
    }
    return false;
}

/* ---- Virtual hardware: resets and power (docs/virtual-hardware.md) ---------- */

/* What a restarted process takes over from the previous one. */
typedef struct {
    bool present;
    roundie_desktop_reset_t reset;
    uint32_t script_next;
    int64_t script_wait_us;
} resume_t;

static resume_t resume;

/* Descriptors this process opened do not pass to the restarted one. */
static void close_on_exec_all(void)
{
    DIR *fds = opendir("/proc/self/fd");
    if (fds == NULL) {
        return;
    }
    int own = dirfd(fds);
    struct dirent *entry;
    while ((entry = readdir(fds)) != NULL) {
        int fd = atoi(entry->d_name);
        if (fd > 2 && fd != own) {
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }
    closedir(fds);
}

/* A reset or a power change of the virtual board restarts the desktop
 * process, as it ends every task on the board at once: no shutdown and no
 * cleanup. The machine, the timekeeping hardware and the script position go to
 * LAB_STATE_FILE in the data directory, and the new process reads them back
 * (--resume). Package installs on the command line are not repeated. GUI thread. */
static _Noreturn void restart_process(bool powered, roundie_desktop_reset_t reset, int64_t now)
{
    /* BootGuard blames the loaded application only after a panic or a watchdog reset. */
    if (!powered || (reset != ROUNDIE_DESKTOP_RESET_PANIC && reset != ROUNDIE_DESKTOP_RESET_TASK_WDT)) {
        roundie_desktop_apps_forget_loaded();
    }
    fprintf(stderr, "roundie-desktop: %s%s; restarting the process\n", powered ? roundie_desktop_reset_name(reset) : "power off",
            powered ? " reset" : "");
    FILE *file = fopen(LAB_STATE_FILE ".new", "w");
    if (file == NULL) {
        fail("%s: %s", LAB_STATE_FILE, strerror(errno));
    }
    struct timespec real;
    clock_gettime(CLOCK_MONOTONIC, &real);
    int64_t wait_us = script.resume_us > now ? script.resume_us - now : 0;
    fprintf(file, "lab.reset %d\nlab.script_next %u\nlab.script_wait %lld\nlab.real_ns %lld\n", (int)reset,
            (unsigned)script.next, (long long)wait_us, (long long)real.tv_sec * 1000000000LL + real.tv_nsec);
    vhw_save(file, now);
    roundie_desktop_clock_save(file);
    if (fclose(file) != 0 || rename(LAB_STATE_FILE ".new", LAB_STATE_FILE) != 0) {
        fail("%s: %s", LAB_STATE_FILE, strerror(errno));
    }
    char *state_path = realpath(LAB_STATE_FILE, NULL);
    char **arguments = calloc((size_t)saved_argc + 3U, sizeof(char *));
    if (state_path == NULL || arguments == NULL) {
        fail("restart: %s", strerror(errno));
    }
    static char resume_flag[] = "--resume";
    int count = 0;
    arguments[count++] = saved_argv[0];
    for (int i = 1; i < saved_argc; i++) {
        bool dropped = strcmp(saved_argv[i], "--install") == 0 || strcmp(saved_argv[i], "--resume") == 0;
        if (dropped && i + 1 < saved_argc) {
            i++;
            continue;
        }
        arguments[count++] = saved_argv[i];
    }
    arguments[count++] = resume_flag;
    arguments[count++] = state_path;
    arguments[count] = NULL;
    fflush(stdout);
    fflush(stderr);
    close_on_exec_all();
    if (caller_directory != NULL && chdir(caller_directory) != 0) {
        fail("restart: %s: %s", caller_directory, strerror(errno));
    }
    execv("/proc/self/exe", arguments);
    fail("restart: %s", strerror(errno));
}

/* Reads LAB_STATE_FILE of the previous process, after vhw_init() and
 * roundie_desktop_clock_init(), then removes it. */
static void resume_load(const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        fail("resume %s: %s", path, strerror(errno));
    }
    int64_t now = roundie_time_us();
    int64_t gap_us = 0;
    char line[4096];
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        char *value = strchr(line, ' ');
        if (value == NULL) {
            continue;
        }
        *value = '\0';
        value++;
        long long number = strtoll(value, NULL, 10);
        if (strcmp(line, "lab.reset") == 0 && number >= 0 && number <= ROUNDIE_DESKTOP_RESET_PIN) {
            resume.reset = (roundie_desktop_reset_t)number;
        } else if (strcmp(line, "lab.script_next") == 0 && number >= 0 && number <= SCRIPT_LINES) {
            resume.script_next = (uint32_t)number;
        } else if (strcmp(line, "lab.script_wait") == 0 && number >= 0) {
            resume.script_wait_us = number;
        } else if (strcmp(line, "lab.real_ns") == 0) {
            /* In real time the hardware clocks ran on while the process restarted. */
            struct timespec real;
            clock_gettime(CLOCK_MONOTONIC, &real);
            int64_t real_ns = (int64_t)real.tv_sec * 1000000000LL + real.tv_nsec;
            if (!roundie_desktop_time_frozen() && real_ns > number) {
                gap_us = (real_ns - number) / 1000;
            }
        } else if (!vhw_load_line(line, value, now) && !roundie_desktop_clock_load_line(line, value, gap_us)) {
            fprintf(stderr, "roundie-desktop: resume: ignored %s\n", line);
        }
    }
    fclose(file);
    if (unlink(path) != 0) {
        fprintf(stderr, "roundie-desktop: resume %s: %s\n", path, strerror(errno));
    }
    resume.present = true;
}

/* Carries out what the virtual board does next. Returns true when power came
 * back to a board that is off. GUI thread. */
static bool lab_poll(int64_t now)
{
    char command[64];
    bool clicked = false;
    if (gui.lab_panel != NULL && lab_panel_take_command(gui.lab_panel, command, sizeof(command))) {
        uint32_t hold_ms = 0;
        fprintf(stderr, "roundie-desktop: lab panel: %s\n", command);
        if (lab_command(command, &gui.input, now, &hold_ms) != LAB_DONE) {
            fprintf(stderr, "roundie-desktop: lab panel: %s refused\n", command);
        }
        clicked = true;
    }
    if (gui.lab_panel != NULL && !lab_panel_update(gui.lab_panel, now, clicked)) {
        fprintf(stderr, "roundie-desktop: lab panel presentation failed\n");
        atomic_store(&quit_requested, true);
    }
    vhw_set_pwr(roundie_desktop_key_pressed(&gui.input, ROUNDIE_DESKTOP_KEY_PWR), now);
    int kind = 0;
    vhw_action_t action = vhw_poll(now, &kind);
    if (action == VHW_ACTION_POWER_OFF) {
        vhw_state_t state;
        vhw_get(&state, now);
        fprintf(stderr, "roundie-desktop: power lost: %s\n", vhw_off_name(state.off_reason));
        restart_process(false, ROUNDIE_DESKTOP_RESET_POWER, now);
    }
    if (action == VHW_ACTION_RESET) {
        restart_process(true, (roundie_desktop_reset_t)kind, now);
    }
    return action == VHW_ACTION_POWER_ON;
}

/* The board is off, or its bring-up stopped (halted): no Roundie Core and a
 * dark panel; only the lab acts. Returns when power comes back to a board that
 * is off, or on quit. GUI thread. */
static void idle_loop(SDL_Window *window, vk_present_t *present, bool halted)
{
    bool present_again = true;
    while (!atomic_load(&quit_requested)) {
        if (window != NULL) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (handle_event(&event, present, window)) {
                    present_again = true;
                }
            }
        }
        if (script.count > 0) {
            script_step();
            if (script.next == script.count && !script.failed) {
                atomic_store(&quit_requested, true);
            } else if (roundie_desktop_time_frozen() && !script.failed) {
                /* No Core thread follows virtual time here. */
                int64_t now = roundie_time_us();
                int64_t step = VIRTUAL_STEP_US;
                if (script.resume_us > now && script.resume_us - now < step) {
                    step = script.resume_us - now;
                }
                roundie_desktop_time_advance(step);
            }
        }
        int64_t now = roundie_time_us();
        roundie_desktop_input_poll(&gui.input, now);
        if (lab_poll(now) && !halted) {
            return;
        }
        if (present_again && present != NULL) {
            present_again = false;
            compose(true);
            if (!vk_present_frame(present, gui.presented)) {
                fprintf(stderr, "roundie-desktop: presentation failed\n");
                atomic_store(&quit_requested, true);
            }
        }
        if (roundie_desktop_time_frozen()) {
            continue;
        }
        if (window != NULL) {
            SDL_WaitEventTimeout(NULL, 10);
        } else {
            roundie_desktop_sleep_real_ms(10);
        }
    }
}

/* Roundie Core on the powered board, until quit. Returns the exit code. */
static int run_core(SDL_Window *window, vk_present_t *present)
{
    /* The speaker; applications get no stream without it, as on a board whose codec failed. */
    if (roundie_audio_init(NULL) != ESP_OK) {
        fprintf(stderr, "roundie-desktop: no audio output\n");
    }

    lv_init();
    lv_tick_set_cb(tick_ms);
    create_display();
    lv_indev_t *pointer = lv_indev_create();
    if (pointer == NULL) {
        fail("LVGL input allocation failed");
    }
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, gui.display);
    lv_indev_set_read_cb(pointer, read_touch);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    roundie_ui_gui_bind();
    roundie_launcher_gui_bind();
    roundie_launcher_gui_set_quick_settings(&quick_settings);
    gui.lit = true;
    roundie_power_gui_bind(gui.board, &brightness_percent, roundie_time_us());
    /* The firmware's time startup (main.c): zone, then wall clock, then the
     * applications' clock source. */
    roundie_local_time_init(true, roundie_wall_clock_valid);
    roundie_wall_clock_init(roundie_desktop_clock_has_rtc());
    time_start();
    roundie_desktop_set_app_clock_source(roundie_local_time_now);

    pthread_t worker;
    if (pthread_create(&worker, NULL, system_worker, NULL) != 0) {
        fail("system worker thread");
    }

    int exit_code = 0;
    bool present_again = false;
    while (!atomic_load(&quit_requested)) {
        if (roundie_desktop_time_frozen()) {
            roundie_desktop_time_wait_follower();
        }
        if (window != NULL) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (handle_event(&event, present, window)) {
                    present_again = true;
                }
            }
        }
        if (script.count > 0) {
            script_step();
            if (script.next == script.count && !script.failed) {
                atomic_store(&quit_requested, true);
            } else if (roundie_desktop_time_frozen() && !script.failed) {
                virtual_time_step();
            }
        }
        roundie_desktop_input_poll(&gui.input, roundie_time_us());
        roundie_desktop_board_inputs(gui.board, roundie_desktop_key_pressed(&gui.input, ROUNDIE_DESKTOP_KEY_BOOT),
                                     roundie_desktop_touch_active(&gui.input));
        lab_poll(roundie_time_us());
        uint32_t next_ms = GUI_DELAY_MAX_MS;
        /* The panel flush is synchronous, so no transfer is ever in flight. */
        bool gui_active = roundie_power_gui_poll(roundie_time_us(), true, false);
        if (roundie_desktop_board_take_panel_lost(gui.board)) {
            /* A failed re-initialisation lost the panel memory: what remains
             * shows until the GUI repaints. */
            for (size_t i = 0; i < (size_t)gui.profile->width * gui.profile->height; i++) {
                gui.panel[i] = 0xF81F;
            }
            gui.dirty = true;
        }
        if (gui_active) {
            roundie_ui_gui_batch(ROUNDIE_UI_QUEUE_CAPACITY);
            roundie_ui_gui_pump();
            roundie_launcher_gui_pump(false);
            next_ms = lv_timer_handler();
        }
        /* Otherwise parked for light sleep: no LVGL work until resumed. */
        if (roundie_desktop_board_lit(gui.board) != gui.lit ||
            roundie_desktop_board_backlight(gui.board) != gui.backlight) {
            gui.dirty = true;
        }
        if ((gui.dirty || present_again) && present != NULL) {
            present_again = false;
            compose(true);
            if (!vk_present_frame(present, gui.presented)) {
                fprintf(stderr, "roundie-desktop: presentation failed\n");
                exit_code = 1;
                atomic_store(&quit_requested, true);
            }
            gui.dirty = false;
        }
        if (next_ms > GUI_DELAY_MAX_MS) {
            next_ms = GUI_DELAY_MAX_MS;
        }
        if (next_ms == 0 || roundie_desktop_time_frozen()) {
            next_ms = 1;
        }
        if (roundie_desktop_time_frozen()) {
            roundie_desktop_time_gui_done();
        }
        if (window != NULL) {
            SDL_WaitEventTimeout(NULL, (Sint32)next_ms);
        } else {
            roundie_desktop_sleep_real_ms(next_ms);
        }
    }
    roundie_desktop_time_release();
    pthread_join(worker, NULL);
    if (gui.flush_errors > 0) {
        fprintf(stderr, "roundie-desktop: %u flush errors\n", (unsigned)gui.flush_errors);
        exit_code = 1;
    }
    return exit_code;
}

int main(int argc, char **argv)
{
    const char *board = "1.85b";
    const char *data = NULL;
    const char *script_path = NULL;
    const char *resume_path = NULL;
    bool headless = false;
    bool lab_window = false;
    int zoom = 2;
    saved_argc = argc;
    saved_argv = argv;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--board") == 0 && i + 1 < argc) {
            board = argv[++i];
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data = argv[++i];
        } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            script_path = argv[++i];
        } else if (strcmp(argv[i], "--zoom") == 0 && i + 1 < argc) {
            zoom = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--install") == 0 && i + 1 < argc && install_count < INSTALLS_MAX) {
            /* Absolute, since the process moves to the data directory. */
            const char *path = argv[++i];
            installs[install_count] = realpath(path, NULL);
            if (installs[install_count] == NULL) {
                fail("install %s: %s", path, strerror(errno));
            }
            install_count++;
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--lab-panel") == 0) {
            lab_window = true;
        } else if (strcmp(argv[i], "--native-apps") == 0) {
            roundie_desktop_apps_enable(true);
        } else if (strcmp(argv[i], "--virtual-time") == 0) {
            roundie_desktop_time_freeze();
        } else if (strcmp(argv[i], "--resume") == 0 && i + 1 < argc) {
            resume_path = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        if (strcmp(board, profiles[i].name) == 0) {
            gui.profile = &profiles[i];
        }
    }
    if (gui.profile == NULL || zoom < 1 || zoom > 8) {
        usage();
        return 2;
    }
    if (script_path != NULL && !script_load(script_path)) {
        return 2;
    }
    char default_data[4096];
    if (data == NULL) {
        const char *base = getenv("XDG_DATA_HOME");
        const char *home = getenv("HOME");
        if (base != NULL && base[0] != '\0') {
            snprintf(default_data, sizeof(default_data), "%s/roundie-desktop", base);
        } else if (home != NULL) {
            snprintf(default_data, sizeof(default_data), "%s/.local/share/roundie-desktop", home);
        } else {
            fail("set --data, HOME or XDG_DATA_HOME");
        }
        data = default_data;
    }
    caller_directory = getcwd(NULL, 0);
    (void)roundie_time_us(); /* origin before any other thread */
    prepare_data(data);
    if (psa_crypto_init() != PSA_SUCCESS) {
        fail("PSA crypto initialisation failed");
    }

    /* The virtual hardware, and what the previous process left of it. */
    int64_t now = roundie_time_us();
    vhw_init(gui.profile->gauge ? VHW_CHIP_GAUGE : VHW_CHIP_PMIC, now);
    roundie_desktop_clock_init(gui.profile->rtc, "clock-tz");
    if (resume_path != NULL) {
        resume_load(resume_path);
        if (resume.script_next <= script.count) {
            script.next = resume.script_next;
        }
        script.resume_us = roundie_time_us() + resume.script_wait_us;
    }

    SDL_Window *window = NULL;
    vk_present_t *present = NULL;
    if (!headless) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            fail("SDL: %s", SDL_GetError());
        }
        char title[64];
        snprintf(title, sizeof(title), "Roundie %s (%ux%u)", gui.profile->name,
                 (unsigned)gui.profile->width, (unsigned)gui.profile->height);
        /* High density: on a scaled output the swapchain gets device pixels and
         * the panel an integer zoom, rather than a compositor-stretched buffer.
         * handle_event() converts mouse points with the window's pixel density. */
        window = SDL_CreateWindow(title, (int)gui.profile->width * zoom, (int)gui.profile->height * zoom,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (window == NULL) {
            fail("SDL window: %s", SDL_GetError());
        }
        present = vk_present_create(window, gui.profile->width, gui.profile->height);
        if (present == NULL) {
            fail("Vulkan presentation unavailable");
        }
        fprintf(stderr, "roundie-desktop: %s panel %ux%u on %s\n", gui.profile->name,
                (unsigned)gui.profile->width, (unsigned)gui.profile->height, vk_present_device(present));
        log_window(window);
    }

    roundie_desktop_input_init(&gui.input, gui.profile->width, gui.profile->height);
    create_panel();
    gui.lab_panel = lab_panel_create();
    if (gui.lab_panel != NULL && lab_window && !headless && !lab_panel_open_window(gui.lab_panel, zoom)) {
        fail("lab panel window unavailable");
    }
    gui.board = roundie_desktop_board();
    vhw_state_t state;
    vhw_get(&state, roundie_time_us());
    int exit_code = 0;
    bool start = true;
    if (!state.powered) {
        fprintf(stderr, "roundie-desktop: the board is off (%s)\n", vhw_off_name(state.off_reason));
        idle_loop(window, present, false);
        start = !atomic_load(&quit_requested);
        if (start) {
            /* Power-on reset; the PCF85063 kept time only on its backup. */
            vhw_get(&state, roundie_time_us());
            if (!state.rtc_backup) {
                roundie_desktop_clock_chip_power_lost();
            }
            roundie_desktop_clock_reset(ROUNDIE_DESKTOP_RESET_POWER);
            fprintf(stderr, "roundie-desktop: power on\n");
        }
    } else if (resume.present) {
        roundie_desktop_clock_reset(resume.reset);
        fprintf(stderr, "roundie-desktop: start after %s reset\n", roundie_desktop_reset_name(resume.reset));
    }
    if (start && state.display == VHW_DISPLAY_INIT) {
        /* main/main.c: roundie_board_start() failed; the board continues in its
         * headless recovery, which the desktop does not emulate. */
        ESP_LOGE("roundie", "Bring-up stopped: %s", esp_err_to_name(ESP_ERR_TIMEOUT));
        idle_loop(window, present, true);
        start = false;
    }
    if (start) {
        core_running = true;
        exit_code = run_core(window, present);
    }
    if (script.failed || atomic_load(&install_failed)) {
        exit_code = 1;
    }
    lab_panel_destroy(gui.lab_panel);
    vk_present_destroy(present);
    if (window != NULL) {
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    free(caller_directory);
    for (uint32_t i = 0; i < install_count; i++) {
        free(installs[i]);
    }
    return exit_code;
}
