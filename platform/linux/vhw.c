/* Virtual hardware lab model (vhw.h, docs/virtual-hardware.md). */

#include "vhw.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Open-circuit voltage against state of charge: a generic Li-ion curve, an
 * assumption, not the cell fitted to either board. */
static const struct {
    uint32_t soc;
    uint32_t mv;
} ocv_curve[] = {
    {0, 3300}, {10, 3600}, {50, 3750}, {90, 4100}, {100, 4200},
};

/* Internal resistance in milliohms, for the voltage under load. Assumption. */
#define CELL_RESISTANCE_MOHM 150
#define PWR_ON_HOLD_US 1000000
#define PWR_OFF_HOLD_US 2000000
#define MA_US_PER_UAH INT64_C(3600000)

typedef struct {
    vhw_state_t state;
    bool cut;      /* the supply is disconnected (power-cut) */
    bool latch_on; /* the PWR latch connects the battery */
    int64_t residual_ma_us; /* charge below one micro-ampere-hour */
    int64_t updated_us;
    bool pwr_pressed;
    int64_t pwr_since_us;
    bool pwr_handled;
    bool reset_pending;
    int reset_kind;
    vhw_action_t pending;
} machine_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static machine_t machine;
/* A copy of machine.state.storage that the storage wrappers read without the
 * lock: they may run in a signal handler (vhw_storage.c). */
static atomic_int storage_fault;
static atomic_int storage_call;

static uint32_t full_uah(void)
{
    return machine.state.full_mah * 1000U;
}

static uint32_t soc_of(uint32_t charge_uah)
{
    uint32_t full = full_uah();
    if (full == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)charge_uah * 100U + full / 2U) / full);
}

static uint16_t ocv_mv(uint32_t charge_uah)
{
    uint32_t full = full_uah();
    /* State of charge in hundredths of a percent, for a smooth curve. */
    uint32_t centi = full == 0 ? 0 : (uint32_t)((uint64_t)charge_uah * 10000U / full);
    for (size_t i = 1; i < sizeof(ocv_curve) / sizeof(ocv_curve[0]); i++) {
        uint32_t upper = ocv_curve[i].soc * 100U;
        if (centi <= upper) {
            uint32_t lower = ocv_curve[i - 1].soc * 100U;
            uint32_t span = upper - lower;
            return (uint16_t)(ocv_curve[i - 1].mv +
                              (ocv_curve[i].mv - ocv_curve[i - 1].mv) * (centi - lower) / span);
        }
    }
    return (uint16_t)ocv_curve[sizeof(ocv_curve) / sizeof(ocv_curve[0]) - 1].mv;
}

static bool supply_available(void)
{
    const vhw_state_t *s = &machine.state;
    return !machine.cut && (s->usb || (s->battery_present && s->charge_mah_x1000 > 0));
}

static bool should_run(void)
{
    return supply_available() && (machine.state.usb || machine.latch_on);
}

/* The battery current the present conditions draw or supply. */
static int32_t battery_current(void)
{
    const vhw_state_t *s = &machine.state;
    if (!s->battery_present || machine.cut) {
        return 0;
    }
    if (s->usb) {
        /* Assumption: the charger feeds the system; the cell sees the charge
         * current until full, then nothing (no CV taper). */
        return s->charge_mah_x1000 < full_uah() ? (int32_t)s->charge_ma : 0;
    }
    if (!s->powered) {
        return 0;
    }
    return -(int32_t)(s->sleeping ? s->sleep_load_ma : s->load_ma);
}

