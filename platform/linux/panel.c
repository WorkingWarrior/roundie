/* Control panel of the virtual hardware lab (panel.h). */

#include "panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "clock.h"
#include "vhw.h"
#include "vk_present.h"

#define FRAME_WIDTH 360
#define FRAME_HEIGHT 392
#define CHAR_SIZE SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE
#define MARGIN 8
#define LABEL_WIDTH 72
#define ROW_TOP 64
#define ROW_HEIGHT 25
#define BUTTON_HEIGHT 19
#define BUTTON_PADDING 6
#define BUTTON_GAP 5
#define UPDATE_INTERVAL_NS 100000000LL
#define COMMAND_SIZE 64

/* What a button's highlight follows. */
typedef enum {
    GROUP_NONE,
    GROUP_USB,
    GROUP_PRESENT,
    GROUP_GAUGE,
    GROUP_BACKUP,
    GROUP_RTC,
    GROUP_DISPLAY,
    GROUP_TOUCH,
    GROUP_AUDIO,
    GROUP_STORAGE,
    GROUP_LIMIT,
} group_t;

typedef struct {
    const char *label;
    const char *command;
    group_t group;
    uint64_t value; /* the group's value while this button is lit */
} button_t;

#define BUTTONS_PER_ROW 5

typedef struct {
    const char *label;
    button_t buttons[BUTTONS_PER_ROW];
} row_t;

/* The panel's model: every control is a lab command (lab.c). */
static const row_t rows[] = {
    {"Supply", {{"USB in", "usb 1", GROUP_USB, 1}, {"USB out", "usb 0", GROUP_USB, 0},
                {"Cut", "power-cut", GROUP_NONE, 0}, {"Restore", "power-restore", GROUP_NONE, 0}}},
    {"Keys", {{"PWR", "key pwr 100", GROUP_NONE, 0}, {"PWR 2.5s", "key pwr 2500", GROUP_NONE, 0},
              {"BOOT", "key boot 100", GROUP_NONE, 0}, {"BOOT 1.5s", "key boot 1500", GROUP_NONE, 0}}},
    {"Charge", {{"0%", "battery soc 0", GROUP_NONE, 0}, {"5%", "battery soc 5", GROUP_NONE, 0},
                {"20%", "battery soc 20", GROUP_NONE, 0}, {"50%", "battery soc 50", GROUP_NONE, 0},
                {"100%", "battery soc 100", GROUP_NONE, 0}}},
    {"Cell", {{"in", "battery present 1", GROUP_PRESENT, 1}, {"out", "battery present 0", GROUP_PRESENT, 0},
              {"RTC bat", "rtc-backup 1", GROUP_BACKUP, 1}, {"no RTC bat", "rtc-backup 0", GROUP_BACKUP, 0}}},
    {"Gauge", {{"ok", "gauge-fault none", GROUP_GAUGE, VHW_BUS_OK},
               {"timeout", "gauge-fault timeout", GROUP_GAUGE, VHW_BUS_TIMEOUT},
               {"absent", "gauge-fault absent", GROUP_GAUGE, VHW_BUS_ABSENT}}},
    {"Reset", {{"SW", "reboot sw", GROUP_NONE, 0}, {"panic", "reboot panic", GROUP_NONE, 0},
               {"WDT", "reboot wdt", GROUP_NONE, 0}, {"brownout", "reboot brownout", GROUP_NONE, 0}}},
    {"RTC", {{"ok", "rtc-fault none", GROUP_RTC, ROUNDIE_DESKTOP_RTC_RUNNING},
             {"stop", "rtc-fault stop", GROUP_RTC, ROUNDIE_DESKTOP_RTC_STOPPED},
             {"bus", "rtc-fault bus", GROUP_RTC, ROUNDIE_DESKTOP_RTC_BUS},
             {"absent", "rtc-fault absent", GROUP_RTC, ROUNDIE_DESKTOP_RTC_ABSENT}}},
    {"Display", {{"ok", "display-fault none", GROUP_DISPLAY, VHW_DISPLAY_OK},
                 {"wake fails", "display-fault restore", GROUP_DISPLAY, VHW_DISPLAY_RESTORE},
                 {"init fails", "display-fault init", GROUP_DISPLAY, VHW_DISPLAY_INIT}}},
    {"Touch", {{"ok", "touch-fault none", GROUP_TOUCH, VHW_TOUCH_OK},
               {"timeout", "touch-fault timeout", GROUP_TOUCH, VHW_TOUCH_TIMEOUT},
               {"noise", "touch-fault noise", GROUP_TOUCH, VHW_TOUCH_NOISE},
               {"absent", "touch-fault absent", GROUP_TOUCH, VHW_TOUCH_ABSENT}}},
    {"Audio", {{"ok", "audio-fault none", GROUP_AUDIO, VHW_AUDIO_OK},
               {"unavailable", "audio-fault unavailable", GROUP_AUDIO, VHW_AUDIO_UNAVAILABLE},
               {"underrun", "audio-underrun", GROUP_NONE, 0}}},
    {"Storage", {{"ok", "storage-fault none", GROUP_STORAGE, VHW_STORAGE_OK},
                 {"absent", "storage-fault absent", GROUP_STORAGE, VHW_STORAGE_ABSENT},
                 {"I/O", "storage-fault io", GROUP_STORAGE, VHW_STORAGE_IO},
                 {"64 KB", "storage-limit 64", GROUP_LIMIT, 65536},
                 {"no limit", "storage-limit 0", GROUP_LIMIT, 0}}},
};

