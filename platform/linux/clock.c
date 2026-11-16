/* Linux port of roundie_clock_port.h and of the board's RTC calls, on
 * emulated timekeeping hardware (clock.h, docs/roundie-core.md M3).
 *
 * Everything derives from roundie_time_us(), so a forward advance moves all
 * clocks together. The system wall clock is roundie_time_us() + wall_offset_us.
 * The RTC timer and the PCF85063 run from their own anchors at their own
 * rates. The PCF85063 registers go through the 1.85B's own codec
 * (sensor_codec.c), as on the board. */

#include "clock.h"

#include <stdio.h>
#include <string.h>
#include "roundie_board.h"
#include "roundie_clock_port.h"
#include "roundie_platform.h"
#include "sensor_codec.h"

/* The PCF85063's reset value: 2000-01-01 00:00:00. */
#define CHIP_RESET_US (INT64_C(946684800) * 1000000)

typedef struct {
    int64_t value_us; /* reading at anchor_mono_us */
    int64_t anchor_mono_us;
    int32_t ppm;
    bool running;
} oscillator_t;

static roundie_lock_t lock = ROUNDIE_LOCK_INITIALIZER;
/* Under lock. */
static bool has_rtc;
static int64_t wall_offset_us;
static oscillator_t rtc_timer;       /* ESP32-S3 RTC timer, esp_rtc_get_time_us() */
static int64_t boot_time_us;         /* RTC_BOOT_TIME registers */
static roundie_clock_retention_t retained; /* RTC_NOINIT memory */
static roundie_desktop_reset_t reset_reason = ROUNDIE_DESKTOP_RESET_POWER;
static uint32_t power_cycles;
static oscillator_t chip;            /* PCF85063 */
static bool chip_os_flag;
static bool chip_bus_fault;
static int64_t sleep_started_us;
/* Set before other threads start. */
static char zone_file[512];

static roundie_lock_t zone_lock = ROUNDIE_LOCK_INITIALIZER;

static const char *const reset_names[] = {
    [ROUNDIE_DESKTOP_RESET_POWER] = "power",
    [ROUNDIE_DESKTOP_RESET_SW] = "sw",
    [ROUNDIE_DESKTOP_RESET_PANIC] = "panic",
    [ROUNDIE_DESKTOP_RESET_TASK_WDT] = "wdt",
    [ROUNDIE_DESKTOP_RESET_BROWNOUT] = "brownout",
    [ROUNDIE_DESKTOP_RESET_PIN] = "pin",
};

static int64_t oscillator_read(const oscillator_t *oscillator, int64_t mono_us)
{
    if (!oscillator->running) {
        return oscillator->value_us;
    }
    int64_t elapsed = mono_us - oscillator->anchor_mono_us;
    return oscillator->value_us + elapsed + elapsed / 1000000 * oscillator->ppm +
           elapsed % 1000000 * oscillator->ppm / 1000000;
}

static void oscillator_anchor(oscillator_t *oscillator, int64_t mono_us, int64_t value_us)
{
    oscillator->value_us = value_us;
    oscillator->anchor_mono_us = mono_us;
}

/* Contents of RTC_NOINIT memory after power-on: arbitrary, reproducible. */
static void scramble_retained(void)
{
    uint32_t state = 0x9e3779b9U ^ ++power_cycles;
    uint8_t *bytes = (uint8_t *)&retained;
    for (size_t i = 0; i < sizeof(retained); i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes[i] = (uint8_t)state;
    }
}

/* Under lock. ESP-IDF startup: system time = boot time + RTC timer. */
static void restore_system_time(int64_t mono_us)
{
    wall_offset_us = boot_time_us + oscillator_read(&rtc_timer, mono_us) - mono_us;
}

void roundie_desktop_clock_init(bool board_has_rtc, const char *zone_path)
{
    snprintf(zone_file, sizeof(zone_file), "%s", zone_path);
    int64_t mono = roundie_time_us();
    roundie_lock(&lock);
    has_rtc = board_has_rtc;
    rtc_timer = (oscillator_t){.anchor_mono_us = mono, .running = true};
    boot_time_us = 0;
    scramble_retained();
    reset_reason = ROUNDIE_DESKTOP_RESET_POWER;
    chip = (oscillator_t){.value_us = CHIP_RESET_US, .anchor_mono_us = mono, .running = true};
    chip_os_flag = true;
    chip_bus_fault = false;
    restore_system_time(mono);
    roundie_unlock(&lock);
}

bool roundie_desktop_clock_has_rtc(void)
{
    roundie_lock(&lock);
    bool value = has_rtc;
    roundie_unlock(&lock);
    return value;
}

void roundie_desktop_clock_advance(int64_t us)
{
    roundie_desktop_time_advance(us);
}

void roundie_desktop_clock_jump_wall(int64_t us)
{
    roundie_lock(&lock);
    wall_offset_us += us;
    roundie_unlock(&lock);
}