/* Under lock: charge integrated up to now, derived values, power transitions. */
static void update(int64_t now_us)
{
    vhw_state_t *s = &machine.state;
    int64_t elapsed = now_us - machine.updated_us;
    if (elapsed > 0) {
        machine.updated_us = now_us;
        int32_t current = battery_current();
        machine.residual_ma_us += (int64_t)current * elapsed;
        int64_t whole = machine.residual_ma_us / MA_US_PER_UAH;
        machine.residual_ma_us -= whole * MA_US_PER_UAH;
        int64_t charge = (int64_t)s->charge_mah_x1000 + whole;
        /* Empty or full: the remainder pointing beyond goes too. */
        if (charge < 0 || (charge == 0 && machine.residual_ma_us < 0)) {
            charge = 0;
            machine.residual_ma_us = 0;
        }
        if (charge > (int64_t)full_uah() || (charge == (int64_t)full_uah() && machine.residual_ma_us > 0)) {
            charge = full_uah();
            machine.residual_ma_us = 0;
        }
        s->charge_mah_x1000 = (uint32_t)charge;
    }
    if (s->powered && !should_run()) {
        s->powered = false;
        if (machine.cut || (!s->battery_present && !s->usb)) {
            s->off_reason = VHW_OFF_CUT;
        } else if (!machine.latch_on) {
            s->off_reason = VHW_OFF_LATCH;
        } else {
            s->off_reason = VHW_OFF_DEPLETED;
            /* Assumption: the latch drops with the supply. */
            machine.latch_on = false;
        }
        machine.reset_pending = false;
        machine.pending = VHW_ACTION_POWER_OFF;
    } else if (!s->powered && should_run()) {
        s->powered = true;
        s->off_reason = VHW_OFF_NONE;
        s->sleeping = false;
        /* Assumption: started from USB, the board then stays on without it. */
        machine.latch_on = true;
        s->power_cycles++;
        machine.pending = VHW_ACTION_POWER_ON;
    }
    s->soc_percent = soc_of(s->charge_mah_x1000);
    s->open_circuit_mv = ocv_mv(s->charge_mah_x1000);
    s->current_ma = battery_current();
}

void vhw_init(vhw_chip_t chip, int64_t now_us)
{
    pthread_mutex_lock(&lock);
    memset(&machine, 0, sizeof(machine));
    vhw_state_t *s = &machine.state;
    s->chip = chip;
    s->powered = true;
    s->battery_present = true;
    s->full_mah = 500;
    s->charge_mah_x1000 = 400000;
    s->load_ma = 120;
    s->sleep_load_ma = 10;
    s->charge_ma = 250;
    s->rtc_backup = true;
    machine.latch_on = true;
    machine.updated_us = now_us;
    atomic_store(&storage_fault, (int)VHW_STORAGE_OK);
    atomic_store(&storage_call, (int)VHW_STORAGE_CALL_ALL);
    update(now_us);
    pthread_mutex_unlock(&lock);
}

void vhw_get(vhw_state_t *state, int64_t now_us)
{
    pthread_mutex_lock(&lock);
    update(now_us);
    *state = machine.state;
    pthread_mutex_unlock(&lock);
}

void vhw_set_usb(bool connected, int64_t now_us)
{
    pthread_mutex_lock(&lock);
    update(now_us);
    machine.state.usb = connected;
    update(now_us);
    pthread_mutex_unlock(&lock);
}

void vhw_set_battery_present(bool present, int64_t now_us)
{
    pthread_mutex_lock(&lock);
    update(now_us);
    machine.state.battery_present = present;
    update(now_us);
    pthread_mutex_unlock(&lock);
}

void vhw_set_soc(uint32_t soc_percent, int64_t now_us)
{
    if (soc_percent > 100) {
        soc_percent = 100;
    }
    pthread_mutex_lock(&lock);
    update(now_us);
    machine.state.charge_mah_x1000 = (uint32_t)((uint64_t)full_uah() * soc_percent / 100U);
    machine.residual_ma_us = 0;
    update(now_us);
    pthread_mutex_unlock(&lock);
}

void vhw_set_capacity(uint32_t full_mah, int64_t now_us)
{
    if (full_mah == 0 || full_mah > 60000) {
        return;
    }
    pthread_mutex_lock(&lock);
    update(now_us);
    /* The state of charge stays; the stored charge scales. */
    uint32_t old_full = full_uah();
    uint64_t charge = old_full == 0 ? 0 : (uint64_t)machine.state.charge_mah_x1000 * full_mah * 1000U / old_full;
    machine.state.full_mah = full_mah;
    machine.state.charge_mah_x1000 = (uint32_t)charge;
    machine.residual_ma_us = 0;
    update(now_us);
    pthread_mutex_unlock(&lock);
}

void vhw_set_load(uint32_t awake_ma, uint32_t sleep_ma, int64_t now_us)
{
    pthread_mutex_lock(&lock);
    update(now_us);
    machine.state.load_ma = awake_ma;
    machine.state.sleep_load_ma = sleep_ma;
    update(now_us);
    pthread_mutex_unlock(&lock);
}

void vhw_set_charge_current(uint32_t ma, int64_t now_us)
{
    pthread_mutex_lock(&lock);
    update(now_us);
    machine.state.charge_ma = ma;
    update(now_us);
    pthread_mutex_unlock(&lock);
}