#define ROW_COUNT (sizeof(rows) / sizeof(rows[0]))

struct lab_panel {
    uint32_t pixels[FRAME_WIDTH * FRAME_HEIGHT]; /* B8G8R8A8, as vk_present takes it */
    SDL_Surface *surface;
    SDL_Renderer *renderer;
    SDL_Window *window;
    vk_present_t *present;
    int64_t drawn_ns;
    bool clicked;
    char command[COMMAND_SIZE];
};

static int64_t real_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static int button_width(const button_t *button)
{
    return (int)strlen(button->label) * CHAR_SIZE + 2 * BUTTON_PADDING;
}

/* The rectangle of rows[row].buttons[index]. */
static SDL_FRect button_rect(size_t row, size_t index)
{
    float x = MARGIN + LABEL_WIDTH;
    for (size_t i = 0; i < index; i++) {
        x += (float)(button_width(&rows[row].buttons[i]) + BUTTON_GAP);
    }
    SDL_FRect rect = {x, (float)(ROW_TOP + (int)row * ROW_HEIGHT), (float)button_width(&rows[row].buttons[index]),
                      BUTTON_HEIGHT};
    return rect;
}

static uint64_t group_value(group_t group, const vhw_state_t *state)
{
    switch (group) {
    case GROUP_USB:
        return state->usb;
    case GROUP_PRESENT:
        return state->battery_present;
    case GROUP_GAUGE:
        return state->gauge;
    case GROUP_BACKUP:
        return state->rtc_backup;
    case GROUP_RTC:
        return (uint64_t)roundie_desktop_clock_rtc_fault_get();
    case GROUP_DISPLAY:
        return state->display;
    case GROUP_TOUCH:
        return state->touch;
    case GROUP_AUDIO:
        return state->audio;
    case GROUP_STORAGE:
        return state->storage;
    case GROUP_LIMIT:
        return state->storage_limit_bytes;
    case GROUP_NONE:
        break;
    }
    return UINT64_MAX;
}

