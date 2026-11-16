/* Shared time code (components/roundie_time) on the desktop's emulated
 * timekeeping hardware (platform/linux/clock.c), with a virtual monotonic
 * clock: every scenario is deterministic. Usage: test_wall_clock DIRECTORY,
 * where the time zone rule is stored. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clock.h"
#include "local_time.h"
#include "roundie_clock_port.h"
#include "roundie_wall_clock.h"
#include "time_protocol.h"

#define SECOND INT64_C(1000000)
#define HOUR (3600 * SECOND)
#define DAY (86400 * SECOND)

static int64_t mono_us;
static char zone_path[4096];
/* The published board: clock.c needs only a non-NULL handle. NULL is the
 * firmware's board before publication. */
static int board_handle;
#define BOARD ((roundie_board_t *)&board_handle)

int64_t roundie_time_us(void)
{
    return mono_us;
}

void roundie_sleep_ms(uint32_t ms)
{
    mono_us += (int64_t)ms * 1000;
}

void roundie_desktop_time_advance(int64_t us)
{
    mono_us += us;
}

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "error";
}

static int64_t civil_seconds(int year, unsigned month, unsigned day, int hour, int minute, int second)
{
    return roundie_time_days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

static bool valid(void)
{
    uint32_t sync_count;
    return roundie_wall_clock_valid(&sync_count);
}

static uint32_t syncs(void)
{
    roundie_wall_clock_status_t status;
    roundie_wall_clock_status(&status);
    return status.sync_count;
}

/* The firmware's startup after a reset: start, then publish the board. */
static bool start_and_publish(void)
{
    roundie_rtc_sample_t rtc;
    roundie_board_rtc_read(BOARD, &rtc);
    bool trusted = roundie_wall_clock_start(&rtc);
    assert(!valid()); /* not before publication */
    roundie_wall_clock_publish();
    assert(valid() == trusted);
    return trusted;
}

/* The firmware's startup after a reset of the given kind. */
static bool boot(roundie_desktop_reset_t reset)
{
    roundie_desktop_clock_reset(reset);
    return start_and_publish();
}

/* A fresh device: power-on with the given board. */
static void power_on(bool has_rtc, int64_t start_us)
{
    mono_us = start_us;
    remove(zone_path);
    roundie_desktop_clock_init(has_rtc, zone_path);
    roundie_local_time_init(true, roundie_wall_clock_valid);
    roundie_wall_clock_init(has_rtc);
    assert(!boot(ROUNDIE_DESKTOP_RESET_POWER));
}

static void poll(void)
{
    roundie_rtc_sample_t rtc;
    roundie_board_rtc_read(BOARD, &rtc);
    roundie_wall_clock_poll(&rtc);
}

static const char *request(const char *refusal, bool set, int64_t seconds)
{
    roundie_rtc_sample_t rtc;
    esp_err_t error;
    return roundie_wall_clock_request(BOARD, refusal, set, seconds, &rtc, &error);
}

static int64_t wall_seconds(void)
{
    return roundie_clock_port_wall_us() / SECOND;
}

static roundie_elf_clock_t local(void)
{
    roundie_elf_clock_t clock;
    assert(roundie_local_time_now(&clock));
    return clock;
}

static void assert_local(int year, unsigned month, unsigned day, int hour, int minute, int second)
{
    roundie_elf_clock_t clock = local();
    assert(clock.days == (uint32_t)roundie_time_days_from_civil(year, month, day));
    assert(clock.ms_of_day / 1000 == (uint32_t)(hour * 3600 + minute * 60 + second));
}

static void without_rtc(void)
{
    const int64_t set_at = civil_seconds(2026, 10, 17, 9, 42, 6);
    power_on(false, SECOND);
    roundie_elf_clock_t unused;
    assert(!roundie_local_time_now(&unused));
    assert(strcmp(request("OK", false, 0), "OK") == 0);
    assert(!valid());
    /* The only basis without an RTC: an explicit SET. */
    assert(strcmp(request("OK", true, set_at), "OK") == 0);
    assert(valid() && syncs() == 1 && wall_seconds() == set_at);
    /* A refused RESTART (main.c: OTA_BUSY) changes nothing. */
    assert(strcmp(request("OTA_BUSY", false, 0), "OTA_BUSY") == 0);
    assert(valid() && wall_seconds() == set_at);

    /* Resets that keep the RTC domain keep the time, many times over: each
     * accepted boot renews the retention record. */
    for (int i = 0; i < 20; i++) {
        roundie_desktop_clock_advance(HOUR);
        assert(boot(ROUNDIE_DESKTOP_RESET_PANIC));
        assert(syncs() == 1);
        poll();
        assert(boot(ROUNDIE_DESKTOP_RESET_TASK_WDT));
        assert(boot(ROUNDIE_DESKTOP_RESET_SW));
    }
    assert(wall_seconds() == set_at + 20 * 3600);

    /* Resets that may lose the RTC domain, and power loss, never keep it, and
     * a rejected record stays rejected. */
    const roundie_desktop_reset_t lost[] = {ROUNDIE_DESKTOP_RESET_POWER, ROUNDIE_DESKTOP_RESET_BROWNOUT,
                                            ROUNDIE_DESKTOP_RESET_PIN};
    for (size_t i = 0; i < sizeof(lost) / sizeof(lost[0]); i++) {
        assert(strcmp(request("OK", true, set_at), "OK") == 0);
        roundie_desktop_clock_advance(5 * SECOND);
        assert(!boot(lost[i]));
        assert(!boot(ROUNDIE_DESKTOP_RESET_SW));
        assert(!roundie_local_time_now(&unused));
    }

    /* RTC timer drift. ESP-IDF restores the time as boot time + RTC timer, so
     * the restored time carries the timer's error since the last alignment;
     * the five-second poll bounds that. Without any alignment since the SET,
     * wall and timer progress agree by construction and the error is not
     * detected. With alignment, the retention check compares the crystal-
     * based wall clock with the timer and rejects a rate error above 2 %. */
    roundie_desktop_clock_drift(false, 50000);
    assert(strcmp(request("OK", true, set_at), "OK") == 0);
    roundie_desktop_clock_advance(200 * SECOND);
    assert(boot(ROUNDIE_DESKTOP_RESET_SW));
    assert(wall_seconds() == set_at + 210);
    const int32_t rates[] = {5000, 50000};
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        roundie_desktop_clock_drift(false, rates[i]);
        assert(strcmp(request("OK", true, set_at), "OK") == 0);
        for (int step = 0; step < 40; step++) {
            roundie_desktop_clock_advance(5 * SECOND);
            poll();
        }
        assert(wall_seconds() == set_at + 200);
        roundie_desktop_clock_advance(SECOND);
        bool kept = boot(ROUNDIE_DESKTOP_RESET_SW);
        assert(kept == (rates[i] == 5000));
        if (kept) {
            assert(roundie_clock_port_wall_us() - (set_at + 201) * SECOND == SECOND / 200);
        }
    }
    roundie_desktop_clock_drift(false, 0);

    /* The system clock moved without a sync, then stored by the periodic
     * alignment: wall progress no longer matches the RTC timer. */
    assert(strcmp(request("OK", true, set_at), "OK") == 0);
    roundie_desktop_clock_jump_wall(3 * SECOND);
    poll();
    roundie_desktop_clock_advance(10 * SECOND);
    assert(!boot(ROUNDIE_DESKTOP_RESET_SW));

    /* Light sleep: the RTC timer's count of the sleep enters the wall clock;
     * monotonic time is exact. */
    assert(strcmp(request("OK", true, set_at), "OK") == 0);
    roundie_desktop_clock_drift(false, -20000);
    int64_t before_mono = mono_us;
    roundie_desktop_clock_sleep_enter();
    roundie_desktop_clock_advance(100 * SECOND);
    roundie_desktop_clock_sleep_exit();
    assert(mono_us - before_mono == 100 * SECOND);
    assert(wall_seconds() == set_at + 98);
    assert(valid());
    roundie_desktop_clock_drift(false, 0);
    puts("without RTC: OK");
}