void roundie_desktop_clock_drift(bool use_chip, int32_t ppm)
{
    int64_t mono = roundie_time_us();
    roundie_lock(&lock);
    oscillator_t *oscillator = use_chip ? &chip : &rtc_timer;
    oscillator_anchor(oscillator, mono, oscillator_read(oscillator, mono));
    oscillator->ppm = ppm;
    roundie_unlock(&lock);
}

void roundie_desktop_clock_rtc_fault(roundie_desktop_rtc_fault_t fault)
{
    int64_t mono = roundie_time_us();
    roundie_lock(&lock);
    oscillator_anchor(&chip, mono, oscillator_read(&chip, mono));
    chip_bus_fault = fault == ROUNDIE_DESKTOP_RTC_BUS;
    if (fault == ROUNDIE_DESKTOP_RTC_STOPPED) {
        chip.running = false;
        chip_os_flag = true;
    } else {
        /* A restarted oscillator keeps the OS flag until the time is set. */
        chip.running = true;
    }
    roundie_unlock(&lock);
}

void roundie_desktop_clock_reset(roundie_desktop_reset_t reset)
{
    int64_t mono = roundie_time_us();
    roundie_lock(&lock);
    if (reset == ROUNDIE_DESKTOP_RESET_POWER || reset == ROUNDIE_DESKTOP_RESET_PIN) {
        /* Assumption: power loss and CHIP_PU clear the RTC domain. The
         * PCF85063 keeps running on its backup battery. */
        oscillator_anchor(&rtc_timer, mono, 0);
        boot_time_us = 0;
        scramble_retained();
    }
    /* Assumption: a brownout keeps the RTC timer; the firmware still distrusts
     * it by reset reason. Software, panic and watchdog resets keep it. */
    reset_reason = reset;
    restore_system_time(mono);
    roundie_unlock(&lock);
}

const char *roundie_desktop_reset_name(roundie_desktop_reset_t reset)
{
    return reset_names[reset];
}

bool roundie_desktop_reset_parse(const char *name, roundie_desktop_reset_t *reset)
{
    for (size_t i = 0; i < sizeof(reset_names) / sizeof(reset_names[0]); i++) {
        if (strcmp(name, reset_names[i]) == 0) {
            *reset = (roundie_desktop_reset_t)i;
            return true;
        }
    }
    return false;
}

void roundie_desktop_clock_sleep_enter(void)
{
    int64_t mono = roundie_time_us();
    roundie_lock(&lock);
    sleep_started_us = mono;
    roundie_unlock(&lock);
}

void roundie_desktop_clock_sleep_exit(void)
{
    int64_t mono = roundie_time_us();
    roundie_lock(&lock);
    int64_t slept = mono - sleep_started_us;
    /* The RTC timer's count of the sleep, minus the true duration. Assumption:
     * on the board esp_timer is compensated the same way; here monotonic time
     * stays exact, since it must never go backwards. */
    wall_offset_us += slept / 1000000 * rtc_timer.ppm + slept % 1000000 * rtc_timer.ppm / 1000000;
    roundie_unlock(&lock);
}

/* ---- roundie_clock_port.h ------------------------------------------------ */

int64_t roundie_clock_port_wall_us(void)
{
    int64_t mono = roundie_time_us();
    roundie_lock(&lock);
    int64_t wall = mono + wall_offset_us;
    roundie_unlock(&lock);
    return wall;
}

bool roundie_clock_port_wall_set(int64_t seconds)
{
    int64_t mono = roundie_time_us();
    roundie_lock(&lock);
    wall_offset_us = seconds * 1000000 - mono;
    /* settimeofday() also moves the boot time that survives a reset. */
    boot_time_us = seconds * 1000000 - oscillator_read(&rtc_timer, mono);
    roundie_unlock(&lock);
    return true;
}

int64_t roundie_clock_port_retention_timer_us(void)
{
    int64_t mono = roundie_time_us();
    roundie_lock(&lock);
    int64_t value = oscillator_read(&rtc_timer, mono);
    roundie_unlock(&lock);
    return value;
}

void roundie_clock_port_retention_align(void)
{
    int64_t mono = roundie_time_us();
    roundie_lock(&lock);
    boot_time_us = mono + wall_offset_us - oscillator_read(&rtc_timer, mono);
    roundie_unlock(&lock);
}

/* The shared code calls this from the system worker only, as on the board. */
roundie_clock_retention_t *roundie_clock_port_retained(void)
{
    return &retained;
}

bool roundie_clock_port_reset_kept_domain(int *reason)
{
    roundie_lock(&lock);
    roundie_desktop_reset_t value = reset_reason;
    roundie_unlock(&lock);
    *reason = (int)value;
    return value == ROUNDIE_DESKTOP_RESET_SW || value == ROUNDIE_DESKTOP_RESET_PANIC ||
           value == ROUNDIE_DESKTOP_RESET_TASK_WDT;
}