lab_panel_t *lab_panel_create(void)
{
    lab_panel_t *panel = calloc(1, sizeof(*panel));
    if (panel == NULL) {
        fprintf(stderr, "roundie-desktop: lab panel: out of memory\n");
        return NULL;
    }
    panel->surface = SDL_CreateSurfaceFrom(FRAME_WIDTH, FRAME_HEIGHT, SDL_PIXELFORMAT_ARGB8888, panel->pixels,
                                           FRAME_WIDTH * 4);
    if (panel->surface != NULL) {
        panel->renderer = SDL_CreateSoftwareRenderer(panel->surface);
    }
    if (panel->renderer == NULL) {
        fprintf(stderr, "roundie-desktop: lab panel: %s\n", SDL_GetError());
        lab_panel_destroy(panel);
        return NULL;
    }
    return panel;
}

bool lab_panel_open_window(lab_panel_t *panel, int zoom)
{
    panel->window = SDL_CreateWindow("Roundie virtual hardware", FRAME_WIDTH * zoom, FRAME_HEIGHT * zoom,
                                     SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (panel->window == NULL) {
        fprintf(stderr, "roundie-desktop: lab panel window: %s\n", SDL_GetError());
        return false;
    }
    panel->present = vk_present_create(panel->window, FRAME_WIDTH, FRAME_HEIGHT);
    if (panel->present == NULL) {
        SDL_DestroyWindow(panel->window);
        panel->window = NULL;
        return false;
    }
    return true;
}

static void close_window(lab_panel_t *panel)
{
    vk_present_destroy(panel->present);
    panel->present = NULL;
    if (panel->window != NULL) {
        SDL_DestroyWindow(panel->window);
        panel->window = NULL;
    }
}

void lab_panel_destroy(lab_panel_t *panel)
{
    if (panel == NULL) {
        return;
    }
    close_window(panel);
    if (panel->renderer != NULL) {
        SDL_DestroyRenderer(panel->renderer);
    }
    if (panel->surface != NULL) {
        SDL_DestroySurface(panel->surface);
    }
    free(panel);
}

bool lab_panel_command_at(const lab_panel_t *panel, int32_t x, int32_t y, int64_t now_us, char *command,
                          size_t size)
{
    (void)panel;
    (void)now_us;
    SDL_FPoint point = {(float)x + 0.5f, (float)y + 0.5f};
    for (size_t row = 0; row < ROW_COUNT; row++) {
        for (size_t index = 0; index < BUTTONS_PER_ROW && rows[row].buttons[index].label != NULL; index++) {
            SDL_FRect rect = button_rect(row, index);
            if (SDL_PointInRectFloat(&point, &rect)) {
                snprintf(command, size, "%s", rows[row].buttons[index].command);
                return true;
            }
        }
    }
    return false;
}

bool lab_panel_event(lab_panel_t *panel, const SDL_Event *event, int64_t now_us)
{
    if (panel->window == NULL || SDL_GetWindowFromEvent(event) != panel->window) {
        return false;
    }
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        /* Only the panel closes; the emulated board runs on. */
        close_window(panel);
    } else if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        vk_present_resized(panel->present);
        panel->drawn_ns = 0;
    } else if (event->type == SDL_EVENT_WINDOW_EXPOSED) {
        panel->drawn_ns = 0;
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT) {
        float density = SDL_GetWindowPixelDensity(panel->window);
        int32_t x = 0;
        int32_t y = 0;
        if (vk_present_map(panel->present, event->button.x * density, event->button.y * density, &x, &y) &&
            lab_panel_command_at(panel, x, y, now_us, panel->command, sizeof(panel->command))) {
            panel->clicked = true;
        }
    }
    return true;
}

bool lab_panel_take_command(lab_panel_t *panel, char *command, size_t size)
{
    if (!panel->clicked) {
        return false;
    }
    panel->clicked = false;
    snprintf(command, size, "%s", panel->command);
    return true;
}

static void text(SDL_Renderer *renderer, float x, float y, const char *line)
{
    SDL_RenderDebugText(renderer, x, y, line);
}

