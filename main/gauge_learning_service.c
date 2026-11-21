#include "gauge_learning_service.h"

#if defined(CONFIG_ROUNDIE_GAUGE_DESIGN_CAPACITY_MAH)

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "power_manager.h"
#include "roundie_gauge_learning.h"
#if CONFIG_ROUNDIE_DEV_POWER
#include "driver/usb_serial_jtag.h"
#endif

static const char *TAG = "gauge_learn";

#define NVS_NAMESPACE "gauge"
#define NVS_KEY "learned"
#define HOLD_KINDS 0x3U /* CPU_AWAKE | DISPLAY_AWAKE */
#define HOLD_TIMEOUT_S 120U
#define HOLD_RENEW_US INT64_C(60000000)
/* After the end of a hold: light sleep this long between samples. */
#define PROTECT_SLEEP_S 60U

static roundie_gauge_learning_t tracker;
static roundie_gauge_learning_config_t config = {.learning_low_temp_deci_c = 119, .overload_ma = 1500};
static roundie_gauge_learning_sample_t last_sample;
static bool profile_read;
static roundie_gauge_hold_t hold;
#if CONFIG_ROUNDIE_POWER
static uint32_t hold_handle;
static int64_t hold_renewed_us;
#endif
static roundie_gauge_hold_state_t hold_logged;
static roundie_gauge_alert_t alert_shown;
static bool snapshot_reported;
#if CONFIG_ROUNDIE_POWER
static uint32_t protect_requests;
static uint32_t protect_accepted;
#endif
#if CONFIG_ROUNDIE_DEV_POWER
/* The connection monitor is linked only with these options (IDF
 * esp_driver_usb_serial_jtag CMakeLists). */
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED) || defined(CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION)
#define USB_MONITOR 1
#else
#define USB_MONITOR 0
#endif
#define LOG_REPLY_ENTRIES 10U
static roundie_gauge_log_t telemetry;
static uint32_t sleeps_seen;
#endif

/* The stored record as loaded (read once, never erased) and saves since boot. */
static bool record_loaded;
static esp_err_t record_error = ESP_ERR_INVALID_STATE;
static roundie_gauge_record_t record;
static size_t record_size;
#if CONFIG_ROUNDIE_GAUGE_DESIGN_CAPACITY_MAH > 0 || CONFIG_ROUNDIE_DEV_POWER
static uint32_t record_saves;
#endif

static void load_record(void)
{
    if (record_loaded) {
        return;
    }
    record_loaded = true;
    record_size = 0;
    nvs_handle_t handle;
    record_error = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (record_error != ESP_OK) {
        return;
    }
    size_t size = sizeof(record);
    record_error = nvs_get_blob(handle, NVS_KEY, &record, &size);
    nvs_close(handle);
    if (record_error == ESP_OK) {
        record_size = size;
    }
    /* A malformed record stays in NVS; it is ignored and replaced by the
     * next valid save, never erased here. */
}

static void sample_from_battery(const roundie_battery_sample_t *battery, roundie_gauge_learning_sample_t *sample)
{
    memset(sample, 0, sizeof(*sample));
    sample->valid = battery->error == ESP_OK && battery->battery_present && battery->voltage_valid &&
                    battery->current_valid && battery->temperature_valid && battery->remaining_valid &&
                    battery->full_valid && battery->design_valid;
    sample->initialized = battery->initialized;
    sample->time_us = battery->observed_us;
    sample->operation_flags = battery->operation_flags;
    sample->battery_flags = battery->battery_flags;
    sample->voltage_mv = battery->voltage_mv;
    sample->current_ma = battery->current_ma;
    if (battery->average_current_valid) {
        sample->average_current_ma = battery->average_current_ma;
    }
    sample->temperature_deci_c = battery->temperature_deci_c;
    sample->remaining_mah = battery->remaining_mah;
    sample->full_mah = battery->full_mah;
    sample->design_mah = battery->design_mah;
}

static void read_profile(roundie_board_t *board)
{
    roundie_gauge_capacity_t capacity;
    esp_err_t error = roundie_board_gauge_capacity_read(board, &capacity);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Profile read: %s, step %u; TRM defaults used", esp_err_to_name(error), capacity.step);
        return;
    }
    config.learning_low_temp_deci_c = capacity.learning_low_temp_deci_c;
    config.overload_ma = capacity.overload_ma;
    profile_read = true;
    ESP_LOGI(TAG, "Profile: DC %u, DM FCC %u, cfg 0x%04x, near full %u, battery low %u, low temp %u, "
             "overload %u, taper %u, reserve %u, flags A 0x%04x B 0x%02x",
             capacity.design_mah, capacity.dm_full_mah, capacity.gauging_config, capacity.near_full_mah,
             capacity.battery_low_centi, capacity.learning_low_temp_deci_c, capacity.overload_ma,
             capacity.taper_current_ma, capacity.reserve_mah, capacity.flag_config_a, capacity.flag_config_b);
}