void vhw_set_gauge(vhw_bus_t bus)
{
    pthread_mutex_lock(&lock);
    machine.state.gauge = bus;
    pthread_mutex_unlock(&lock);
}

void vhw_set_rtc_backup(bool kept)
{
    pthread_mutex_lock(&lock);
    machine.state.rtc_backup = kept;
    pthread_mutex_unlock(&lock);
}

void vhw_set_touch(vhw_touch_t fault)
{
    pthread_mutex_lock(&lock);
    machine.state.touch = fault;
    pthread_mutex_unlock(&lock);
}

void vhw_set_display(vhw_display_t fault)
{
    pthread_mutex_lock(&lock);
    machine.state.display = fault;
    pthread_mutex_unlock(&lock);
}

void vhw_set_audio(vhw_audio_t fault)
{
    pthread_mutex_lock(&lock);
    machine.state.audio = fault;
    pthread_mutex_unlock(&lock);
}

void vhw_set_storage(vhw_storage_t fault)
{
    pthread_mutex_lock(&lock);
    machine.state.storage = fault;
    atomic_store(&storage_fault, (int)fault);
    pthread_mutex_unlock(&lock);
}

void vhw_set_storage_call(vhw_storage_call_t call)
{
    pthread_mutex_lock(&lock);
    machine.state.storage_call = call;
    atomic_store(&storage_call, (int)call);
    pthread_mutex_unlock(&lock);
}

vhw_storage_t vhw_storage_fault(void)
{
    return (vhw_storage_t)atomic_load(&storage_fault);
}

bool vhw_storage_call_fails(vhw_storage_call_t call)
{
    if (atomic_load(&storage_fault) != (int)VHW_STORAGE_IO) {
        return false;
    }
    int selected = atomic_load(&storage_call);
    return selected == (int)VHW_STORAGE_CALL_ALL || selected == (int)call;
}

void vhw_set_storage_limit(uint64_t bytes)
{
    pthread_mutex_lock(&lock);
    machine.state.storage_limit_bytes = bytes;
    pthread_mutex_unlock(&lock);
}

void vhw_request_underrun(void)
{
    pthread_mutex_lock(&lock);
    machine.state.audio_underruns++;
    pthread_mutex_unlock(&lock);
}

bool vhw_take_underrun(void)
{
    pthread_mutex_lock(&lock);
    bool take = machine.state.audio_underruns > 0;
    if (take) {
        machine.state.audio_underruns--;
    }
    pthread_mutex_unlock(&lock);
    return take;
}

void vhw_power_cut(int64_t now_us)
{
    pthread_mutex_lock(&lock);
    update(now_us);
    machine.cut = true;
    update(now_us);
    pthread_mutex_unlock(&lock);
}

void vhw_power_restore(int64_t now_us)
{
    pthread_mutex_lock(&lock);
    update(now_us);
    machine.cut = false;
    /* Assumption: reconnecting the supply starts the board. */
    machine.latch_on = true;
    update(now_us);
    pthread_mutex_unlock(&lock);
}

void vhw_request_reset(int kind)
{
    pthread_mutex_lock(&lock);
    if (machine.state.powered) {
        machine.reset_pending = true;
        machine.reset_kind = kind;
    }
    pthread_mutex_unlock(&lock);
}

void vhw_set_pwr(bool pressed, int64_t now_us)
{
    pthread_mutex_lock(&lock);
    if (pressed && !machine.pwr_pressed) {
        machine.pwr_since_us = now_us;
        machine.pwr_handled = false;
    }
    machine.pwr_pressed = pressed;
    pthread_mutex_unlock(&lock);
}

void vhw_set_sleeping(bool sleeping, int64_t now_us)
{
    pthread_mutex_lock(&lock);
    update(now_us);
    machine.state.sleeping = sleeping && machine.state.powered;
    update(now_us);
    pthread_mutex_unlock(&lock);
}

vhw_action_t vhw_poll(int64_t now_us, int *reset_kind)
{
    pthread_mutex_lock(&lock);
    update(now_us);
    vhw_state_t *s = &machine.state;
    if (machine.pwr_pressed && !machine.pwr_handled) {
        int64_t held = now_us - machine.pwr_since_us;
        if (s->powered && !s->usb && held >= PWR_OFF_HOLD_US) {
            machine.latch_on = false;
            machine.pwr_handled = true;
        } else if (!s->powered && held >= PWR_ON_HOLD_US) {
            machine.latch_on = true;
            machine.pwr_handled = true;
        }
        update(now_us);
    }
    vhw_action_t action = machine.pending;
    machine.pending = VHW_ACTION_NONE;
    if (action == VHW_ACTION_NONE && machine.reset_pending && s->powered) {
        machine.reset_pending = false;
        action = VHW_ACTION_RESET;
        *reset_kind = machine.reset_kind;
    }
    pthread_mutex_unlock(&lock);
    return action;
}

