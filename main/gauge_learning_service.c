#include "gauge_learning_service.h"

#if defined(CONFIG_ROUNDIE_GAUGE_DESIGN_CAPACITY_MAH)

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "power_manager.h"
#include "roundie_gauge_learning.h"

static const char *TAG = "gauge_learn";

#define NVS_NAMESPACE "gauge"
#define NVS_KEY "learned"
#define HOLD_KINDS 0x3U /* CPU_AWAKE | DISPLAY_AWAKE */
#define HOLD_TIMEOUT_S 120U
#define HOLD_RENEW_US INT64_C(60000000)

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
    apply_hold(roundie_gauge_hold_update(&hold, &sample, result, now_us), now_us);
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

static bool inhibitor_held(void)
{
#if CONFIG_ROUNDIE_POWER
    return hold_handle != 0;
#else
    return false;
#endif
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
#endif
#endif