#if CONFIG_ROUNDIE_GAUGE_DESIGN_CAPACITY_MAH > 0
/* After an FCC update: keep the gauge's Learned FCC (Data Memory) when it
 * belongs to the configured capacity and differs enough from the record. */
static void save_learned(roundie_board_t *board)
{
    const uint16_t design = CONFIG_ROUNDIE_GAUGE_DESIGN_CAPACITY_MAH;
    roundie_gauge_capacity_t capacity;
    esp_err_t error = roundie_board_gauge_capacity_read(board, &capacity);
    if (error != ESP_OK || capacity.design_mah != design || capacity.dm_design_mah != design) {
        ESP_LOGW(TAG, "Learned FCC not saved: %s, DC %u", esp_err_to_name(error), capacity.design_mah);
        return;
    }
    load_record();
    const uint32_t profile = roundie_gauge_profile_id(&capacity);
    const roundie_gauge_record_t *stored = record_size > 0 ? &record : NULL;
    if (!roundie_gauge_record_save_due(stored, record_size, design, profile, capacity.dm_full_mah, record_saves)) {
        ESP_LOGI(TAG, "Learned FCC %u mAh: no save (stored %u, saves %" PRIu32 ")", capacity.dm_full_mah,
                 stored != NULL ? record.learned_full_mah : 0U, record_saves);
        return;
    }
    const bool previous = roundie_gauge_record_valid(stored, record_size, design, profile);
    roundie_gauge_record_t next;
    roundie_gauge_record_make(&next, design, profile, capacity.dm_full_mah, previous ? record.sequence + 1U : 1U);
    nvs_handle_t handle;
    error = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, NVS_KEY, &next, sizeof(next));
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    /* Counted even when it failed: the limit bounds attempts, not successes. */
    record_saves++;
    if (error == ESP_OK) {
        record = next;
        record_size = sizeof(next);
        record_error = ESP_OK;
    }
    ESP_LOGW(TAG, "Learned FCC %u mAh saved (sequence %" PRIu32 "): %s", next.learned_full_mah, next.sequence,
             esp_err_to_name(error));
}
#endif

static void apply_hold(bool wanted, int64_t now_us)
{
#if CONFIG_ROUNDIE_POWER
    if (wanted && (hold_handle == 0 || now_us - hold_renewed_us >= HOLD_RENEW_US)) {
        if (!roundie_power_diagnostics_hold(&hold_handle, HOLD_KINDS, HOLD_TIMEOUT_S)) {
            ESP_LOGE(TAG, "Learning load hold: no inhibitor slot");
        }
        hold_renewed_us = now_us;
    } else if (!wanted && hold_handle != 0) {
        (void)roundie_power_diagnostics_hold(&hold_handle, 0, 0);
    }
#else
    (void)wanted;
    (void)now_us;
#endif
    if (hold.state != hold_logged) {
        ESP_LOGW(TAG, "Learning load hold %s", roundie_gauge_hold_state_name(hold.state));
        hold_logged = hold.state;
    }
}

/* The end of a hold: the alert on the device (independent of a USB host) and
 * the protective light sleep, both through the PowerManager. */
static void protect_end(bool after_wake)
{
    if (hold.alert != alert_shown) {
        ESP_LOGW(TAG, "Battery test alert %s (hold %s, learned %u)", roundie_gauge_alert_name(hold.alert),
                 roundie_gauge_hold_state_name(hold.state), hold.learned ? 1U : 0U);
        alert_shown = hold.alert;
#if CONFIG_ROUNDIE_POWER
        roundie_power_battery_alert((uint8_t)hold.alert);
#endif
    }
    if (!hold.edv2_seen) {
        snapshot_reported = false; /* re-armed */
    } else if (!snapshot_reported) {
        snapshot_reported = true;
        ESP_LOGW(TAG, "EDV2 snapshot: %u mV %d mA (avg %d) %" PRId32 " dC RM %u FCC %u op 0x%04x flags 0x%04x",
                 hold.edv2.voltage_mv, hold.edv2.current_ma, hold.edv2.average_current_ma,
                 hold.edv2.temperature_deci_c, hold.edv2.remaining_mah, hold.edv2.full_mah,
                 hold.edv2.operation_flags, hold.edv2.battery_flags);
    }
#if CONFIG_ROUNDIE_POWER
    /* Not in the sample taken right after a wake: one more sample period
     * awake lets a reconnected USB host enumerate and be seen. */
    if (roundie_gauge_hold_sleep_wanted(&hold) && !after_wake) {
        protect_requests++;
        if (roundie_power_request_timed_sleep(PROTECT_SLEEP_S)) {
            protect_accepted++;
        }
    }
#else
    (void)after_wake;
#endif
}