void roundie_clock_port_zone_lock_init(void)
{
}

void roundie_clock_port_zone_lock(void)
{
    roundie_lock(&zone_lock);
}

void roundie_clock_port_zone_unlock(void)
{
    roundie_unlock(&zone_lock);
}

bool roundie_clock_port_zone_load(char rule[ROUNDIE_TIME_ZONE_CAPACITY])
{
    FILE *file = fopen(zone_file, "r");
    if (file == NULL) {
        return false;
    }
    char line[ROUNDIE_TIME_ZONE_CAPACITY + 2];
    bool loaded = fgets(line, sizeof(line), file) != NULL;
    fclose(file);
    if (!loaded) {
        return false;
    }
    line[strcspn(line, "\n")] = '\0';
    size_t length = strlen(line);
    if (length == 0 || length >= ROUNDIE_TIME_ZONE_CAPACITY) {
        return false;
    }
    memcpy(rule, line, length + 1);
    return true;
}

/* Written whole, then renamed over the old rule. */
esp_err_t roundie_clock_port_zone_store(const char *rule)
{
    char temporary[sizeof(zone_file) + 8];
    snprintf(temporary, sizeof(temporary), "%s.new", zone_file);
    FILE *file = fopen(temporary, "w");
    if (file == NULL) {
        return ESP_FAIL;
    }
    bool written = fprintf(file, "%s\n", rule) > 0;
    written = fclose(file) == 0 && written;
    if (!written || rename(temporary, zone_file) != 0) {
        remove(temporary);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- roundie_board_rtc_*: the 1.85B's PCF85063 ----------------------------- */

/* Floor division: the chip may hold a time before 1970 only in theory. */
static int64_t whole_seconds(int64_t us)
{
    int64_t seconds = us / 1000000;
    if (us % 1000000 < 0) {
        seconds--;
    }
    return seconds;
}

void roundie_board_rtc_read(roundie_board_t *board, roundie_rtc_sample_t *sample)
{
    if (sample == NULL) {
        return;
    }
    memset(sample, 0, sizeof(*sample));
    sample->state = ROUNDIE_SENSOR_NOT_FOUND;
    sample->error = ESP_ERR_NOT_FOUND;
    int64_t mono = roundie_time_us();
    sample->observed_us = mono;
    /* As the 1.85B driver: no board, no RTC. */
    if (board == NULL) {
        return;
    }
    roundie_lock(&lock);
    bool present = has_rtc;
    bool bus_fault = chip_bus_fault;
    int64_t chip_us = oscillator_read(&chip, mono);
    bool os_flag = chip_os_flag;
    roundie_unlock(&lock);
    if (!present) {
        return;
    }
    if (bus_fault) {
        sample->state = ROUNDIE_SENSOR_COMM_ERROR;
        sample->error = ESP_ERR_TIMEOUT;
        return;
    }
    roundie_rtc_time_t calendar;
    uint8_t raw[7];
    if (!roundie_rtc_from_unix(whole_seconds(chip_us), &calendar) || !roundie_rtc_encode(&calendar, raw)) {
        /* Outside 2000..2099: registers this codec cannot represent. */
        sample->state = ROUNDIE_SENSOR_INVALID;
        sample->error = ESP_ERR_INVALID_RESPONSE;
        return;
    }
    if (os_flag) {
        raw[0] |= 0x80;
    }
    bool calendar_valid = roundie_rtc_decode(raw, &sample->time, &sample->oscillator_stopped);
    if (!calendar_valid || sample->oscillator_stopped) {
        sample->state = ROUNDIE_SENSOR_INVALID;
        sample->error = ESP_ERR_INVALID_RESPONSE;
    } else {
        sample->state = ROUNDIE_SENSOR_VALID;
        sample->error = ESP_OK;
    }
}

/* As the 1.85B driver: write seconds..years (OS = 0), then verify readback. */
esp_err_t roundie_board_rtc_set_trusted(roundie_board_t *board, const roundie_rtc_time_t *time)
{
    uint8_t raw[7];
    int64_t seconds;
    if (board == NULL || time == NULL || !roundie_rtc_encode(time, raw) || !roundie_rtc_unix_seconds(time, &seconds)) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t mono = roundie_time_us();
    roundie_lock(&lock);
    if (!has_rtc) {
        roundie_unlock(&lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (chip_bus_fault) {
        roundie_unlock(&lock);
        return ESP_ERR_TIMEOUT;
    }
    oscillator_anchor(&chip, mono, seconds * 1000000);
    chip_os_flag = false;
    roundie_unlock(&lock);
    roundie_rtc_sample_t verified;
    roundie_board_rtc_read(board, &verified);
    if (verified.state != ROUNDIE_SENSOR_VALID) {
        return verified.error;
    }
    int64_t actual_seconds;
    if (!roundie_rtc_unix_seconds(&verified.time, &actual_seconds) ||
        !roundie_rtc_readback_matches(seconds, actual_seconds)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}
