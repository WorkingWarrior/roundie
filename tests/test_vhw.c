/* Host test of the desktop's virtual hardware model (platform/linux/vhw.c). */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "vhw.h"

#define S(seconds) ((int64_t)(seconds) * 1000000)

static vhw_state_t get(int64_t now)
{
    vhw_state_t state;
    vhw_get(&state, now);
    return state;
}

static void discharge_and_charge(void)
{
    int64_t now = S(1);
    vhw_init(VHW_CHIP_GAUGE, now);
    vhw_set_capacity(100, now);
    vhw_set_soc(10, now);
    vhw_set_load(100, 10, now);
    int kind = -1;
    assert(vhw_poll(now, &kind) == VHW_ACTION_NONE);
    /* 10 mAh at 100 mA: 6 minutes. */
    now += S(179);
    assert(vhw_poll(now, &kind) == VHW_ACTION_NONE);
    vhw_state_t s = get(now);
    assert(s.soc_percent == 5);
    assert(s.current_ma == -100);
    roundie_battery_sample_t sample;
    vhw_battery_sample(&sample, now);
    assert(sample.state == ROUNDIE_SENSOR_VALID);
    assert(sample.soc_valid && sample.soc_percent == 5);
    assert(sample.discharging && (sample.battery_flags & 0x0009) == 0x0009);
    assert(sample.current_valid && sample.current_ma == -100);
    assert(sample.voltage_valid && sample.voltage_mv < s.open_circuit_mv);
    /* Light sleep draws less. */
    vhw_set_sleeping(true, now);
    now += S(60);
    s = get(now);
    assert(s.current_ma == -10);
    vhw_set_sleeping(false, now);
    now += S(200);
    assert(vhw_poll(now, &kind) == VHW_ACTION_POWER_OFF);
    s = get(now);
    assert(!s.powered && s.off_reason == VHW_OFF_DEPLETED && s.soc_percent == 0);
    assert(vhw_poll(now, &kind) == VHW_ACTION_NONE);
    /* No draw while off. */
    now += S(3600);
    assert(get(now).charge_mah_x1000 == 0);
    /* USB starts the board and charges the cell. */
    vhw_set_usb(true, now);
    assert(vhw_poll(now, &kind) == VHW_ACTION_POWER_ON);
    vhw_set_charge_current(100, now);
    /* Many short steps from empty, as the desktop polls: nothing is lost. */
    for (int i = 0; i < 3600; i++) {
        now += 10000;
        assert(vhw_poll(now, &kind) == VHW_ACTION_NONE);
    }
    s = get(now);
    assert(s.powered && s.current_ma == 100 && s.charge_mah_x1000 == 1000);
    vhw_battery_sample(&sample, now);
    assert(!sample.discharging && sample.current_ma == 100);
    /* Full: no more current. */
    now += S(7200);
    s = get(now);
    assert(s.soc_percent == 100 && s.current_ma == 0);
    /* Unplugged: the latch keeps it on battery. */
    vhw_set_usb(false, now);
    assert(vhw_poll(now, &kind) == VHW_ACTION_NONE);
    assert(get(now).powered);
}

static void latch_and_cut(void)
{
    int64_t now = S(1);
    int kind = -1;
    vhw_init(VHW_CHIP_GAUGE, now);
    /* A short press does nothing; 2 s on battery switches off. */
    vhw_set_pwr(true, now);
    now += S(1);
    assert(vhw_poll(now, &kind) == VHW_ACTION_NONE);
    now += S(1);
    assert(vhw_poll(now, &kind) == VHW_ACTION_POWER_OFF);
    assert(get(now).off_reason == VHW_OFF_LATCH);
    vhw_set_pwr(false, now);
    /* 1 s switches on again. */
    vhw_set_pwr(true, now);
    now += S(1) - 1;
    assert(vhw_poll(now, &kind) == VHW_ACTION_NONE);
    now += 1;
    assert(vhw_poll(now, &kind) == VHW_ACTION_POWER_ON);
    vhw_set_pwr(false, now);
    /* With USB the latch cannot switch it off. */
    vhw_set_usb(true, now);
    vhw_set_pwr(true, now);
    now += S(5);
    assert(vhw_poll(now, &kind) == VHW_ACTION_NONE);
    vhw_set_pwr(false, now);
    /* Power cut and restore. */
    vhw_power_cut(now);
    assert(vhw_poll(now, &kind) == VHW_ACTION_POWER_OFF);
    assert(get(now).off_reason == VHW_OFF_CUT);
    vhw_request_reset(1);
    vhw_power_restore(now);
    assert(vhw_poll(now, &kind) == VHW_ACTION_POWER_ON);
    /* A reset requested while off is dropped. */
    assert(vhw_poll(now, &kind) == VHW_ACTION_NONE);
    vhw_request_reset(2);
    assert(vhw_poll(now, &kind) == VHW_ACTION_RESET && kind == 2);
    /* Battery removed without USB: off. */
    vhw_set_usb(false, now);
    vhw_set_battery_present(false, now);
    assert(vhw_poll(now, &kind) == VHW_ACTION_POWER_OFF);
    assert(get(now).off_reason == VHW_OFF_CUT);
}