/* The 1.85B's BQ27220 as its driver reports it: battery flags 0x0008 present,
 * 0x0001 discharging; operation flags 0x0020 initialized. */
static void gauge_sample(const vhw_state_t *s, roundie_battery_sample_t *sample)
{
    sample->battery_flags = s->battery_present ? 0x0008 : 0;
    sample->battery_present = s->battery_present;
    if (!s->battery_present) {
        sample->state = ROUNDIE_SENSOR_BATTERY_ABSENT;
        sample->error = ESP_OK;
        return;
    }
    if (s->current_ma < 0) {
        sample->battery_flags |= 0x0001;
    }
    sample->operation_flags = 0x0020;
    sample->initialized = true;
    sample->discharging = s->current_ma < 0;
    sample->state = ROUNDIE_SENSOR_VALID;
    sample->error = ESP_OK;
    int32_t loaded = (int32_t)s->open_circuit_mv + s->current_ma * CELL_RESISTANCE_MOHM / 1000;
    sample->voltage_mv = (uint16_t)loaded;
    sample->voltage_valid = true;
    sample->full_mah = (uint16_t)s->full_mah;
    sample->full_valid = true;
    sample->remaining_mah = (uint16_t)(s->charge_mah_x1000 / 1000U);
    sample->remaining_valid = true;
    sample->soc_percent = (uint16_t)s->soc_percent;
    sample->soc_valid = true;
    sample->current_ma = (int16_t)s->current_ma;
    sample->current_valid = true;
    sample->temperature_deci_c = 250;
    sample->temperature_valid = true;
}

/* The 1.75C's AXP2101 as its driver reports it: presence, direction, voltage. */
static void pmic_sample(const vhw_state_t *s, roundie_battery_sample_t *sample)
{
    sample->battery_present = s->battery_present;
    if (!s->battery_present) {
        sample->state = ROUNDIE_SENSOR_BATTERY_ABSENT;
        sample->error = ESP_OK;
        return;
    }
    sample->discharging = s->current_ma < 0;
    sample->state = ROUNDIE_SENSOR_UNCALIBRATED;
    sample->error = ESP_OK;
    int32_t loaded = (int32_t)s->open_circuit_mv + s->current_ma * CELL_RESISTANCE_MOHM / 1000;
    sample->voltage_mv = (uint16_t)loaded;
    sample->voltage_valid = true;
}

void vhw_battery_sample(roundie_battery_sample_t *sample, int64_t now_us)
{
    memset(sample, 0, sizeof(*sample));
    sample->observed_us = now_us;
    pthread_mutex_lock(&lock);
    update(now_us);
    vhw_state_t s = machine.state;
    pthread_mutex_unlock(&lock);
    if (s.gauge == VHW_BUS_ABSENT) {
        sample->state = ROUNDIE_SENSOR_NOT_FOUND;
        sample->error = ESP_ERR_NOT_FOUND;
        return;
    }
    if (s.gauge == VHW_BUS_TIMEOUT) {
        sample->state = ROUNDIE_SENSOR_COMM_ERROR;
        sample->error = ESP_ERR_TIMEOUT;
        return;
    }
    if (s.chip == VHW_CHIP_GAUGE) {
        gauge_sample(&s, sample);
    } else {
        pmic_sample(&s, sample);
    }
}

/* ---- Persistence across a desktop restart ---- */