static void with_rtc(void)
{
    const int64_t set_at = civil_seconds(2026, 10, 17, 9, 42, 6);
    /* A new PCF85063 has its oscillator-stop flag set: not a basis. */
    power_on(true, SECOND);
    poll();
    assert(!valid());
    assert(strcmp(request("OK", false, 0), "RTC_INVALID") == 0);
    assert(!valid());

    /* SET writes the RTC, checks that it runs (1.2 s), then syncs from it. */
    int64_t before = mono_us;
    assert(strcmp(request("OK", true, set_at), "OK") == 0);
    assert(mono_us - before == 1200000);
    assert(valid() && syncs() == 1 && wall_seconds() == set_at + 1);

    /* Power loss: the battery-backed RTC restores the time. */
    roundie_desktop_clock_advance(HOUR);
    assert(boot(ROUNDIE_DESKTOP_RESET_POWER));
    assert(wall_seconds() == set_at + 1 + 3600);

    /* A bus fault invalidates at the next poll; recovery resyncs. */
    roundie_desktop_clock_rtc_fault(ROUNDIE_DESKTOP_RTC_BUS);
    poll();
    assert(!valid());
    assert(strcmp(request("OK", false, 0), "RTC_READ") == 0);
    assert(boot(ROUNDIE_DESKTOP_RESET_SW) == false);
    roundie_desktop_clock_rtc_fault(ROUNDIE_DESKTOP_RTC_RUNNING);
    poll();
    assert(valid() && syncs() == 1);
    poll();
    assert(syncs() == 1); /* a valid wall clock is not resynced */

    /* A stopped oscillator: the flag invalidates; a SET passes readback but
     * not the progress check; afterwards only a successful SET is a basis. */
    roundie_desktop_clock_rtc_fault(ROUNDIE_DESKTOP_RTC_STOPPED);
    poll();
    assert(!valid());
    assert(strcmp(request("OK", true, set_at), "RTC_VERIFY") == 0);
    roundie_desktop_clock_rtc_fault(ROUNDIE_DESKTOP_RTC_RUNNING);
    assert(strcmp(request("OK", false, 0), "OK") == 0);
    poll();
    assert(!valid());
    assert(strcmp(request("OK", true, set_at), "OK") == 0);
    assert(valid());

    /* A chip three times too fast fails the progress check; 100 ppm passes,
     * and its error reaches the wall clock through the next sync. */
    roundie_desktop_clock_drift(true, 2000000);
    assert(strcmp(request("OK", true, set_at), "RTC_VERIFY") == 0);
    assert(!valid());
    roundie_desktop_clock_drift(true, 100);
    assert(strcmp(request("OK", true, set_at), "OK") == 0);
    roundie_desktop_clock_advance(10000 * SECOND);
    assert(boot(ROUNDIE_DESKTOP_RESET_SW));
    assert(wall_seconds() == set_at + 1 + 10000 + 1);
    roundie_desktop_clock_drift(true, 0);
    puts("with RTC: OK");
}

