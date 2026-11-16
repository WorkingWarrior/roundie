/* Commands of the virtual hardware lab (lab.h). */

#include "lab.h"

#include <stdio.h>
#include <string.h>
#include "board.h"
#include "clock.h"
#include "vhw.h"

#define KEY_HOLD_DEFAULT_MS 100

static bool word_is(const char *word, const char *expected)
{
    return strcmp(word, expected) == 0;
}

static bool parse_flag(const char *word, bool *value)
{
    if (word_is(word, "0") || word_is(word, "off")) {
        *value = false;
        return true;
    }
    if (word_is(word, "1") || word_is(word, "on")) {
        *value = true;
        return true;
    }
    return false;
}

static int find_name(const char *word, const char *const *names, int count)
{
    for (int i = 0; i < count; i++) {
        if (word_is(word, names[i])) {
            return i;
        }
    }
    return -1;
}

static const char *const touch_names[] = {"none", "timeout", "noise", "absent"};
static const char *const display_names[] = {"none", "restore", "init"};
static const char *const audio_names[] = {"none", "unavailable"};
static const char *const storage_names[] = {"none", "absent", "io"};
/* vhw_storage_call_t: "all", then the wrapped calls. */
static const char *const storage_call_names[] = {"all", "open", "read", "write", "fsync", "mkdir",
                                                 "rmdir", "unlink", "rename", "stat", "opendir"};
static const char *const rtc_names[] = {"none", "stop", "bus", "absent"};

#define COUNT(array) ((int)(sizeof(array) / sizeof((array)[0])))

static lab_result_t battery_command(const char *line, int64_t now)
{
    char what[32] = "";
    long a = -1;
    long b = -1;
    int fields = sscanf(line, "%*s %31s %ld %ld", what, &a, &b);
    if (fields < 2 || a < 0) {
        return LAB_MALFORMED;
    }
    if (word_is(what, "soc") && a <= 100) {
        vhw_set_soc((uint32_t)a, now);
    } else if (word_is(what, "present") && a <= 1) {
        vhw_set_battery_present(a == 1, now);
    } else if (word_is(what, "capacity") && a > 0 && a <= 60000) {
        vhw_set_capacity((uint32_t)a, now);
    } else if (word_is(what, "load") && a <= 100000) {
        vhw_state_t state;
        vhw_get(&state, now);
        uint32_t sleep_ma = state.sleep_load_ma;
        if (fields == 3) {
            if (b < 0 || b > 100000) {
                return LAB_MALFORMED;
            }
            sleep_ma = (uint32_t)b;
        }
        vhw_set_load((uint32_t)a, sleep_ma, now);
    } else if (word_is(what, "charge") && a <= 100000) {
        vhw_set_charge_current((uint32_t)a, now);
    } else {
        return LAB_MALFORMED;
    }
    return LAB_DONE;
}