#if CONFIG_ROUNDIE_DEV_POWER
static bool inhibitor_held(void)
{
#if CONFIG_ROUNDIE_POWER
    return hold_handle != 0;
#else
    return false;
#endif
}

#endif

void roundie_gauge_service_sample(roundie_board_t *board, const roundie_battery_sample_t *battery,
                                  int64_t now_us)
{
    roundie_gauge_learning_sample_t sample;
    sample_from_battery(battery, &sample);
    if (sample.valid && sample.initialized && !profile_read) {
        read_profile(board);
    }
    const roundie_gauge_learn_phase_t phase = tracker.phase;
    roundie_gauge_learn_result_t result = roundie_gauge_learning_update(&tracker, &config, &sample);
    if (sample.valid) {
        last_sample = sample;
    }
    if (tracker.phase != phase) {
        ESP_LOGI(TAG, "Learning %s -> %s (RM %u, FCC %u, %u mV, %d mA)", roundie_gauge_learn_phase_name(phase),
                 roundie_gauge_learn_phase_name(tracker.phase), sample.remaining_mah, sample.full_mah,
                 sample.voltage_mv, sample.current_ma);
    }
    if (result != ROUNDIE_GAUGE_LEARN_RESULT_NONE) {
        ESP_LOGW(TAG, "Learning %s, cause %s: FCC %u -> %u; EDV2 %u mV %d mA %" PRId32 " dC",
                 roundie_gauge_learn_result_name(result), roundie_gauge_learn_cause_name(tracker.cause),
                 tracker.result_old_full_mah, tracker.result_new_full_mah, tracker.edv2_voltage_mv,
                 tracker.edv2_current_ma, tracker.edv2_temperature_deci_c);
#if CONFIG_ROUNDIE_GAUGE_DESIGN_CAPACITY_MAH > 0
        if (result == ROUNDIE_GAUGE_LEARN_RESULT_UPDATED) {
            save_learned(board);
        }
#endif
    }
    if (sample.valid && !sample.initialized) {
        /* A gauge reset: its thresholds are read again once it initialized. */
        profile_read = false;
    }
#if CONFIG_ROUNDIE_DEV_POWER
#if USB_MONITOR
    const bool usb_connected = usb_serial_jtag_is_connected();
#else
    const bool usb_connected = false;
#endif
    /* The worker samples right after a light sleep returns, so this entry's
     * gauge currents mostly cover the sleep (docs/bringup.md). */
    const uint32_t sleeps = roundie_power_light_sleeps();
    const bool after_wake = sleeps != sleeps_seen;
    sleeps_seen = sleeps;
#else
    const bool usb_connected = false;
    const bool after_wake = false;
#endif
    apply_hold(roundie_gauge_hold_update(&hold, &sample, result, now_us), now_us);
    if (usb_connected) {
        roundie_gauge_hold_external_power(&hold);
    }
    protect_end(after_wake);
#if CONFIG_ROUNDIE_DEV_POWER
    sample.time_us = now_us;
    roundie_gauge_log_push(&telemetry, &sample, hold.state, usb_connected, inhibitor_held(), after_wake);
#endif
}

uint16_t roundie_gauge_service_restore_full(const roundie_gauge_capacity_t *before, uint16_t design_mah)
{
    load_record();
    const uint32_t profile = roundie_gauge_profile_id(before);
    const roundie_gauge_record_t *stored = record_size > 0 ? &record : NULL;
    uint16_t full = roundie_gauge_record_restore_full(stored, record_size, design_mah, profile);
    ESP_LOGW(TAG, "Restore FCC %u mAh (%s record, load %s)", full,
             roundie_gauge_record_valid(stored, record_size, design_mah, profile) ? "valid" : "no valid",
             esp_err_to_name(record_error));
    return full;
}