static void calendar(void)
{
    power_on(false, SECOND);
    /* Leap day and midnight. */
    assert(strcmp(request("OK", true, civil_seconds(2028, 2, 28, 23, 59, 59)), "OK") == 0);
    assert_local(2028, 2, 28, 23, 59, 59);
    assert(local().ms_of_day == 86399000);
    roundie_desktop_clock_advance(SECOND);
    assert_local(2028, 2, 29, 0, 0, 0);
    assert(local().ms_of_day == 0);
    roundie_desktop_clock_advance(DAY);
    assert_local(2028, 3, 1, 0, 0, 0);
    /* Year end, and the end of the RTC range: 2100 is not a leap year. */
    assert(strcmp(request("OK", true, civil_seconds(2026, 12, 31, 23, 59, 59)), "OK") == 0);
    roundie_desktop_clock_advance(SECOND);
    assert_local(2027, 1, 1, 0, 0, 0);
    assert(strcmp(request("OK", true, civil_seconds(2099, 12, 31, 23, 59, 59)), "OK") == 0);
    roundie_desktop_clock_advance(SECOND);
    assert_local(2100, 1, 1, 0, 0, 0);
    roundie_desktop_clock_advance(58 * DAY);
    assert_local(2100, 2, 28, 0, 0, 0);
    roundie_desktop_clock_advance(DAY);
    assert_local(2100, 3, 1, 0, 0, 0);

    /* Daylight saving time changes local fields and nothing else: monotonic
     * time advances by exactly the elapsed second and the revision stays. */
    assert(roundie_local_time_set_zone("CET-1CEST,M3.5.0,M10.5.0/3") == ESP_OK);
    assert(strcmp(request("OK", true, civil_seconds(2026, 3, 29, 0, 59, 59)), "OK") == 0);
    roundie_elf_clock_t winter = local();
    assert(winter.ms_of_day == (1 * 3600 + 59 * 60 + 59) * 1000U);
    roundie_desktop_clock_advance(SECOND);
    roundie_elf_clock_t summer = local();
    assert(summer.ms_of_day == 3 * 3600 * 1000U);
    assert(summer.monotonic_us - winter.monotonic_us == SECOND);
    assert(summer.revision == winter.revision && summer.days == winter.days);
    assert(strcmp(request("OK", true, civil_seconds(2026, 10, 25, 0, 59, 59)), "OK") == 0);
    roundie_elf_clock_t late = local();
    assert(late.ms_of_day == (2 * 3600 + 59 * 60 + 59) * 1000U);
    roundie_desktop_clock_advance(SECOND);
    roundie_elf_clock_t back = local();
    assert(back.ms_of_day == 2 * 3600 * 1000U); /* local time repeats an hour */
    assert(back.monotonic_us - late.monotonic_us == SECOND);
    assert(back.revision == late.revision);

    /* A zone change is a new revision; time itself does not move. */
    assert(roundie_local_time_set_zone("UTC0") == ESP_OK);
    roundie_elf_clock_t utc = local();
    assert(utc.ms_of_day == 1 * 3600 * 1000U && utc.revision == back.revision + 1);
    assert(utc.monotonic_us == back.monotonic_us);
    assert(roundie_local_time_set_zone("bad rule\x01") == ESP_ERR_INVALID_ARG);
    assert(local().revision == utc.revision);

    /* The stored rule is reloaded; a damaged or missing one means UTC0. */
    assert(roundie_local_time_set_zone("JST-9") == ESP_OK);
    roundie_local_time_init(true, roundie_wall_clock_valid);
    char zone[ROUNDIE_TIME_ZONE_CAPACITY];
    roundie_local_time_zone(zone);
    assert(strcmp(zone, "JST-9") == 0);
    FILE *file = fopen(zone_path, "w");
    assert(file != NULL);
    fputs("\x01\x02\n", file);
    fclose(file);
    roundie_local_time_init(true, roundie_wall_clock_valid);
    roundie_local_time_zone(zone);
    assert(strcmp(zone, "UTC0") == 0);
    roundie_local_time_init(false, roundie_wall_clock_valid);
    assert(roundie_local_time_set_zone("JST-9") == ESP_ERR_INVALID_STATE);
    roundie_local_time_init(true, roundie_wall_clock_valid);
    puts("calendar and zones: OK");
}