void vhw_save(FILE *file, int64_t now_us)
{
    pthread_mutex_lock(&lock);
    update(now_us);
    const vhw_state_t *s = &machine.state;
    fprintf(file, "vhw.powered %d\nvhw.off %d\nvhw.usb %d\nvhw.present %d\nvhw.full %u\nvhw.charge %u\n",
            (int)s->powered, (int)s->off_reason, (int)s->usb, (int)s->battery_present, (unsigned)s->full_mah,
            (unsigned)s->charge_mah_x1000);
    fprintf(file, "vhw.load %u\nvhw.sleep_load %u\nvhw.charge_ma %u\nvhw.rtc_backup %d\nvhw.gauge %d\n",
            (unsigned)s->load_ma, (unsigned)s->sleep_load_ma, (unsigned)s->charge_ma, (int)s->rtc_backup,
            (int)s->gauge);
    fprintf(file, "vhw.touch %d\nvhw.display %d\nvhw.audio %d\nvhw.storage %d\nvhw.storage_call %d\n"
            "vhw.storage_limit %llu\n", (int)s->touch, (int)s->display, (int)s->audio, (int)s->storage,
            (int)s->storage_call, (unsigned long long)s->storage_limit_bytes);
    fprintf(file, "vhw.cut %d\nvhw.latch %d\nvhw.cycles %u\n", (int)machine.cut, (int)machine.latch_on,
            (unsigned)s->power_cycles);
    pthread_mutex_unlock(&lock);
}

bool vhw_load_line(const char *key, const char *value, int64_t now_us)
{
    if (strncmp(key, "vhw.", 4) != 0) {
        return false;
    }
    key += 4;
    char *end = NULL;
    unsigned long long number = strtoull(value, &end, 10);
    if (end == value) {
        return false;
    }
    pthread_mutex_lock(&lock);
    vhw_state_t *s = &machine.state;
    bool known = true;
    if (strcmp(key, "powered") == 0) {
        s->powered = number != 0;
    } else if (strcmp(key, "off") == 0 && number <= VHW_OFF_LATCH) {
        s->off_reason = (vhw_off_t)number;
    } else if (strcmp(key, "usb") == 0) {
        s->usb = number != 0;
    } else if (strcmp(key, "present") == 0) {
        s->battery_present = number != 0;
    } else if (strcmp(key, "full") == 0 && number > 0 && number <= 60000) {
        s->full_mah = (uint32_t)number;
    } else if (strcmp(key, "charge") == 0 && number <= 60000000ULL) {
        s->charge_mah_x1000 = (uint32_t)number;
    } else if (strcmp(key, "load") == 0 && number <= 100000) {
        s->load_ma = (uint32_t)number;
    } else if (strcmp(key, "sleep_load") == 0 && number <= 100000) {
        s->sleep_load_ma = (uint32_t)number;
    } else if (strcmp(key, "charge_ma") == 0 && number <= 100000) {
        s->charge_ma = (uint32_t)number;
    } else if (strcmp(key, "rtc_backup") == 0) {
        s->rtc_backup = number != 0;
    } else if (strcmp(key, "gauge") == 0 && number <= VHW_BUS_ABSENT) {
        s->gauge = (vhw_bus_t)number;
    } else if (strcmp(key, "touch") == 0 && number <= VHW_TOUCH_ABSENT) {
        s->touch = (vhw_touch_t)number;
    } else if (strcmp(key, "display") == 0 && number <= VHW_DISPLAY_INIT) {
        s->display = (vhw_display_t)number;
    } else if (strcmp(key, "audio") == 0 && number <= VHW_AUDIO_UNAVAILABLE) {
        s->audio = (vhw_audio_t)number;
    } else if (strcmp(key, "storage") == 0 && number <= VHW_STORAGE_IO) {
        s->storage = (vhw_storage_t)number;
        atomic_store(&storage_fault, (int)number);
    } else if (strcmp(key, "storage_call") == 0 && number <= VHW_STORAGE_CALL_OPENDIR) {
        s->storage_call = (vhw_storage_call_t)number;
        atomic_store(&storage_call, (int)number);
    } else if (strcmp(key, "storage_limit") == 0) {
        s->storage_limit_bytes = number;
    } else if (strcmp(key, "cut") == 0) {
        machine.cut = number != 0;
    } else if (strcmp(key, "latch") == 0) {
        machine.latch_on = number != 0;
    } else if (strcmp(key, "cycles") == 0) {
        s->power_cycles = (uint32_t)number;
    } else {
        known = false;
    }
    if (s->charge_mah_x1000 > full_uah()) {
        s->charge_mah_x1000 = full_uah();
    }
    machine.updated_us = now_us;
    machine.pending = VHW_ACTION_NONE;
    s->sleeping = false;
    pthread_mutex_unlock(&lock);
    return known;
}

const char *vhw_bus_name(vhw_bus_t bus)
{
    static const char *const names[] = {"none", "timeout", "absent"};
    return names[bus];
}

const char *vhw_off_name(vhw_off_t reason)
{
    static const char *const names[] = {"none", "cut", "depleted", "latch"};
    return names[reason];
}