static void samples(void)
{
    int64_t now = S(1);
    roundie_battery_sample_t sample;
    vhw_init(VHW_CHIP_PMIC, now);
    vhw_battery_sample(&sample, now);
    assert(sample.state == ROUNDIE_SENSOR_UNCALIBRATED && !sample.soc_valid && sample.voltage_valid);
    vhw_set_gauge(VHW_BUS_TIMEOUT);
    vhw_battery_sample(&sample, now);
    assert(sample.state == ROUNDIE_SENSOR_COMM_ERROR && sample.error == ESP_ERR_TIMEOUT);
    vhw_set_gauge(VHW_BUS_ABSENT);
    vhw_battery_sample(&sample, now);
    assert(sample.state == ROUNDIE_SENSOR_NOT_FOUND);
    vhw_init(VHW_CHIP_GAUGE, now);
    vhw_set_usb(true, now);
    vhw_set_battery_present(false, now);
    vhw_battery_sample(&sample, now);
    assert(sample.state == ROUNDIE_SENSOR_BATTERY_ABSENT && !sample.battery_present);
    assert(get(now).powered);
    /* Voltage follows the curve. */
    vhw_set_battery_present(true, now);
    vhw_set_usb(false, now);
    vhw_set_load(0, 0, now);
    vhw_set_soc(50, now);
    assert(get(now).open_circuit_mv == 3750);
    vhw_set_soc(100, now);
    assert(get(now).open_circuit_mv == 4200);
    vhw_set_soc(0, now);
    int kind;
    assert(vhw_poll(now, &kind) == VHW_ACTION_POWER_OFF);
}

static void persistence(void)
{
    int64_t now = S(10);
    vhw_init(VHW_CHIP_GAUGE, now);
    vhw_set_soc(37, now);
    vhw_set_usb(true, now);
    vhw_set_touch(VHW_TOUCH_NOISE);
    vhw_set_storage_limit(65536);
    vhw_set_storage_call(VHW_STORAGE_CALL_RENAME);
    vhw_set_storage(VHW_STORAGE_IO);
    assert(vhw_storage_call_fails(VHW_STORAGE_CALL_RENAME) && !vhw_storage_call_fails(VHW_STORAGE_CALL_WRITE));
    char buffer[2048];
    FILE *file = fmemopen(buffer, sizeof(buffer), "w");
    vhw_save(file, now);
    fclose(file);
    vhw_init(VHW_CHIP_GAUGE, 0);
    file = fmemopen(buffer, strlen(buffer), "r");
    char key[64];
    char value[64];
    while (fscanf(file, "%63s %63s", key, value) == 2) {
        assert(vhw_load_line(key, value, S(1)));
    }
    fclose(file);
    vhw_state_t s = get(S(1));
    assert(s.soc_percent == 37 && s.usb && s.touch == VHW_TOUCH_NOISE && s.storage_limit_bytes == 65536);
    assert(s.storage == VHW_STORAGE_IO && s.storage_call == VHW_STORAGE_CALL_RENAME);
    assert(vhw_storage_call_fails(VHW_STORAGE_CALL_RENAME) && !vhw_storage_call_fails(VHW_STORAGE_CALL_OPEN));
    vhw_set_storage_call(VHW_STORAGE_CALL_ALL);
    assert(vhw_storage_call_fails(VHW_STORAGE_CALL_OPEN));
    vhw_set_storage(VHW_STORAGE_OK);
    assert(!vhw_storage_call_fails(VHW_STORAGE_CALL_OPEN));
    int kind;
    assert(vhw_poll(S(1), &kind) == VHW_ACTION_NONE);
    assert(!vhw_load_line("other", "1", 0));
}

/* --reader: another thread samples the machine as the desktop's worker does. */
static atomic_bool reading = true;

static void *reader(void *argument)
{
    (void)argument;
    uint32_t samples = 0;
    while (atomic_load(&reading)) {
        roundie_battery_sample_t sample;
        vhw_battery_sample(&sample, 0);
        vhw_state_t state;
        vhw_get(&state, 0);
        samples++;
    }
    return (void *)(uintptr_t)samples;
}

int main(int argc, char **argv)
{
    pthread_t thread;
    bool threaded = argc > 1 && strcmp(argv[1], "--reader") == 0;
    if (threaded) {
        assert(pthread_create(&thread, NULL, reader, NULL) == 0);
    }
    discharge_and_charge();
    latch_and_cut();
    samples();
    persistence();
    if (threaded) {
        atomic_store(&reading, false);
        pthread_join(thread, NULL);
    }
    puts("Virtual hardware model: battery, charger, PWR latch, power cut, samples, persistence PASS");
    return 0;
}