static void extremes(void)
{
    /* Monotonic time far beyond 32-bit microsecond and millisecond counters. */
    power_on(false, INT64_C(1) << 52);
    const int64_t set_at = civil_seconds(2026, 10, 17, 9, 42, 6);
    assert(strcmp(request("OK", true, set_at), "OK") == 0);
    roundie_wall_clock_status_t status;
    roundie_wall_clock_status(&status);
    assert(status.sync_at_us == INT64_C(1) << 52);
    roundie_desktop_clock_advance(INT64_C(4294967296) * 1000 + SECOND);
    assert(wall_seconds() == set_at + 4294968);
    assert(boot(ROUNDIE_DESKTOP_RESET_SW));
    roundie_elf_clock_t clock = local();
    assert(clock.monotonic_us == mono_us);

    /* A wall clock before 1970 cannot be expressed in days: no local time,
     * though the clock remains trusted (nothing but a SET moves it on the
     * board). Far in the future it still converts. */
    roundie_desktop_clock_jump_wall(-roundie_clock_port_wall_us() - DAY);
    roundie_elf_clock_t unused;
    assert(!roundie_local_time_now(&unused));
    roundie_desktop_clock_jump_wall(DAY + INT64_C(100000) * 365 * DAY);
    clock = local();
    assert(clock.days >= 100000U * 365U);
    puts("extremes: OK");
}