static void draw(lab_panel_t *panel, int64_t now_us)
{
    SDL_Renderer *renderer = panel->renderer;
    vhw_state_t state;
    vhw_get(&state, now_us);
    roundie_battery_sample_t sample;
    vhw_battery_sample(&sample, now_us);
    SDL_SetRenderDrawColor(renderer, 16, 20, 24, 255);
    SDL_RenderClear(renderer);
    char line[96];
    SDL_SetRenderDrawColor(renderer, 230, 235, 240, 255);
    if (state.powered) {
        snprintf(line, sizeof(line), "Board on%s   power cycles %u", state.sleeping ? ", light sleep" : "",
                 (unsigned)state.power_cycles);
    } else {
        snprintf(line, sizeof(line), "Board OFF (%s)   power cycles %u", vhw_off_name(state.off_reason),
                 (unsigned)state.power_cycles);
    }
    text(renderer, MARGIN, MARGIN, line);
    snprintf(line, sizeof(line), "Cell %u%%  %u/%u mAh  %+d mA  OCV %u mV", (unsigned)state.soc_percent,
             (unsigned)(state.charge_mah_x1000 / 1000U), (unsigned)state.full_mah, (int)state.current_ma,
             (unsigned)state.open_circuit_mv);
    text(renderer, MARGIN, MARGIN + 14, line);
    snprintf(line, sizeof(line), "Gauge reads %u mV, %s", (unsigned)sample.voltage_mv,
             sample.soc_valid ? "SoC valid" : "no SoC");
    text(renderer, MARGIN, MARGIN + 28, line);
    snprintf(line, sizeof(line), "RTC %s   USB %s", roundie_desktop_clock_has_rtc() ? "PCF85063" : "none on board",
             state.usb ? "connected" : "out");
    text(renderer, MARGIN, MARGIN + 42, line);
    for (size_t row = 0; row < ROW_COUNT; row++) {
        SDL_SetRenderDrawColor(renderer, 150, 160, 170, 255);
        text(renderer, MARGIN, (float)(ROW_TOP + (int)row * ROW_HEIGHT + (BUTTON_HEIGHT - CHAR_SIZE) / 2),
             rows[row].label);
        for (size_t index = 0; index < BUTTONS_PER_ROW && rows[row].buttons[index].label != NULL; index++) {
            const button_t *button = &rows[row].buttons[index];
            SDL_FRect rect = button_rect(row, index);
            bool lit = button->group != GROUP_NONE && group_value(button->group, &state) == button->value;
            if (lit) {
                SDL_SetRenderDrawColor(renderer, 30, 120, 210, 255);
            } else {
                SDL_SetRenderDrawColor(renderer, 44, 50, 58, 255);
            }
            SDL_RenderFillRect(renderer, &rect);
            SDL_SetRenderDrawColor(renderer, 240, 244, 248, 255);
            text(renderer, rect.x + BUTTON_PADDING, rect.y + (BUTTON_HEIGHT - CHAR_SIZE) / 2.0f, button->label);
        }
    }
    SDL_RenderPresent(renderer);
}

bool lab_panel_update(lab_panel_t *panel, int64_t now_us, bool force)
{
    if (panel->window == NULL) {
        return true;
    }
    int64_t now_ns = real_ns();
    if (!force && panel->drawn_ns != 0 && now_ns - panel->drawn_ns < UPDATE_INTERVAL_NS) {
        return true;
    }
    panel->drawn_ns = now_ns;
    draw(panel, now_us);
    return vk_present_frame(panel->present, panel->pixels);
}

bool lab_panel_save(lab_panel_t *panel, int64_t now_us, const char *path)
{
    draw(panel, now_us);
    if (!SDL_SavePNG(panel->surface, path)) {
        fprintf(stderr, "roundie-desktop: lab panel %s: %s\n", path, SDL_GetError());
        return false;
    }
    return true;
}