#if CONFIG_ROUNDIE_DEV_POWER
esp_err_t roundie_gauge_service_hold(uint32_t minutes, int64_t now_us)
{
#if CONFIG_ROUNDIE_POWER
    if (minutes == 0) {
        roundie_gauge_hold_cancel(&hold);
        apply_hold(false, now_us);
        return ESP_OK;
    }
    if (minutes > 1440 || !roundie_gauge_hold_arm(&hold, now_us, (int64_t)minutes * 60 * 1000000)) {
        return ESP_ERR_INVALID_ARG;
    }
    apply_hold(true, now_us);
    return hold_handle != 0 ? ESP_OK : ESP_ERR_NO_MEM;
#else
    (void)minutes;
    (void)now_us;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t roundie_gauge_service_test(roundie_gauge_test_t test, uint32_t delay_s, int64_t now_us)
{
    if (!roundie_gauge_hold_test(&hold, test, now_us, (int64_t)delay_s * 1000000)) {
        return hold.state == ROUNDIE_GAUGE_HOLD_RUNNING ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "Simulated end test %s in %" PRIu32 " s (warning), end 60 s later; nothing is written",
             test == ROUNDIE_GAUGE_TEST_END ? "END" : "STOP", delay_s);
    return ESP_OK;
}

int roundie_gauge_service_json(char *buffer, size_t size, int64_t now_us)
{
    load_record();
    const int64_t discharge_average = tracker.discharge_samples > 0
                                          ? tracker.discharge_sum_ma / (int64_t)tracker.discharge_samples
                                          : 0;
    const int64_t hold_left = hold.state == ROUNDIE_GAUGE_HOLD_RUNNING && hold.deadline_us > now_us
                                  ? (hold.deadline_us - now_us) / 1000000
                                  : 0;
    const bool stored = record_size == sizeof(record);
    return snprintf(buffer, size,
        "{\"learn\":1,\"phase\":\"%s\",\"resumed\":%u,\"result\":\"%s\",\"cause\":\"%s\",\"old_full\":%u,"
        "\"new_full\":%u,\"result_s\":%" PRId64 ",\"qualified\":%" PRIu32 ",\"updated\":%" PRIu32
        ",\"unchanged\":%" PRIu32 ",\"aborted\":%" PRIu32 ",\"start_rm\":%u,\"start_full\":%u,"
        "\"min_temp\":%" PRId32 ",\"edv2\":%u,\"edv2_qualified\":%u,\"edv2_mv\":%u,\"edv2_ma\":%d,"
        "\"edv2_temp\":%" PRId32 ",\"edv2_rm\":%u,\"dsg_samples\":%" PRIu32 ",\"dsg_avg_ma\":%" PRId64
        ",\"dsg_small_ma\":%d,\"dsg_large_ma\":%d,\"need_ma\":%u,\"hold\":\"%s\",\"hold_s\":%" PRId64
        ",\"inhibitor\":%u,\"stored\":%u,\"stored_full\":%u,\"stored_seq\":%" PRIu32 ",\"saves\":%" PRIu32
        ",\"load\":%d,\"low_temp\":%" PRId32 ",\"overload\":%u,\"op\":%u,\"flags\":%u,\"mv\":%u,\"ma\":%d,"
        "\"temp\":%" PRId32 ",\"rm\":%u,\"fcc\":%u,\"dc\":%u}",
        roundie_gauge_learn_phase_name(tracker.phase), tracker.resumed ? 1U : 0U,
        roundie_gauge_learn_result_name(tracker.result), roundie_gauge_learn_cause_name(tracker.cause),
        tracker.result_old_full_mah, tracker.result_new_full_mah,
        tracker.result == ROUNDIE_GAUGE_LEARN_RESULT_NONE ? INT64_C(-1) : (now_us - tracker.result_us) / 1000000,
        tracker.qualified_count, tracker.updated_count, tracker.unchanged_count, tracker.aborted_count,
        tracker.start_remaining_mah, tracker.start_full_mah, tracker.min_temperature_deci_c,
        tracker.edv2_seen ? 1U : 0U, tracker.edv2_qualified ? 1U : 0U, tracker.edv2_voltage_mv,
        tracker.edv2_current_ma, tracker.edv2_temperature_deci_c, tracker.edv2_remaining_mah,
        tracker.discharge_samples, discharge_average, tracker.discharge_smallest_ma,
        tracker.discharge_largest_ma, roundie_gauge_learning_min_current_ma(last_sample.design_mah),
        roundie_gauge_hold_state_name(hold.state), hold_left, inhibitor_held() ? 1U : 0U,
        stored ? 1U : 0U, stored ? record.learned_full_mah : 0U, stored ? record.sequence : 0U, record_saves,
        (int)record_error, config.learning_low_temp_deci_c, config.overload_ma, last_sample.operation_flags,
        last_sample.battery_flags, last_sample.voltage_mv, last_sample.current_ma,
        last_sample.temperature_deci_c, last_sample.remaining_mah, last_sample.full_mah,
        last_sample.design_mah);
}

static int64_t seconds_since(int64_t then_us, int64_t now_us)
{
    return then_us > 0 && now_us >= then_us ? (now_us - then_us) / 1000000 : INT64_C(-1);
}

int roundie_gauge_service_end_json(char *buffer, size_t size, int64_t now_us)
{
    const roundie_gauge_learning_sample_t *snap = &hold.edv2;
    return snprintf(buffer, size,
        "{\"end\":1,\"hold\":\"%s\",\"alert\":\"%s\",\"learned\":%u,\"hold_result\":\"%s\","
        "\"protect\":%u,\"warned\":%u,\"warned_s\":%" PRId64 ",\"ended_s\":%" PRId64 ",\"sleep_req\":%" PRIu32
        ",\"sleep_ok\":%" PRIu32 ",\"snap\":%u,\"snap_s\":%" PRId64 ",\"snap_mv\":%u,\"snap_ma\":%d,"
        "\"snap_avg_ma\":%d,\"snap_temp\":%" PRId32 ",\"snap_rm\":%u,\"snap_fcc\":%u,\"snap_op\":%u,"
        "\"snap_flags\":%u,\"avg_ma\":%d,\"simulated\":%u,\"test\":%u}",
        roundie_gauge_hold_state_name(hold.state), roundie_gauge_alert_name(hold.alert), hold.learned ? 1U : 0U,
        roundie_gauge_learn_result_name(hold.result), hold.protect ? 1U : 0U, hold.warned ? 1U : 0U,
        hold.warned ? seconds_since(hold.warned_us, now_us) : INT64_C(-1), seconds_since(hold.ended_us, now_us),
        protect_requests, protect_accepted, hold.edv2_seen ? 1U : 0U,
        hold.edv2_seen ? seconds_since(snap->time_us, now_us) : INT64_C(-1), snap->voltage_mv, snap->current_ma,
        snap->average_current_ma, snap->temperature_deci_c, snap->remaining_mah, snap->full_mah,
        snap->operation_flags, snap->battery_flags, last_sample.average_current_ma, hold.simulated ? 1U : 0U,
        (unsigned)hold.test);
}

int roundie_gauge_service_log_json(char *buffer, size_t size, uint32_t from)
{
    uint32_t first = roundie_gauge_log_first(&telemetry);
    uint32_t sequence = from > first ? from : first;
    int length = snprintf(buffer, size, "{\"log\":1,\"usb_monitor\":%u,\"total\":%" PRIu32 ",\"first\":%" PRIu32
                          ",\"e\":[", USB_MONITOR ? 1U : 0U, telemetry.total, first);
    if (length < 0 || (size_t)length >= size) {
        return -1;
    }
    roundie_gauge_log_entry_t entry;
    for (uint32_t count = 0; count < LOG_REPLY_ENTRIES && roundie_gauge_log_get(&telemetry, sequence, &entry);
         count++) {
        int written = snprintf(buffer + length, size - (size_t)length,
                               "%s[%" PRIu32 ",%" PRIu32 ",%u,%d,%d,%u,%u,%u,%u,%u,%u,%d]", count > 0 ? "," : "",
                               sequence, entry.time_ms, entry.voltage_mv, entry.current_ma,
                               entry.temperature_deci_c, entry.remaining_mah, entry.full_mah,
                               entry.operation_flags, entry.battery_flags, entry.hold_state, entry.flags,
                               entry.average_current_ma);
        if (written < 0 || (size_t)written >= size - (size_t)length) {
            return -1;
        }
        length += written;
        sequence++;
    }
    int written = snprintf(buffer + length, size - (size_t)length, "],\"next\":%" PRIu32 "}", sequence);
    if (written < 0 || (size_t)written >= size - (size_t)length) {
        return -1;
    }
    return length + written;
}
#endif
#endif