/* main.c serves RT1 from the system worker before the board is published,
 * with board NULL, while start() may run. Such a request must change nothing,
 * so start() cannot overwrite a time the reply acknowledged. */
static void before_publication(bool has_rtc)
{
    power_on(has_rtc, 1000 * SECOND);
    int64_t set_to = civil_seconds(2026, 11, 16, 12, 0, 0);
    assert(strcmp(request("OK", true, set_to), "OK") == 0);
    assert(valid());
    /* A reset that keeps the time: the PCF85063 across power loss, the RTC
     * domain across a software reset without it. */
    roundie_desktop_clock_reset(has_rtc ? ROUNDIE_DESKTOP_RESET_POWER : ROUNDIE_DESKTOP_RESET_SW);
    int64_t wall_before = roundie_clock_port_wall_us();
    roundie_clock_retention_t retained_before = *roundie_clock_port_retained();

    roundie_rtc_sample_t rtc;
    esp_err_t error;
    const char *result = roundie_wall_clock_request(NULL, "OK", true, set_to + 86400, &rtc, &error);
    assert(strcmp(result, "CLOCK") == 0);
    assert(error == ESP_ERR_INVALID_STATE);
    assert(rtc.state == ROUNDIE_SENSOR_NOT_FOUND);
    result = roundie_wall_clock_request(NULL, "OK", false, 0, &rtc, &error);
    assert(strcmp(result, has_rtc ? "RTC_READ" : "OK") == 0);
    assert(error == (has_rtc ? ESP_ERR_NOT_FOUND : ESP_OK));
    result = roundie_wall_clock_request(NULL, "OTA_BUSY", true, set_to + 86400, &rtc, &error);
    assert(strcmp(result, "OTA_BUSY") == 0);
    assert(roundie_clock_port_wall_us() == wall_before);
    assert(memcmp(roundie_clock_port_retained(), &retained_before, sizeof(retained_before)) == 0);

    /* Startup is as if the early requests never happened. */
    assert(start_and_publish());
    assert(wall_seconds() >= set_to && wall_seconds() <= set_to + 2);
    /* After publication a SET runs, and the next poll keeps it. */
    assert(strcmp(request("OK", true, set_to + 86400), "OK") == 0);
    poll();
    assert(valid());
    assert(wall_seconds() >= set_to + 86400 && wall_seconds() <= set_to + 86400 + 2);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(zone_path, sizeof(zone_path), "%s/clock-tz", argv[1]);
    without_rtc();
    with_rtc();
    calendar();
    extremes();
    before_publication(false);
    before_publication(true);
    return 0;
}
