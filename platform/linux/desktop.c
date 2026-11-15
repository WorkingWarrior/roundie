/* Roundie on the Linux desktop (docs/roundie-core.md).
 *
 * Runs the Roundie Core launcher and application UI bridge with LVGL on an
 * emulated panel. One GUI thread (this main thread) owns LVGL, SDL and
 * Vulkan, as the GUI task does on the board; one system worker thread polls
 * the launcher and the package manager, as the firmware's system worker. */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL3/SDL.h>
#include "psa/crypto.h"
#include "lvgl.h"
#include "display_scale.h"
#include "roundie_app_ui.h"
#include "roundie_launcher.h"
#include "roundie_packages.h"
#include "roundie_platform.h"
#include "desktop_input.h"
#include "vk_present.h"

#define PACKAGES_PARENT "storage"
#define GUI_DELAY_MAX_MS 33U
#define SCRIPT_LINES 512U
#define SCRIPT_LINE 256U
#define INSTALLS_MAX 8U

/* Panel profiles of the supported boards; the values mirror each board's
 * include/roundie_board_config.h and the 1.85B's default
 * CONFIG_ROUNDIE_LCD_STRIP_LINES. */
typedef struct {
    const char *name;
    uint32_t width;
    uint32_t height;
    uint32_t align;
    uint32_t strip_lines;
    bool round;
} profile_t;

static const profile_t profiles[] = {
    {"1.85b", 360, 360, 1, 20, true},
    {"1.75c", 466, 466, 2, 24, true},
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
} gui_t;

static gui_t gui;
static atomic_bool quit_requested;
static char *caller_directory; /* script paths are relative to it */
static char *installs[INSTALLS_MAX];
static uint32_t install_count;
static atomic_bool install_failed;

static void fail(const char *format, ...)
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

static void compose(void)
{
    size_t pixels = (size_t)gui.profile->width * gui.profile->height;
    for (size_t i = 0; i < pixels; i++) {
        uint16_t c = gui.panel[i];
        uint32_t r = (c >> 11) & 0x1FU;
        uint32_t g = (c >> 5) & 0x3FU;
        uint32_t b = c & 0x1FU;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        uint32_t value = 0xFF000000U | (r << 16) | (g << 8) | b;
        gui.presented[i] = gui.glass[i] ? value : 0xFF000000U;
    }
}

static bool save_png(const char *path)
{
    compose();
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

static void read_touch(lv_indev_t *device, lv_indev_data_t *data)
{
    (void)device;
    gui.touch_reads++;
    bool pressed = false;
    uint16_t x = 0;
    uint16_t y = 0;
    roundie_desktop_touch_read(&gui.input, &pressed, &x, &y);
    if (pressed && gui.scaled) {
        /* The board reports panel coordinates; LVGL works on the canvas. */
        x = (uint16_t)roundie_scale_touch(x, ROUNDIE_UI_WIDTH, gui.profile->width);
        y = (uint16_t)roundie_scale_touch(y, ROUNDIE_UI_HEIGHT, gui.profile->height);
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
        if (result != ESP_OK) {
            roundie_packages_abort(session);
        }
    }
    free(bytes);
    if (result != ESP_OK) {
        roundie_packages_status_t status;
        roundie_packages_status(&status);
        fprintf(stderr, "roundie-desktop: install %s: %s (%s)\n", path, esp_err_to_name(result), status.error);
        return false;
    }
    return true;
}

static void *system_worker(void *argument)
{
    (void)argument;
    for (uint32_t i = 0; i < install_count; i++) {
        if (!install_package(installs[i])) {
            atomic_store(&install_failed, true);
        }
    }
    roundie_launcher_set_home(ROUNDIE_LAUNCHER_HOME_PACKAGE, true);
    while (!atomic_load(&quit_requested)) {
        int64_t now = roundie_time_us();
        roundie_packages_poll(now);
        roundie_launcher_poll(true, roundie_packages_busy());
        roundie_sleep_ms(10);
    }
    return NULL;
}

/* ---- Scripted runs --------------------------------------------------------- */

static const char *const navigation_names[] = {
    "WAITING", "REFRESHING", "LIST", "REQUESTED", "LOADING", "ACTIVE", "EXITING", "ERROR", "RESULT",
};

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
} script_t;

static script_t script;

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
        } else if (strcmp(command, "key") == 0 && sscanf(line, "%*s %63s", word) == 1) {
            if (!roundie_desktop_key_name(&gui.input, word, true)) {
                script_fail(line, "unknown key");
                return;
            }
            snprintf(script.lines[script.next], SCRIPT_LINE, "release %s", word);
            script.resume_us = now + 100000;
            return;
        } else if (strcmp(command, "release") == 0 && sscanf(line, "%*s %63s", word) == 1) {
            roundie_desktop_key_name(&gui.input, word, false);
        } else if (strcmp(command, "screenshot") == 0 && sscanf(line, "%*s %63s", word) == 1) {
            char path[4096];
            if (word[0] == '/' || caller_directory == NULL) {
                snprintf(path, sizeof(path), "%s", word);
            } else {
                snprintf(path, sizeof(path), "%s/%s", caller_directory, word);
            }
            lv_refr_now(gui.display);
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
        } else if (strcmp(command, "quit") == 0) {
            atomic_store(&quit_requested, true);
        } else {
            script_fail(line, "unknown or malformed command");
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

int main(int argc, char **argv)
{
    const char *board = "1.85b";
    const char *data = NULL;
    const char *script_path = NULL;
    bool headless = false;
    int zoom = 2;
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

    pthread_t worker;
    if (pthread_create(&worker, NULL, system_worker, NULL) != 0) {
        fail("system worker thread");
    }

    int exit_code = 0;
    bool present_again = false;
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
            }
        }
        roundie_desktop_input_poll(&gui.input, roundie_time_us());
        roundie_ui_gui_batch(ROUNDIE_UI_QUEUE_CAPACITY);
        roundie_ui_gui_pump();
        roundie_launcher_gui_pump(false);
        uint32_t next_ms = lv_timer_handler();
        if ((gui.dirty || present_again) && present != NULL) {
            present_again = false;
            compose();
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
        if (next_ms == 0) {
            next_ms = 1;
        }
        if (window != NULL) {
            SDL_WaitEventTimeout(NULL, (Sint32)next_ms);
        } else {
            roundie_sleep_ms(next_ms);
        }
    }
    pthread_join(worker, NULL);
    if (script.failed || atomic_load(&install_failed)) {
        exit_code = 1;
    }
    if (gui.flush_errors > 0) {
        fprintf(stderr, "roundie-desktop: %u flush errors\n", (unsigned)gui.flush_errors);
        exit_code = 1;
    }
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