lab_result_t lab_command(const char *line, roundie_desktop_input_t *input, int64_t now, uint32_t *hold_ms)
{
    char command[32] = "";
    char word[64] = "";
    long value = -1;
    *hold_ms = 0;
    if (sscanf(line, "%31s", command) != 1) {
        return LAB_UNKNOWN;
    }
    bool has_word = sscanf(line, "%*s %63s", word) == 1;
    bool flag = false;
    int index = -1;
    if (word_is(command, "hw")) {
        lab_print_status(now);
    } else if (word_is(command, "battery")) {
        return battery_command(line, now);
    } else if (word_is(command, "usb")) {
        if (!has_word || !parse_flag(word, &flag)) {
            return LAB_MALFORMED;
        }
        vhw_set_usb(flag, now);
    } else if (word_is(command, "gauge-fault")) {
        if (word_is(word, "none")) {
            vhw_set_gauge(VHW_BUS_OK);
        } else if (word_is(word, "timeout")) {
            vhw_set_gauge(VHW_BUS_TIMEOUT);
        } else if (word_is(word, "absent")) {
            vhw_set_gauge(VHW_BUS_ABSENT);
        } else {
            return LAB_MALFORMED;
        }
    } else if (word_is(command, "power-cut")) {
        vhw_power_cut(now);
    } else if (word_is(command, "power-restore")) {
        vhw_power_restore(now);
    } else if (word_is(command, "reboot")) {
        /* The power-on reset comes only from the supply (power-cut). */
        roundie_desktop_reset_t reset = ROUNDIE_DESKTOP_RESET_POWER;
        if (!roundie_desktop_reset_parse(word, &reset) || reset == ROUNDIE_DESKTOP_RESET_POWER) {
            return LAB_MALFORMED;
        }
        vhw_request_reset((int)reset);
    } else if (word_is(command, "rtc-fault")) {
        index = find_name(word, rtc_names, COUNT(rtc_names));
        if (index < 0) {
            return LAB_MALFORMED;
        }
        static const roundie_desktop_rtc_fault_t faults[] = {
            ROUNDIE_DESKTOP_RTC_RUNNING, ROUNDIE_DESKTOP_RTC_STOPPED, ROUNDIE_DESKTOP_RTC_BUS,
            ROUNDIE_DESKTOP_RTC_ABSENT,
        };
        roundie_desktop_clock_rtc_fault(faults[index]);
    } else if (word_is(command, "rtc-backup")) {
        if (!has_word || !parse_flag(word, &flag)) {
            return LAB_MALFORMED;
        }
        vhw_set_rtc_backup(flag);
    } else if (word_is(command, "touch-fault")) {
        index = find_name(word, touch_names, COUNT(touch_names));
        if (index < 0) {
            return LAB_MALFORMED;
        }
        vhw_set_touch((vhw_touch_t)index);
    } else if (word_is(command, "display-fault")) {
        index = find_name(word, display_names, COUNT(display_names));
        if (index < 0) {
            return LAB_MALFORMED;
        }
        vhw_set_display((vhw_display_t)index);
    } else if (word_is(command, "audio-fault")) {
        index = find_name(word, audio_names, COUNT(audio_names));
        if (index < 0) {
            return LAB_MALFORMED;
        }
        vhw_set_audio((vhw_audio_t)index);
    } else if (word_is(command, "audio-underrun")) {
        vhw_request_underrun();
    } else if (word_is(command, "storage-fault")) {
        index = find_name(word, storage_names, COUNT(storage_names));
        char call_word[16] = "all";
        int fields = sscanf(line, "%*s %*s %15s", call_word);
        int call = find_name(call_word, storage_call_names, COUNT(storage_call_names));
        if (index < 0 || call < 0 || (fields == 1 && index != VHW_STORAGE_IO)) {
            return LAB_MALFORMED;
        }
        vhw_set_storage_call((vhw_storage_call_t)call);
        vhw_set_storage((vhw_storage_t)index);
    } else if (word_is(command, "storage-limit")) {
        if (sscanf(line, "%*s %ld", &value) != 1 || value < 0) {
            return LAB_MALFORMED;
        }
        vhw_set_storage_limit((uint64_t)value * 1024U);
    } else if (word_is(command, "key")) {
        long ms = KEY_HOLD_DEFAULT_MS;
        int fields = sscanf(line, "%*s %63s %ld", word, &ms);
        if (fields < 1 || ms <= 0 || ms > 60000) {
            return LAB_MALFORMED;
        }
        if (!roundie_desktop_key_hold(input, word, now + ms * 1000)) {
            return LAB_MALFORMED;
        }
        *hold_ms = (uint32_t)ms;
    } else {
        return LAB_UNKNOWN;
    }
    return LAB_DONE;
}

void lab_print_status(int64_t now)
{
    vhw_state_t s;
    vhw_get(&s, now);
    roundie_battery_sample_t sample;
    vhw_battery_sample(&sample, now);
    printf("hw powered=%d off=%s usb=%d battery=%d soc=%u charge_uah=%u ocv=%u current=%d sample=%d mv=%u"
           " gauge=%s rtc_backup=%d touch=%s display=%s audio=%s storage=%s%s%s limit=%llu cycles=%u sleeping=%d\n",
           (int)s.powered, vhw_off_name(s.off_reason), (int)s.usb, (int)s.battery_present,
           (unsigned)s.soc_percent, (unsigned)s.charge_mah_x1000, (unsigned)s.open_circuit_mv, (int)s.current_ma,
           (int)sample.state, (unsigned)sample.voltage_mv, vhw_bus_name(s.gauge), (int)s.rtc_backup,
           touch_names[s.touch], display_names[s.display], audio_names[s.audio], storage_names[s.storage],
           s.storage_call != VHW_STORAGE_CALL_ALL ? ":" : "",
           s.storage_call != VHW_STORAGE_CALL_ALL ? storage_call_names[s.storage_call] : "",
           (unsigned long long)s.storage_limit_bytes, (unsigned)s.power_cycles, (int)s.sleeping);
    fflush(stdout);
}
